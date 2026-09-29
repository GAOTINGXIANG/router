# ESP32 软件 NAT 网关 —— 可行性评估报告

> 评估日期:2026-09-28
> 目标:ESP32-WROOM-32,Wi-Fi STA 作上行 WAN(DHCP client),以太网(LAN8720)作下行 LAN(DHCP server),经 NAT 让有线设备共享 Wi-Fi 上行
> 环境:ESP-IDF v6.0.2
> 结论:**原生支持良好,核心 NAT 实现难度低,做成完整可用网关难度中等。整体低~中难度,可落地。**

---

## 一、总体结论

| 维度 | 评估 |
| --- | --- |
| 核心 NAT(IP+端口转换) | ✅ 内建,2 个配置项 + 1 个 API,难度 **低** |
| 上下行接口 | ✅ 现有代码已具备(WiFi STA = WAN、以太网 = LAN) |
| DNS 下发 | ⚠️ 需自行处理(默认不下发),难度 **中** |
| 吞吐/资源 | ⚠️ 软件 NAT 受 CPU 限制,适合少量设备 |
| 官方示例 | ❌ 无,需按 API 自行拼装 |

---

## 二、ESP-IDF 6.0.2 NAT 支持程度核查

### 2.1 内建支持(已核实)

| 项 | 位置 / 说明 |
| --- | --- |
| lwIP IP 转发 | `CONFIG_LWIP_IP_FORWARD`(默认 n),"allows packets forwarding across multiple interfaces" |
| lwIP NAPT | `CONFIG_LWIP_IPV4_NAPT`(默认 n,依赖 IP_FORWARD),"Network Address and Port Translation" |
| 端口映射 | `CONFIG_LWIP_IPV4_NAPT_PORTMAP`(默认 y,依赖 IPV4_NAPT) |
| esp_netif 封装 | `esp_netif_napt_enable()` / `esp_netif_napt_disable()`([esp_netif.h:544/556](../../esp-idf/components/esp_netif/include/esp_netif.h)) |
| 转换协议 | lwIP NAPT 处理 TCP / UDP / ICMP 的地址与端口改写 |

### 2.2 关键实现细节(已读源码确认)

1. **无需 `esp_netif_napt_init`**:6.0.2 已移除旧版 init 接口,直接 `esp_netif_napt_enable()` 即可。
2. **NAPT 只能开在一个接口上**:`enable` 时会把其它接口的 NAPT 自动关掉。本方案正好开在 **WiFi STA(WAN)** 上。
3. **前置条件**:被 enable 的 netif 必须已 **up**(`netif_is_up` 检查),否则返回 `ESP_FAIL`。所以要在 **WiFi 拿到 IP 之后**(`IP_EVENT_STA_GOT_IP` 事件)调用。
4. **未开启时返回 `ESP_ERR_NOT_SUPPORTED`**:若没开 `CONFIG_LWIP_IPV4_NAPT`,`esp_netif_napt_enable` 直接返回不支持。

### 2.3 需要额外补齐的部分

- **无官方 NAT 示例**:`examples/` 下没有 Wi-Fi↔以太网 NAT 的参考工程,需按 API 自行组装。
- **DNS 不下发**:实测 DHCP 服务器源码,默认 `dns_server[].addr = 0`、`dhcps_dns = 0x00`,即**默认不给有线客户端下发 DNS**。不处理的话,有线设备拿到 IP 也解析不了域名(纯 IP 访问不受影响)。

---

## 三、与现有代码的衔接

当前项目已具备 NAT 网关的两大基础件,无需返工:

| 角色 | 现状 |
| --- | --- |
| WAN 上行 | [wifi.c](../CODE/src/wifi.c):WiFi STA,DHCP client,连 OPPOFindx7,断线重连 ✅ |
| LAN 下行 | [ethernet.c](../CODE/src/ethernet.c):以太网 DHCP server,静态 192.168.4.1/24 ✅ |
| 解耦 | WiFi 与以太网模块独立,无耦合 ✅ |

---

## 四、落地需要新增的工作(仅步骤,不写代码)

1. **开启内核选项**(menuconfig):
   - `Component config → LWIP → Enable IP forwarding`(`CONFIG_LWIP_IP_FORWARD=y`)
   - `Component config → LWIP → Enable NAT`(`CONFIG_LWIP_IPV4_NAPT=y`)

2. **在 WiFi 拿到 IP 后启用 NAPT**:
   - 在 `IP_EVENT_STA_GOT_IP` 事件里,对 STA 网卡调用 `esp_netif_napt_enable(sta_netif)`。
   - 需要拿到 STA 网卡句柄(可在 `esp_netif_create_default_wifi_sta()` 时保存返回值)。

3. **给 LAN 客户端下发 DNS**(三选一):
   - **A. 下发公网 DNS**(最简单):`esp_netif_dhcps_option` 设 `ESP_NETIF_DOMAIN_NAME_SERVER` = 8.8.8.8 / 114.114.114.114,客户端 DNS 直接经 NAT 转发出去。
   - **B. 下发上游热点 DNS**:把 WiFi STA 从 DHCP 拿到的 DNS 透传给有线客户端。
   - **C. 网关自建 DNS 转发器**(最稳但最重):下发 192.168.4.1,ESP32 上跑一个 DNS 服务器转发到上游(需额外组件/代码)。

4. **(可选)WiFi 重连后复核 NAPT**:STA 网卡句柄在重连过程中不变,NAPT 一般仍有效;若担心,可在 `STA_GOT_IP` 里重复调用 `esp_netif_napt_enable`(幂等)。

---

## 五、难点与风险

| # | 项 | 说明 | 影响 |
| --- | --- | --- | --- |
| 1 | **DNS 下发** | 默认不下发,是最主要的额外工作量 | 中 |
| 2 | **吞吐量** | ESP32 软件 NAT 受 CPU 限制,实测约 **10~20 Mbps**;几台有线设备上网浏览够用,大流量/跑满 100M 不现实 | 中 |
| 3 | **内存** | WiFi + 以太网 + NAPT 叠加,堆占用上升;ESP32 约 300KB RAM,需留意剩余 | 低~中 |
| 4 | **MSS/MTU** | lwIP TCP 会按接口 MTU 自适应 MSS;WiFi 与以太网默认 MTU 均为 1500,一般无需手动 clamp | 低 |
| 5 | **NAPT 单接口限制** | 只能在一个 netif 上开,本方案固定开在 WAN(WiFi STA),不影响 | 低 |
| 6 | **无示例可抄** | 需自行组装,但对熟悉 esp_netif 来说成本低 | 低 |

---

## 六、结论

- **可行性:完全可行**,ESP-IDF 6.0.2 的 lwIP NAPT 原生支持 IP/端口转换,`esp_netif_napt_enable` 一行 API 即用。
- **难度:核心 NAT 低;完整可用网关中**。真正的增量工作量不在 NAT 本身,而在 **DNS 下发策略** 与 **重连/资源边界处理**。
- **推荐路径**:开启 `IP_FORWARD` + `IPV4_NAPT` → `STA_GOT_IP` 时 `esp_netif_napt_enable` → DHCP 服务器下发公网 DNS(方案 A)先行打通,后续再按需升级为自建 DNS 转发器(方案 C)。

## 参考

- `components/esp_netif/include/esp_netif.h` —— `esp_netif_napt_enable/disable`
- `components/lwip/Kconfig` —— `LWIP_IP_FORWARD` / `LWIP_IPV4_NAPT` / `LWIP_IPV4_NAPT_PORTMAP`
- `components/lwip/lwip/src/include/lwip/lwip_napt.h` —— lwIP NAPT 接口
- `components/lwip/apps/dhcpserver/dhcpserver.c` —— DHCP 服务器 DNS 选项下发逻辑
