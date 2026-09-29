# NAT 网关实现方案(修正版)

> 目标:Wi-Fi STA 作上行 WAN(DHCP client),以太网(LAN8720)作下行 LAN(DHCP server),经 lwIP NAPT 让有线设备共享 Wi-Fi 上网
> 环境:ESP-IDF v6.0.2
> 状态:✅ 方案已落地并修正(NAPT 需开在 LAN 网卡,见第二节)
> 本方案**取代**早期《NAT网关代码落地方案.md》——那份把 NAPT 开在了 WiFi 上,方向是错的。

---

## 一、总体架构

```
有线设备 ──以太网──► [LAN8720] ──► lwIP 转发(IP_FORWARD) ──► NAPT 改写 ──► [WiFi STA] ──► 热点 ──► 互联网
                     (下行 LAN, 静态 192.168.4.1,  DHCP server)          (上行 WAN, DHCP client)
```

| 角色 | 网卡 | IP | 职责 |
| --- | --- | --- | --- |
| WAN 上行 | WiFi STA | 上游 DHCP 分配(如 10.199.148.159) | 接热点、默认路由 |
| LAN 下行 | 以太网 LAN8720 | 静态 192.168.4.1/24 | DHCP server、**NAPT 标志所在** |

---

## 二、核心原理:NAPT 标志开在哪张网卡(关键!)

lwIP 的 NAPT 语义**反直觉**,是全案最容易出错的一点:

- `netif->napt` 标志标记的是**内网侧(LAN/以太网)网卡**,不是外网(WAN)网卡。
- 出站改写时,源地址自动改成**出口网卡的 IP**(即 WAN 的 IP),不需要写死。

源码依据(`components/lwip/lwip/src/core/ipv4/ip4_napt.c:869`):

```c
ip_napt_forward(struct pbuf *p, struct ip_hdr *iphdr,
                struct netif *inp, struct netif *outp)
{
    if (!inp->napt)          // 入包网卡(以太网)带 napt 标志才改写
        return ERR_OK;       // 否则直接跳过,不做 SNAT
    ...
    ip_napt_modify_addr(iphdr, &iphdr->src,
                        ip_2_ip4(&outp->ip_addr)->addr);  // 源 = 出口网卡(WiFi)IP
}
```

lwIP 两个挂钩点(`ip4.c`):

```c
// ① 转发路径 ip4_forward:出口网卡不是 NAPT 网卡才做 SNAT
if (!netif->napt)
    ip_napt_forward(p, iphdr, inp, netif);

// ② 收包路径 ip4_input:入包网卡不是 NAPT 网卡、且目的=本机 WAN 地址时做 DNAT 还原
if (!inp->napt && ip4_addr_cmp(&iphdr->dest, netif_ip4_addr(inp)))
    ip_napt_recv(p, iphdr);
```

**结论:必须 `esp_netif_napt_enable(以太网网卡)`,而不是 WiFi 网卡。** 这是早期方案失败(ping 不通)的直接原因。

---

## 三、编译期配置(sdkconfig.defaults)

三条缺一不可:

```
CONFIG_LWIP_L2_TO_L3_COPY=y   # WiFi 回程(DNAT 改 IP 头)需要 L2→L3 缓冲拷贝
CONFIG_LWIP_IP_FORWARD=y      # 开启 IP 转发(跨网卡路由)
CONFIG_LWIP_IPV4_NAPT=y       # 开启 NAT
```

映射到 lwIP 宏:`IP_FORWARD=1`、`IP_NAPT=1`、`IP_NAPT_PORTMAP=1`(端口映射默认随 NAPT 开启)。

---

## 四、模块设计(解耦)

| 文件 | 角色 | 关键逻辑 | 是否引用对侧 |
| --- | --- | --- | --- |
| `CODE/src/wifi.c` | WAN | STA 连接 + **默认路由** | 否 |
| `CODE/src/ethernet.c` | LAN | DHCP server + **NAPT 启用** + **DNS 下发** | 否 |
| `main/main.c` | 编排 | 只按序调用两个 init | 仅头文件 |

NAT 的「连接」发生在 lwIP 内核层(默认路由 + `netif->napt` + 转发),应用层两模块零耦合。

### 4.1 wifi.c(WAN 侧)

1. `s_sta_netif = esp_netif_create_default_wifi_sta();`(保存句柄)
2. `IP_EVENT_STA_GOT_IP` 回调里:

```c
/* 强制 WiFi STA 为默认路由,保证转发出去的包走 WAN */
ESP_ERROR_CHECK(esp_netif_set_default_netif(s_sta_netif));
```

> 注意:**不再**在此处 `esp_netif_napt_enable`(早期错误做法)。WiFi 只负责「成为默认路由」。

### 4.2 ethernet.c(LAN 侧)

1. 创建 netif 时声明 `ESP_NETIF_DHCP_SERVER` 标志。
2. `esp_eth_start()` 前:静态 IP + DNS 下发(两步)。
3. `ETHERNET_EVENT_CONNECTED`(网卡已 up)回调里:

```c
/* 在 LAN 网卡(以太网)上启用 NAPT */
err = esp_netif_napt_enable(s_eth_netif);
...
/* 启动 DHCP 服务器 */
err = esp_netif_dhcps_start(s_eth_netif);
```

### 4.3 DNS 下发(两步,缺一不可)

```c
esp_netif_dns_info_t dns = { .ip = ESP_IP4ADDR_INIT(8, 8, 8, 8) };
esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN, &dns);   // ① 设 DNS IP

uint8_t offer_dns = 1;
esp_netif_dhcps_option(s_eth_netif, ESP_NETIF_OP_SET,
                       ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));  // ② 打开下发开关
```

> 实测:DHCP 服务器默认不下发 DNS;`set_dns_info` 只写 IP、`dhcps_option` 只打开 `OFFER_DNS` 位,两个都要做。

---

## 五、完整数据流(带地址)

假设:WiFi WAN 地址 `10.199.148.159`,LAN 客户端 `192.168.4.2` 访问 `8.8.8.8`。

### 5.1 出站(客户端 → 互联网)

```
客户端 192.168.4.2:50000 → 8.8.8.8:53
   │ 到达以太网(inp, napt=1)
   ▼
ip4_forward:查路由 → 出口 WiFi(outp, napt=0)
   │ if (!outp->napt) 成立 → 调 ip_napt_forward()
   ▼
ip_napt_forward:inp->napt=1 → 建表并改写
   源 192.168.4.2:50000 → 10.199.148.159:<伪装端口>
   ▼
WiFi 发出 → 热点 → 8.8.8.8
```

### 5.2 回程(互联网 → 客户端)

```
8.8.8.8:53 → 10.199.148.159:<伪装端口>
   │ 到达 WiFi(inp, napt=0),目的 == WAN 地址
   ▼
ip4_input:if (!inp->napt && dest==WAN) → 调 ip_napt_recv()
   ▼
ip_napt_recv:查表命中,还原
   目的 10.199.148.159:<伪装端口> → 192.168.4.2:50000
   ▼
目的变成 192.168.4.2 → ip4_forward → 出口以太网(outp, napt=1)
   │ if(!outp->napt) 为假 → 不再二次改写
   ▼
以太网发出 → 客户端
```

---

## 六、验证方法

1. 编译烧录后串口应出现:
   ```
   wifi: WiFi got IP: <上游IP>
   ethernet: Ethernet link up
   ethernet: NAPT enabled on LAN
   ethernet: DHCP server started, serving 192.168.4.0/24
   esp_netif_lwip: DHCP server assigned IP to a client, IP is: 192.168.4.2
   ```
2. 有线设备拿到 `192.168.4.2`、网关 `192.168.4.1`、DNS `8.8.8.8`。
3. 设备上 `ping 8.8.8.8`(验证 NAT)和 `ping www.baidu.com`(验证 DNS + NAT)。

---

## 七、注意事项

1. **NAPT 必须开在以太网(LAN)网卡**,且需网卡已 up —— 故放在 `ETHERNET_EVENT_CONNECTED` 里,而不是 init。
2. **默认路由必须是 WiFi** —— 用 `esp_netif_set_default_netif` 显式锁定,防止以太网抢占。
3. **DNS 两步都要做**,否则客户端收不到 DNS。
4. WiFi 重连:`GOT_IP` 会再次触发 `set_default_netif`(保持默认路由);以太网 `CONNECTED` 每次也会重开 NAPT(幂等)。
5. 吞吐:ESP32 软件 NAT 约 10~20 Mbps,适合少量设备。
6. DNS 若在国内可换 `114.114.114.114`(改 `ESP_IP4ADDR_INIT` 参数即可)。

---

## 八、与参考工程 esp32_ethernet_router 的取舍

| 参考工程的做法 | 本方案是否采用 | 说明 |
| --- | --- | --- |
| NAPT 开在以太网(`ip_napt_enable(my_ap_ip,1)`) | ✅ 采用(改用 `esp_netif_napt_enable(handle)`) | 核心,方向必须一致 |
| `CONFIG_LWIP_L2_TO_L3_COPY=y` | ✅ 采用 | WiFi 回程需要 |
| `CONFIG_LWIP_IP_FORWARD` + `IPV4_NAPT` | ✅ 采用 | 基础前提 |
| 端口映射 portmap(DNAT) | ❌ 未采用 | 无外网入站需求 |
| netif_hooks 钩子、ACL、VPN、Web/MQTT/NetFlow/PCAP | ❌ 未采用 | 过度设计,本网关用不上 |

---

## 九、文件清单

| 文件 | 内容 |
| --- | --- |
| `sdkconfig.defaults` | 3 条 NAT 配置 |
| `CODE/src/wifi.c` | STA 连接 + 默认路由(不含 NAPT) |
| `CODE/src/ethernet.c` | DHCP server + NAPT 启用 + DNS 下发 |
| `CODE/inc/wifi.h` / `CODE/inc/ethernet.h` | 模块接口 |
| `main/main.c` | 编排 wifi_sta_init → ethernet_init |
