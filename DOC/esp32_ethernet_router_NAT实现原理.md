# esp32_ethernet_router NAT 实现原理说明

> 对象:`esp32_ethernet_router` 工程(WT32-ETH01 / LAN8720,以及 W5500 变体共用同一套 NAT 逻辑)
> 主题:LAN8720(以太网下行 LAN)与 WiFi STA(上行 WAN)之间如何实现 NAT
> 环境:ESP-IDF v6.0.2(NAPT 引擎源码位于 `components/lwip/lwip/src/core/ipv4/ip4_napt.c`)

---

## 一、总体结论(先看这里)

1. **NAT 用的不是自研代码,而是 ESP-IDF lwIP 内置的 NAPT 引擎**(`IP_NAPT` / `ip4_napt.c`),项目只负责「打开开关、喂端口映射表、在数据链路上挂钩子」。
2. **NAPT 开启在「以太网下行网卡」上**,而不是 WiFi 网卡上——这是最容易误解的一点。lwIP 的 `netif->napt` 标志标记的是「内网(待改写源地址)侧」网卡。
3. **源地址改写成哪个 IP,不是写死的**,而是改写为「包实际出去的那张网卡」的 IP:正常时是 WiFi STA 的 WAN 地址(`my_ip`),走 VPN 时自动变成 WireGuard 隧道地址。
4. 端口转发(DNAT)通过 `ip_portmap_add()/ip_portmap_remove()` 实现,由 `main/portmap.c` 管理并持久化到 NVS。
5. 开启 NAPT 的编译前提是三条 sdkconfig:`CONFIG_LWIP_IP_FORWARD`、`CONFIG_LWIP_IPV4_NAPT`、`CONFIG_LWIP_L2_TO_L3_COPY`。

---

## 二、网络拓扑与 NAT 的位置

```
                      esp32_ethernet_router
                 ┌───────────────────────────────────┐
  Internet ──────│ WiFi STA (WAN 上行)   my_ip       │
         ◄────── │  (DHCP client 或静态 IP)          │
                 │           │        ▲              │
                 │      NAPT 引擎(ip4_napt.c)         │
                 │           │        │              │
  LAN 设备 ──────│ Ethernet (LAN 下行)  my_ap_ip     │
  192.168.4.x ◄──│ (LAN8720, DHCP server, 192.168.4.1)│
                 └───────────────────────────────────┘
```

- **上行 WAN**:WiFi STA,拿到上游 IP `my_ip`(如 `192.168.1.23`)。
- **下行 LAN**:LAN8720 以太网,静态 IP `my_ap_ip`(默认 `192.168.4.1/24`),跑 DHCP server。
- **NAT 职责**:把 LAN 客户端(源 `192.168.4.x`)的流量改写为 `my_ip` 发出,回程再还原回 `192.168.4.x`。上游 AP 只看到 ESP32 一个客户端。

两个关键全局 IP(定义于 `main/esp32_nat_router.c`)：

| 变量 | 含义 | 默认值 |
| --- | --- | --- |
| `my_ip` | WiFi STA 上行地址(WAN) | 由上游 DHCP / 静态配置 |
| `my_ap_ip` | 以太网下行地址(LAN) | `192.168.4.1` |

---

## 三、编译期配置(sdkconfig)

`esp32_ethernet_router/sdkconfig.defaults` 中有三行是 NAT 能否工作的硬前提:

```
CONFIG_LWIP_L2_TO_L3_COPY=y   # 把二层帧复制到三层,NAPT 才能改写 IP 头
CONFIG_LWIP_IP_FORWARD=y      # 开启 IP 转发(跨网卡路由),映射为 lwIP IP_FORWARD=1
CONFIG_LWIP_IPV4_NAPT=y       # 开启 NAT,映射为 lwIP IP_NAPT=1
```

在 ESP-IDF 里这些 Kconfig 最终映射到 lwIP 宏(`components/lwip/port/include/lwipopts.h`)：

| Kconfig | lwIP 宏 | 值 |
| --- | --- | --- |
| `CONFIG_LWIP_IP_FORWARD` | `IP_FORWARD` | 1 |
| `CONFIG_LWIP_IPV4_NAPT` | `IP_NAPT` | 1 |
| `CONFIG_LWIP_IPV4_NAPT_PORTMAP`(默认 y) | `IP_NAPT_PORTMAP` | 1 |

`esp32_nat_router.c` 顶部有编译期硬校验,没开 NAPT 直接报错:

```c
#if !IP_NAPT
#error "IP_NAPT must be defined"
#endif
#include "lwip/lwip_napt.h"
```

> `CONFIG_LWIP_IPV4_NAPT_PORTMAP` 默认即为 `y`(见 lwIP `Kconfig` 的 `default y`),所以端口映射功能随 NAPT 一起打开,无需额外配置。

---

## 四、NAT 的启用(项目代码)

### 4.1 主启用点 —— 在「以太网下行」网卡上开 NAPT

`main/esp32_nat_router.c` 的 `app_main()` 末尾(`router_init()` 之后):

```c
if (eth_nat_enabled) {
    ip_napt_enable(my_ap_ip, 1);      // 在 IP == my_ap_ip(以太网)的网卡上开 NAPT
    ESP_LOGI(TAG, "NAT is enabled");
} else {
    ESP_LOGI(TAG, "NAT is disabled (routed mode)");
}
```

- `my_ap_ip` 是**以太网下行**地址(默认 `192.168.4.1`),所以 `ip_napt_enable` 找到的是 **LAN8720 那张网卡**。
- `eth_nat_enabled` 来自 NVS 键 `eth_nat`(默认 1),可由 `set_eth_nat on/off` 或网页改。

### 4.2 lwIP 侧 `ip_napt_enable()` 做了什么

`ip4_napt.c` 的 `ip_napt_enable(addr, enable)`:

```c
for (netif = netif_list; netif; netif = netif->next) {
    if (netif_is_up(netif) && !ip_addr_isany(&netif->ip_addr)
        && ip_2_ip4(&netif->ip_addr)->addr == addr) {
        netif->napt = enable;       // 找到地址匹配的网卡,置 napt 标志
        matching_netif = netif;
    }
    ...
}
```

即:**`netif->napt = 1` 落在以太网下行网卡上**。lwIP 的转发/收包路径会在后面据此判断「要不要做 NAT」。

> 关键语义:`netif->napt` 标记的是**内网侧(待 NAT 的源)网卡**,而不是外网侧。这正是本工程与早期方案文档里「在 WAN 上开 NAPT」描述不同的根本原因——lwIP 实际需要的是在内网网卡上置位,外网地址则在改写时由「出口网卡的 IP」自动决定。

### 4.3 以太网上行(DHCP client)模式的特殊处理

普通模式 `my_ap_ip` 在开机时已知(静态 IP),所以 `app_main` 里能直接开。但 `set_eth_dhcpc on` 模式里以太网变成 DHCP client,`my_ap_ip` 开机时未知,于是在拿到租约后**重新开一次** NAT(`esp32_nat_router.c` 的 `IP_EVENT_ETH_GOT_IP` 处理器):

```c
my_ap_ip = event->ip_info.ip.addr;
if (eth_nat_enabled) {
    ip_napt_enable(my_ap_ip, 1);   // 用 DHCP 分到的地址再开一次
    ...
}
```

---

## 五、lwIP NAPT 引擎原理(ip4_napt.c)

### 5.1 转换表结构

NAPT 维护一张**地址端口转换表**(`ip_napt_table`,默认 512 条 `IP_NAPT_MAX`),每条记录一个「内网连接 ↔ 外网连接」的映射:

```c
struct ip_napt_entry {
    u32_t src;      // 内网源 IP(如 192.168.4.10)
    u32_t dest;     // 外网目的 IP
    u16_t sport;    // 内网源端口
    u16_t dport;    // 外网目的端口
    u16_t mport;    // 改写后的「伪装端口」(外网侧端口)
    u8_t  proto;    // TCP / UDP / ICMP
    ... 序列号 / FIN / RST 等 TCP 状态位 ...
};
```

- 伪装端口 `mport` 从 **49152–61439**(`IP_NAPT_PORT_RANGE_START`/`_END`)里随机取,避开已占用端口。
- 超时回收:`TCP 30 分钟`(断开后 `TCP_MSL`)、`UDP 2 秒`、`ICMP 2 秒`;由定时器 `ip_napt_tmr` 每 2 秒做一次 GC。

### 5.2 两个挂钩点

lwIP 只在两个地方调用 NAPT(`ip4.c`)：

**① 转发路径(`ip4_forward`)—— 出站改写(SNAT)**

```c
if (!netif->napt) {                          // 出口网卡不是 NAPT 网卡才做
    if (ip_napt_forward(p, iphdr, inp, netif) != ERR_OK)
        return;                              // 改写失败则丢弃
}
```

**② 收包路径(`ip4_input`)—— 回程还原(DNAT)**

```c
if (!inp->napt && ip4_addr_cmp(&iphdr->dest, netif_ip4_addr(inp)))
    ip_napt_recv(p, iphdr);                  // 目的=本机 WAN 地址时查表还原
```

### 5.3 出站 SNAT(内网 → 互联网)

`ip_napt_forward(p, iphdr, inp, outp)`:

1. 先判断 `if (!inp->napt) return ERR_OK;` —— **只有入包网卡是 NAPT 网卡(以太网)才改写**。
2. 按协议处理:
   - **TCP**:SYN 且源端口 ≥1024 时新建表项,分配 `mport`;否则查已有表项(找不到则回 `ICMP port unreachable`)。
   - **UDP**:源端口 ≥1024 新建表项,否则查表。
   - **ICMP Echo**:用 identifier 做 key 建表。
3. 改写源地址/端口:
   ```c
   ip_napt_modify_addr(iphdr, &iphdr->src, ip_2_ip4(&outp->ip_addr)->addr);
   ```
   **源 IP 被改成 `outp->ip_addr`,即出口网卡的地址**——正常是 WiFi STA 的 `my_ip`,走 VPN 时是隧道地址。这一步同时做 IP 头与 TCP/UDP 校验和的增量更新(`checksumadjust`,RFC 1624)。

### 5.4 回程 DNAT(互联网 → 内网)

`ip_napt_recv(p, iphdr)`(在 `ip4_input` 中,包到达 WiFi STA 且目的地址 == WAN 地址时触发):

1. 先查**端口映射表**(见第六节),命中则改写目的为内网映射目标(DNAT 端口转发)。
2. 否则按 `(proto, src, sport, dport, mport)` 查 `ip_napt_table`,把**目的 IP/端口还原**为 `t->src / t->sport`(即最初的内网客户端)。
3. 还原后,目的地址变成 `192.168.4.x`,lwIP 继续走 `ip4_forward` 把它转发到以太网下行网卡。

> 回程包不会再被二次改写:此时入包网卡是 WiFi(无 `napt`),出口网卡是以太网(有 `napt`),`ip4_forward` 里的 `if (!netif->napt)` 为假,跳过 `ip_napt_forward`。

---

## 六、端口映射(Port Forwarding / DNAT)

端口转发让「从外网访问 WAN 的某端口」的流量被转发到内网某台主机,本质是**入站方向的目的 NAT**。

### 6.1 lwIP 端口映射表

`ip4_napt.c` 里另有一张 `ip_portmap_table`(默认 32 条 `IP_PORTMAP_MAX`):

```c
struct ip_portmap_entry {
    u32_t maddr;   // 外网地址(绑定 my_ip 或 VPN 隧道 IP)
    u32_t daddr;   // 内网目标主机 IP
    u16_t mport;   // 外网映射端口
    u16_t dport;   // 内网目标端口
    u8_t  proto;   // TCP / UDP
    u8_t  valid;
};
```

注册 API:`ip_portmap_add(proto, maddr, mport, daddr, dport)`。

- 入站:包到达 WAN、目的端口命中 `mport` 时,`ip_napt_recv` 优先查端口映射表,把目的改写成 `daddr:dport`。
- 出站:内网主机回包时,`ip_napt_forward` 用 `ip_portmap_find_dest()` 把源地址改回 `maddr:mport`(SNAT 回程)。

### 6.2 项目的端口映射管理层(`main/portmap.c`)

| 函数 | 作用 |
| --- | --- |
| `apply_portmap_tab()` | 遍历 `portmap_tab[]`,逐条 `ip_portmap_add()`,绑定地址 `my_ip`(STA)或 `vpn_tunnel_ip`(VPN) |
| `delete_portmap_tab()` | 逐条 `ip_portmap_remove()`(在 STA 每次拿到新 IP 前先删旧映射) |
| `add_portmap()/del_portmap()` | 增删条目并立即生效 + 写入 NVS |
| `get_portmap_tab()` | 从 NVS 键 `portmap_tab` 读表 |

绑定地址选择(`portmap.c` 的 `apply_portmap_tab`):

```c
if (portmap_tab[i].iface == 1) {          // iface=1 → 绑 VPN
    if (vpn_tunnel_ip == 0) continue;
    bind_ip = vpn_tunnel_ip;
} else {                                  // iface=0 → 绑 STA
    bind_ip = my_ip;
}
ip_portmap_add(proto, bind_ip, mport, daddr, dport);
```

STA 重连导致 `my_ip` 变化时,`wifi_event_handler` 的 `IP_EVENT_STA_GOT_IP` 里先 `delete_portmap_tab()` 再 `apply_portmap_tab()`,保证映射跟着新 WAN 地址走。

---

## 七、完整数据流(带具体地址示例)

假设:STA 拿到 `my_ip=192.168.1.23`,以太网 `my_ap_ip=192.168.4.1`,LAN 客户端 `192.168.4.10` 访问 `8.8.8.8:53`。

### 7.1 出站(客户端 → 互联网)

```
[客户端 192.168.4.10:50000 → 8.8.8.8:53]
      │  以太网帧(源 192.168.4.10)
      ▼
LAN8720 收包 → dl_netif_input_hook           ← 项目钩子(隔离/ACL/VPN kill switch/PMTU/MSS/统计/NetFlow/PCAP)
      │
      ▼
lwIP ethernet_input → ip4_input
      │ 目的 8.8.8.8 ≠ 本机 → 走 ip4_forward
      ▼
ip4_forward:查路由 → 出口 = WiFi STA,TTL-1
      │  inp=以太网(napt=1)  outp=WiFi(napt=0)
      │  if (!outp->napt) → 调 ip_napt_forward()
      ▼
ip_napt_forward:
      新建表项 {src=192.168.4.10, sport=50000, dest=8.8.8.8, dport=53, mport=50123}
      改写:src 192.168.4.10:50000  →  192.168.1.23:50123   (源 = outp->ip_addr = my_ip)
      │
      ▼
netif_linkoutput_hook(STA 字节计数 + ACL from_esp) → WiFi 发出 → 上游 AP → 8.8.8.8
```

### 7.2 回程(互联网 → 客户端)

```
[8.8.8.8:53 → 192.168.1.23:50123]
      ▼
WiFi 收到 → netif_input_hook(ACL to_esp + 字节计数)
      ▼
lwIP ip4_input:目的 == my_ip(192.168.1.23),inp=WiFi(napt=0)
      │  if (!inp->napt && dest==inp->ip) → 调 ip_napt_recv()
      ▼
ip_napt_recv:查表命中 mport=50123
      改写:dest 192.168.1.23:50123 → 192.168.4.10:50000
      │
      ▼
目的现在是 192.168.4.10 → ip4_forward → 出口 = 以太网
      │  inp=WiFi(napt=0)  outp=以太网(napt=1)
      │  if(!outp->napt) 为假 → 不再做二次 NAT
      ▼
dl_netif_linkoutput_hook(ACL to_eth/MSS/统计/NetFlow egress/PCAP) → LAN8720 发出 → 客户端
```

---

## 八、项目钩子与 NAT 的先后关系

`main/netif_hooks.c` 通过替换 lwIP `netif->input` / `netif->linkoutput` 挂了一堆处理逻辑。它们相对 NAT 的位置决定看到的是「改写前」还是「改写后」的包:

| 钩子 | 挂载点 | 相对 NAT 的位置 | 主要处理 |
| --- | --- | --- | --- |
| `dl_netif_input_hook` | 以太网输入 | **NAT 之前**(看到内网真实源 IP) | LAN 隔离、ACL `from_eth`、VPN kill switch、PMTU、MSS clamp、每客户端统计、NetFlow ingress、PCAP |
| `dl_netif_linkoutput_hook` | 以太网输出 | **NAT 之后**(回程已还原) | ACL `to_eth`、MSS clamp、统计、NetFlow egress、PCAP |
| `netif_input_hook` | WiFi 输入 | **NAT 还原之前** | ACL `to_esp`、STA 字节计数 |
| `netif_linkoutput_hook` | WiFi 输出 | **NAT 改写之后** | ACL `from_esp`、TTL 覆盖、STA 字节计数 |

> 这与 NetFlow 文档里「ingress 是 pre-NAT 路径,源 IP 是真实 LAN 客户端地址」的说明完全一致——因为 `dl_netif_input_hook` 跑在 NAPT 改写之前。

---

## 九、特殊模式与交互

### 9.1 关闭 NAT(纯路由模式)

`set_eth_nat off`(NVS 键 `eth_nat=0`)→ `app_main` 跳过 `ip_napt_enable()`,lwIP 只做 `IP_FORWARD` 路由,不改写地址。此时上游网络必须知道 `192.168.4.0/24` 的路由,否则回程不可达。

### 9.2 VPN(WireGuard)交互

- 出站改写源地址用的是**出口网卡的 IP**:当 VPN `route_all` 生效、默认路由指向 WireGuard 隧道时,`outp->ip_addr` = 隧道地址,LAN 流量自动以隧道地址出现在对端。
- 端口映射 `iface=1` 把 DNAT 绑在 `vpn_tunnel_ip` 上,实现「从 VPN 侧入站转发到内网主机」。

### 9.3 以太网上行(DHCP client)模式

见 4.3:以太网变 DHCP client、`my_ap_ip` 由上游分配,NAT 在 `IP_EVENT_ETH_GOT_IP` 里用分配到的地址重新启用。此模式下仍由 `ip_napt_forward` 把「以太网 → WiFi」的流量伪装成 STA 地址。

---

## 十、关键参数与限制

| 参数 | 值 | 出处 |
| --- | --- | --- |
| NAPT 转换表 | 512 条(`IP_NAPT_MAX`) | `lwip_napt.h` |
| 端口映射表 | 32 条(`IP_PORTMAP_MAX`) | `lwip_napt.h` |
| 伪装端口范围 | 49152–61439 | `lwip_napt.h` |
| TCP 超时 | 30 分钟(断开后 `TCP_MSL`) | `lwip_napt.h` |
| UDP / ICMP 超时 | 2 秒 | `lwip_napt.h` |
| GC 周期 | 2 秒(`NAPT_TMR_INTERVAL`) | `ip4_napt.h` |
| 项目端口映射数组 | `portmap_tab[IP_PORTMAP_MAX]` = 32 | `esp32_nat_router.c` |

限制/注意:

1. **纯软件 NAT**,ESP32 实测约 10~20 Mbps,跑不满百兆。
2. NAT、DHCP server 的开关都在**建网卡时生效**,运行时修改需要重启(见 README)。
3. `ip_napt_enable` 依赖网卡已 up 且已配置 IP,否则找不到匹配网卡(对应 4.3 里上行模式延后启用的原因)。
4. 只有 TCP/UDP/ICMP Echo 三种被 NAPT 处理,其它协议(如 GRE)不会被转换。

---

## 十一、相关文件与函数索引

| 文件 | 关键内容 |
| --- | --- |
| `esp32_ethernet_router/main/esp32_nat_router.c` | `ip_napt_enable(my_ap_ip,1)`(app_main);`IP_EVENT_ETH_GOT_IP` 重开 NAPT;`my_ip`/`my_ap_ip` 定义 |
| `esp32_ethernet_router/main/portmap.c` | 端口映射表管理:`apply/delete/add/del/get_portmap_tab` |
| `esp32_ethernet_router/main/netif_hooks.c` | 四张网卡的 input/linkoutput 钩子,与 NAT 的先后关系 |
| `esp32_ethernet_router/sdkconfig.defaults` | `IP_FORWARD`、`IPV4_NAPT`、`L2_TO_L3_COPY` |
| `components/lwip/lwip/src/core/ipv4/ip4_napt.c` | NAPT 引擎本体(表、`ip_napt_forward`、`ip_napt_recv`、`ip_portmap_*`) |
| `components/lwip/lwip/src/core/ipv4/ip4.c` | 转发/收包路径里调用 NAPT 的两个挂钩点 |
| `components/lwip/lwip/src/include/lwip/lwip_napt.h` | 公共 API 与默认参数 |
