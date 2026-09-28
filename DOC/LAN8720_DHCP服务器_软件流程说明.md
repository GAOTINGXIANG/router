# LAN8720 作为 DHCP 服务器 —— 软件流程说明

> 目标芯片:ESP32-WROOM-32　|　开发环境:ESP-IDF v6.0.2
> 网络拓扑:ESP32 + LAN8720(以太网口)作为 DHCP 服务器,为接入的以太网设备分配 IP。

---

## 一、需求与已确认的硬件参数

| 项 | 值 | 说明 |
| --- | --- | --- |
| RMII 参考时钟 | **输入模式**(`EMAC_CLK_EXT_IN`,GPIO0) | LAN8720 模块板载 25MHz 晶振,由 LAN8720 经 nINT/REFCLKO 输出 50MHz 给 ESP32 GPIO0 |
| PHY 地址 | **0** | PHYAD0 下拉 |
| 复位引脚 nRST | **-1(不使用 GPIO 复位)** | nRST 内部上拉,`reset_gpio_num = -1` |
| 数据接口 | RMII(100M/10M 自适应) | 引脚见可行性分析报告 |
| 本机静态 IP | 192.168.4.1 / 255.255.255.0 | DHCP 服务器自身必须使用静态 IP |
| DHCP 地址池 | 192.168.4.2 起,同子网顺序分配 | 最多 `CONFIG_LWIP_DHCPS_MAX_STATION_NUM`(默认 8)台 |

---

## 二、总体软件架构

```
┌─────────────────────────────────────────────────────┐
│                      应用层 app_main                 │
│   (事件回调:链路状态 / IP 事件 / DHCP 客户端管理)      │
└───────────────────────┬─────────────────────────────┘
                        │ esp_event 事件循环
┌───────────────────────▼─────────────────────────────┐
│            esp_netif (网络接口抽象 + DHCP 服务器)      │
│   esp_netif_set_ip_info / esp_netif_dhcps_start      │
└───────────────────────┬─────────────────────────────┘
                        │ lwIP 协议栈 (CONFIG_LWIP_DHCPS=y)
┌───────────────────────▼─────────────────────────────┐
│          esp_eth 以太网驱动 (esp_eth_driver)          │
│   ┌───────────────┐      ┌──────────────────────┐   │
│   │ MAC: esp32 EMAC│◄────►│ PHY: lan87xx 驱动     │   │
│   │  (RMII 接口)   │  SMI │  (LAN8720, MDIO 管理) │   │
│   └───────┬───────┘      └──────────┬───────────┘   │
└───────────┼─────────────────────────┼───────────────┘
            │ RMII 数据(GPIO19/21/22/25/26/27)│ MDC/MDIO(GPIO23/18)
   ┌────────▼─────────────────────────▼───────────────┐
   │              LAN8720 PHY(板载 25MHz 晶振)          │
   │            ↔ RJ45 ↔ 接入设备(PC / 其他以太网设备)   │
   └───────────────────────────────────────────────────┘
```

- **esp_netif**:负责 IP 配置与 DHCP 服务器启停。
- **esp_eth**:负责以太网 MAC + PHY 的初始化、收发与链路状态检测。
- **事件循环**:把以太网"链路通/断"等事件回调给应用层。

---

## 三、初始化流程(app_main 启动步骤)

按顺序执行以下步骤:

```
app_main()
  │
  1. nvs_flash_init()                     —— 初始化非易失存储(存储 MAC/配置)
  │
  2. esp_netif_init()                     —— 初始化 TCP/IP 网络栈
  │    esp_event_loop_create_default()     —— 创建默认事件循环
  │
  3. esp_netif_new(&ESP_NETIF_DEFAULT_ETH())   —— 创建以太网 netif,保存句柄 eth_netif
  │
  4. esp_eth_mac_new_esp32(&emac_cfg, &ETH_MAC_DEFAULT_CONFIG())
  │        emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG()
  │        (默认: MDC=23, MDIO=18, RMII, 时钟输入 GPIO0)
  │
  5. esp_eth_phy_new_lan87xx(&phy_cfg)
  │        phy_cfg = ETH_PHY_DEFAULT_CONFIG()
  │        phy_cfg.phy_addr        = 0
  │        phy_cfg.reset_gpio_num  = -1
  │
  6. esp_eth_driver_install(&ETH_DEFAULT_CONFIG(mac, phy), &eth_handle)
  │        (默认 check_link_period_ms = 2000,即每 2s 检测一次链路)
  │
  7. esp_eth_new_netif_glue(eth_handle)     —— 创建 eth↔netif 胶合层
  │    esp_netif_attach(eth_netif, glue)     —— 把 netif 挂到以太网驱动
  │
  8. esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL)
  │    esp_event_handler_register(IP_EVENT,  IP_EVENT_ETH_GOT_IP, got_ip_handler, NULL)  (可选)
  │
  9. esp_eth_start(eth_handle)              —— 启动以太网(复位/初始化 PHY,MAC 开始收发)
  │
 10. 配置静态 IP + 启动 DHCP 服务器(见第四节)
  ▼
 进入事件循环,等待接入设备
```

关键点:
- 步骤 4 的 `ETH_ESP32_EMAC_DEFAULT_CONFIG()` 已内置"MDC=23、MDIO=18、RMII、时钟输入 GPIO0",与硬件完全一致,**无需修改**。
- 步骤 5 只需补 `phy_addr = 0`、`reset_gpio_num = -1` 两项。
- 步骤 10 必须**先停 DHCP 客户端、设静态 IP、再启 DHCP 服务器**(见下)。

---

## 四、DHCP 服务器配置流程

以太网 netif 默认是 DHCP 客户端模式,作为服务器需显式切换:

```
10.1  esp_netif_dhcps_stop(eth_netif)        —— 停止 DHCP 服务器(未启动,防御性调用)
10.2  esp_netif_dhcpc_stop(eth_netif)        —— 停止 DHCP 客户端(关闭"向外获取 IP")
10.3  esp_netif_set_ip_info(eth_netif, &ip)
          ip.ip      = 192.168.4.1
          ip.netmask = 255.255.255.0
          ip.gw      = 192.168.4.1
10.4  (可选) esp_netif_dhcps_option()         —— 配置租期/起始地址等
10.5  esp_netif_dhcps_start(eth_netif)        —— 启动 DHCP 服务器
```

- 本机网关 `gw` 建议设为本机 IP(或与分配网关一致),接入设备会以此作为默认网关。
- 若需自定义地址池/租期,用 `esp_netif_dhcps_option(eth_netif, ESP_NETIF_OP_SET, <OPTION_ID>, ...)`,
  例如 `ESP_NETIF_IP_ADDRESS_LEASE_TIME`(租期)、`ESP_NETIF_SUBNET_MASK`、`ESP_NETIF_DOMAIN_NAME_SERVER`(下发给客户端的 DNS)。
- 查询已接入客户端:`esp_netif_dhcps_get_clients_by_mac()`。

---

## 五、运行时流程(链路状态 + DHCP 交互)

### 5.1 以太网链路状态事件处理

```
eth_event_handler(event_base, event_id)
  │
  ├─ ETHERNET_EVENT_START         → 打印本机 MAC;可打印"以太网已启动"
  │
  ├─ ETHERNET_EVENT_CONNECTED     → 网线已插好、PHY 链路建立(Link Up)
  │                                 确保 DHCP 服务器在运行(dhcps_start)
  │
  ├─ ETHERNET_EVENT_DISCONNECTED  → 网线拔出 / 链路断开(Link Down)
  │                                 可选:停止 DHCP 服务器(dhcps_stop)
  │
  └─ ETHERNET_EVENT_STOP          → 以太网已停止(清理资源)
```

> 说明:`ETHERNET_EVENT_CONNECTED/DISCONNECTED` 由驱动周期性链路检测(默认 2s)产生,
> 应用层据此决定是否对外提供 DHCP 服务。

### 5.2 DHCP 地址分配时序(接入设备首次上电)

```
接入设备                          ESP32(DHCP 服务器)
   │                                    │
   │── 广播 DHCPDISCOVER ──────────────►│ 发现服务器
   │◄─ 单播 DHCPOFFER(建议 192.168.4.2)──│ 从地址池挑一个可用 IP
   │── 广播 DHCPREQUEST(确认接受) ─────►│ 记录该 IP 已租出
   │◄─ 单播 DHCPACK(确认 + 掩码/网关/DNS)─│ 完成分配,记录租约
   │                                    │
   │   设备使用 192.168.4.2 通信 ◄──────►│ 定时续租 / 回收过期租约
```

- 整个握手由 **lwIP DHCP 服务器**(`CONFIG_LWIP_DHCPS=y`)自动完成,应用层无需干预。
- 地址从 `192.168.4.2` 开始顺序分配;租约到期未续租则回收,可分配给新设备。
- 服务器本机 IP(192.168.4.1)同时作为接入设备的默认网关与 DNS。

---

## 六、关键配置参数汇总(代码侧)

| 结构体 / 宏 | 需显式设置的字段 | 值 |
| --- | --- | --- |
| `ETH_ESP32_EMAC_DEFAULT_CONFIG()` | 无需修改 | MDC=23,MDIO=18,RMII,`EMAC_CLK_EXT_IN`+GPIO0 |
| `ETH_MAC_DEFAULT_CONFIG()` | 无需修改 | 默认 |
| `ETH_PHY_DEFAULT_CONFIG()` | `phy_addr` / `reset_gpio_num` | `0` / `-1` |
| `ETH_DEFAULT_CONFIG(mac, phy)` | 无需修改 | `check_link_period_ms=2000` |
| netif 静态 IP | `ip` / `netmask` / `gw` | `192.168.4.1` / `255.255.255.0` / `192.168.4.1` |
| DHCP 服务器 | `esp_netif_dhcps_start()` | 启用 |

`menuconfig(sdkconfig)` 侧已具备、无需改动:
- `CONFIG_ETH_USE_ESP32_EMAC=y`、`CONFIG_ETH_ENABLED=y`
- `CONFIG_LWIP_DHCPS=y`、`CONFIG_LWIP_DHCPS_STATIC_ENTRIES=y`
- `CONFIG_LWIP_DHCPS_MAX_STATION_NUM=8`(最大接入设备数,可按需调大)

---

## 七、代码文件规划(遵循项目框架)

按项目目录约定(`CODE/inc` 放头文件、`CODE/src` 放源文件),建议拆分:

| 文件 | 职责 |
| --- | --- |
| `CODE/inc/ethernet.h` | 以太网+DHCPS 初始化、事件处理函数声明 |
| `CODE/src/ethernet.c` | netif/MAC/PHY/驱动创建、静态 IP、DHCP 服务器启停、事件回调 |
| `main/main.c` | `app_main()` 调用 `nvs_flash_init`、`esp_netif_init`、`ethernet_init()` 等入口 |

> 实际落地时,`main/CMakeLists.txt` 需把 `CODE/src` 的源文件与 `CODE/inc` 的头文件路径加入编译
> (或在 `main` 组件中引用),此步骤在编写代码阶段处理。

---

## 八、注意事项

1. **先设静态 IP 再启动 DHCP 服务器**,并关闭 DHCP 客户端(`dhcpc_stop`),避免本机也去获取 IP 造成冲突。
2. **链路断开时**建议同步 `dhcps_stop`,链路恢复(`ETHERNET_EVENT_CONNECTED`)再 `dhcps_start`,保证状态干净。
3. **接入设备需设"自动获取 IP"**(DHCP 客户端),才能从 ESP32 拿到地址。
4. 若接入设备需上外网,需在 ESP32 侧做 NAT/路由转发(本需求仅"分配 IP",不涉及)。
5. `CONFIG_LWIP_DHCPS_MAX_STATION_NUM` 决定可同时接入的客户端上限,默认 8,可按需增大。
6. ESP32 默认 `ETH_DEFAULT_CONFIG` 每 2s 检测一次链路,网线插拔后事件回调有一定延迟,属正常。
