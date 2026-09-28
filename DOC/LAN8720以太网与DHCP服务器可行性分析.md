# LAN8720 网卡 + DHCP 服务器 可行性分析报告

> 分析日期:2026-09-27
> 目标芯片:ESP32-WROOM-32
> 开发环境:ESP-IDF v6.0.2(SDK 路径 `D:\savefile\espidf_pack2\v6.0.2`)
> 分析结论:**支持。当前环境已具备全部软硬件条件,可把 LAN8720 挂载为 ESP32 的以太网卡并作为 DHCP 服务器为接入设备分配 IP。**

---

## 一、结论摘要

| 项目 | 结论 |
| --- | --- |
| LAN8720 挂载为网卡 | ✅ 支持(ESP32 内置 EMAC + lan87xx 官方 PHY 驱动) |
| 作为 DHCP 服务器 | ✅ 支持(lwIP 已开启 `CONFIG_LWIP_DHCPS=y`) |
| 硬件接线 | ✅ 与 ESP32 固定 RMII 引脚完全一致,无需修改 |
| 依赖组件 | ✅ `espressif__lan87xx` v1.1.0 已就位,满足 `idf >= 6.0` 要求 |
| 待确认项 | ⚠️ 仅剩 2 个硬件级参数:RMII 时钟模式(是否板载 25MHz 晶振)、PHY 地址 |

---

## 二、环境核查结果

### 2.1 SDK 与芯片

- SDK 目录 `D:\savefile\espidf_pack2\v6.0.2\esp-idf` 存在,版本 **v6.0.2**。
- `dependencies.lock` 中确认:`idf version: 6.0.2`、`target: esp32`。

### 2.2 以太网 PHY 驱动(lan87xx)

- 组件位于 `managed_components\espressif__lan87xx`,版本 **1.1.0**。
- 来源于 Espressif 官方 `esp-eth-drivers` 仓库,`targets` 明确包含 **esp32**。
- 组件要求 `idf: '>=6.0'`,当前 6.0.2 满足。
- 驱动明确支持 **LAN8720A**(`LAN8720A_MODEL_NUM 0x0F`),并提供标准接口 `esp_eth_phy_new_lan87xx()`。
- `main\idf_component.yml` 已声明依赖 `espressif/lan87xx: '*'`,`dependencies.lock` 已锁定。

### 2.3 以太网 MAC 驱动(ESP32 内置 EMAC)

- SDK 内置组件 `esp_eth` 提供 `esp_eth_mac_new_esp32()`,用于 ESP32 内部 10/100M EMAC。
- `sdkconfig` 中已开启:
  - `CONFIG_ETH_ENABLED=y`
  - `CONFIG_ETH_USE_ESP32_EMAC=y`
- SDK 头文件 `components/esp_eth/include/esp_eth_mac_esp.h` 提供 `ETH_ESP32_EMAC_DEFAULT_CONFIG()` 默认配置(见 4.1)。

### 2.4 DHCP 服务器支持

- `sdkconfig` 中已开启 lwIP DHCP 服务器:
  - `CONFIG_LWIP_DHCPS=y`
  - `CONFIG_LWIP_DHCPS_STATIC_ENTRIES=y`(支持静态绑定)
- `esp_netif` 提供服务器开关接口:`esp_netif_dhcps_start()` / `esp_netif_dhcps_stop()`。
- 以太网与网络栈胶合接口:`esp_eth_driver_install()`、`esp_eth_new_netif_glue()`、`esp_eth_start()` 均存在。

---

## 三、硬件接线核对

ESP32 的 RMII 数据面引脚是**固定**的(不能经 GPIO 矩阵任意映射)。逐条比对:

| LAN8720 引脚 | 说明文档接线 | ESP32 固定 RMII 引脚 | 是否一致 |
| --- | --- | --- | --- |
| TX0 (TXD0) | GPIO19 | EMAC_TXD0 = GPIO19 | ✅ |
| TX_EN | GPIO21 | EMAC_TX_EN = GPIO21 | ✅ |
| TX1 (TXD1) | GPIO22 | EMAC_TXD1 = GPIO22 | ✅ |
| RX0 (RXD0) | GPIO25 | EMAC_RXD0 = GPIO25 | ✅ |
| RX1 (RXD1) | GPIO26 | EMAC_RXD1 = GPIO26 | ✅ |
| CRS (CRS_DV) | GPIO27 | EMAC_CRS_DV = GPIO27 | ✅ |
| MDC | GPIO23 | SMI_MDC = GPIO23 | ✅ |
| MDIO | GPIO18 | SMI_MDIO = GPIO18 | ✅ |
| nINT / REFCLK | GPIO0 | EMAC_CLK = GPIO0 | ✅ |

**9 根信号全部一致,接线无需改动。** 这正好就是 ESP32 官方 Ethernet-Kit 参考设计使用的 RMII 引脚布局。

---

## 四、关键配置点(必须确认/选择的硬件参数)

### 4.1 RMII 参考时钟(50MHz)—— 最关键的确认项

IDF 6.0.2 中,RMII 时钟模式**不在 menuconfig 里配置**,而是代码级配置(结构体 `eth_esp32_emac_config_t.clock_config.rmii`,枚举 `emac_rmii_clock_mode_t`):

- `EMAC_CLK_EXT_IN`:ESP32 从外部**接收** 50MHz 参考时钟(输入模式)。
- `EMAC_CLK_OUT`:ESP32 由内部 APLL **输出** 50MHz 参考时钟(输出模式)。

SDK 对 ESP32 的默认配置 `ETH_ESP32_EMAC_DEFAULT_CONFIG()` 为:

```
SMI: MDC=23, MDIO=18
接口: RMII
时钟: clock_mode = EMAC_CLK_EXT_IN, clock_gpio = 0
```

即**默认就是「GPIO0 输入 50MHz」**,与「GPIO0 → nINT/REFCLK」的接线完全吻合。

因此存在两种可行方案,取决于 LAN8720 模块是否**板载 25MHz 晶振**:

| 方案 | 前提 | 时钟流向 | 代码配置 |
| --- | --- | --- | --- |
| A(推荐,默认) | 模块板载 25MHz 晶振 | LAN8720 经 nINT/REFCLKO 输出 50MHz → ESP32 GPIO0 输入 | `EMAC_CLK_EXT_IN` + `clock_gpio=0`(默认值) |
| B | 模块无晶振,由 ESP32 供时钟 | ESP32 GPIO0 输出 50MHz → LAN8720 时钟输入 | `EMAC_CLK_OUT` + `clock_gpio=0` |

> ⚠️ 方案 B 需注意 ESP32 Errata(见 SDK 头文件 `esp_eth_mac_esp.h` 内注释):
> "If you want the Ethernet to work with Wi-Fi or BT, don't select ESP32 as RMII CLK output as it would result in clock instability."
> 即:若未来需与 Wi-Fi/BT 共存,不建议由 ESP32 输出时钟。本需求为纯以太网 DHCP 服务器,无 Wi-Fi/BT,方案 A/B 均可行,但**优先确认是否板载晶振以决定方案**。

### 4.2 PHY 地址

LAN8720A 由 `PHYAD0` 引脚上下拉决定,默认 **地址 0**。绝大多数模块为 0,需按实际硬件确认(个别板为 1)。

### 4.3 复位引脚(nRST)

说明文档未给出复位线接法。`lan87xx` 驱动的 `eth_phy_config_t.reset_gpio_num` 支持:
- 若模块有独立 nRST 引脚:接到空闲 GPIO 并配置 `reset_gpio_num = <GPIO>`。
- 若模块已将 nRST 上拉(不复位):置 `-1` 即可(驱动默认)。

---

## 五、实现路径(仅步骤,不写代码)

1. **初始化网络接口**:用 `esp_netif_new()` + 以太网 netif 配置创建 `esp_netif`,然后 `esp_netif_attach()`。
2. **创建 MAC**:`esp_eth_mac_new_esp32(ETH_ESP32_EMAC_DEFAULT_CONFIG(), ...)`(默认已含 MDC23/MDIO18/RMII/GPIO0 时钟输入)。
3. **创建 PHY**:`esp_eth_phy_new_lan87xx(&phy_config)`,设置 `phy_addr`、`reset_gpio_num`。
4. **安装驱动**:`esp_eth_driver_install()` → `esp_eth_new_netif_glue()` → `esp_netif_attach()` → `esp_eth_start()`。
5. **注册事件回调**:处理 `ETHERNET_EVENT_START / STOP / CONNECTED / DISCONNECTED`。
6. **配置静态 IP 并启动 DHCP 服务器**:
   - `esp_netif_set_ip_info()` 设置本机静态 IP(如 `192.168.4.1`,掩码 `255.255.255.0`)。
   - 调用 `esp_netif_dhcps_start()` 启动 DHCP 服务器,接入设备即可自动获得 IP。
   - (可选)用 `esp_netif_dhcps_option()` 配置地址池起止、网关、租期等。

---

## 六、风险与注意事项

1. **RMII 时钟模式是首要确认点**:直接决定方案 A / B。建议先确认 LAN8720 模块是否板载 25MHz 晶振(绝大多数通用 LAN8720 转接板都带晶振,走方案 A 默认配置即可)。
2. **PHY 地址错误会导致无法初始化**:默认按 0 尝试,失败再试 1。
3. **复位引脚悬空**可能造成 PHY 不工作:确认模块 nRST 是上拉还是需 GPIO 控制。
4. **事件回调必须处理**:`ETHERNET_EVENT_CONNECTED/DISCONNECTED` 里需 `esp_netif_dhcps_start/stop`(或至少保证 link up 后再对外服务),否则链路断开后 DHCP 状态异常。
5. **DHCP 服务器依赖本机静态 IP**:必须先给以太网 netif 配置静态 IP,再启动 DHCP 服务器。

---

## 七、参考文件

- `managed_components\espressif__lan87xx\include\esp_eth_phy_lan87xx.h` —— PHY 接口
- `D:\savefile\espidf_pack2\v6.0.2\esp-idf\components\esp_eth\include\esp_eth_mac_esp.h` —— EMAC 时钟模式与默认配置
- `D:\savefile\espidf_pack2\v6.0.2\esp-idf\components\esp_eth\include\esp_eth_driver.h` / `esp_eth_netif_glue.h` —— 驱动与 netif 胶合
- `D:\savefile\espidf_pack2\v6.0.2\esp-idf\components\esp_netif\include\esp_netif.h` —— DHCP 服务器接口
- `sdkconfig` —— `CONFIG_ETH_USE_ESP32_EMAC=y`、`CONFIG_LWIP_DHCPS=y`
