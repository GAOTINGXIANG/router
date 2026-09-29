# NAT 网关代码落地方案

> 目标:Wi-Fi STA 作上行 WAN(DHCP client),以太网(LAN8720)作下行 LAN(DHCP server),经 lwIP NAPT 让有线设备共享 Wi-Fi 上网
> 环境:ESP-IDF v6.0.2
> 前置:WiFi STA、以太网 DHCP 服务器已实现且解耦(见 `CODE/src/wifi.c`、`CODE/src/ethernet.c`)

---

## 一、总体设计

```
有线设备 ──以太网──► [LAN8720] ──► lwIP 转发(IP_FORWARD) ──► NAPT 改写 ──► [WiFi STA] ──► 热点(OPPOFindx7) ──► 互联网
                                  (下行 LAN, 静态 192.168.4.1)      (上行 WAN, DHCP 拿 IP)
```

- **转发**:`CONFIG_LWIP_IP_FORWARD` 让 lwIP 在两张网卡之间转发数据包。
- **NAPT**:`esp_netif_napt_enable()` 开在 **WAN(WiFi STA)** 上,自动做源 IP/端口改写与回程还原。
- **解耦原则**:NAPT 属于 WAN 侧 → 放 `wifi.c`;DNS 下发属于 LAN 侧 → 放 `ethernet.c`。两模块仍互不引用。

---

## 二、配置变更(sdkconfig)

在项目根 `sdkconfig.defaults` 新增(或用 `idf.py menuconfig` 勾选):

```
CONFIG_LWIP_IP_FORWARD=y
CONFIG_LWIP_IPV4_NAPT=y
```

- `LWIP_IPV4_NAPT_PORTMAP`(端口映射)默认已为 `y`,无需改。
- 改完执行一次 `idf.py reconfigure` 或直接 `idf.py build` 生效。

---

## 三、代码改动点

### 3.1 WiFi 模块(CODE/src/wifi.c)—— WAN 侧启用 NAPT

改动 3 处,全部在 `wifi.c` 内部,不触碰以太网:

1. **保存 STA 网卡句柄**:

```c
static esp_netif_t *s_sta_netif = NULL;

void wifi_sta_init(void)
{
    s_sta_netif = esp_netif_create_default_wifi_sta();   /* 原来是丢弃返回值 */
    ...
}
```

2. **在拿到 IP 后启用 NAPT**(`IP_EVENT_STA_GOT_IP` 事件里,此时 STA 网卡已 up):

```c
} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ...打印 IP...

    /* WAN 侧启用 NAPT(必须网卡 up 后调用,否则返回 ESP_FAIL) */
    esp_err_t err = esp_netif_napt_enable(s_sta_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NAPT enable failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "NAPT enabled on WAN");
    }
}
```

> 说明:`esp_netif_napt_enable` 需 `CONFIG_LWIP_IPV4_NAPT=y`(否则返回 `ESP_ERR_NOT_SUPPORTED`)、网卡已 up(否则 `ESP_FAIL`)、且同一时刻只能开在一个网卡上(本方案只开 WAN)。

### 3.2 以太网模块(CODE/src/ethernet.c)—— LAN 侧下发 DNS

在 `ethernet_init()` 里、`esp_eth_start()` **之前**加 DNS 配置(两步缺一不可):

```c
/* 7.5 给 LAN 客户端下发 DNS(两步:设 IP + 打开下发开关) */
esp_netif_dns_info_t dns = {
    .ip = {
        .type = ESP_IPADDR_TYPE_V4,
        .u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8),   /* 公网 DNS,可换成 114.114.114.114 */
    },
};
ESP_ERROR_CHECK(esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN, &dns));

uint8_t offer_dns = 1;
ESP_ERROR_CHECK(esp_netif_dhcps_option(s_eth_netif, ESP_NETIF_OP_SET,
                                       ESP_NETIF_DOMAIN_NAME_SERVER,
                                       &offer_dns, sizeof(offer_dns)));
```

> ⚠️ **关键**:实测 6.0.2 源码,DHCP 服务器默认 **不下发 DNS**,而且下发需要两个独立动作:
> 1. `esp_netif_set_dns_info()` → 内部调 `dhcps_dns_setserver_by_type()`,写入 DNS **IP**;
> 2. `esp_netif_dhcps_option(..., ESP_NETIF_DOMAIN_NAME_SERVER, &1)` → 打开 `OFFER_DNS` **位**。
>
> 两者都做了,DHCP offer 里才会带 DNS(option 6)。

### 3.3 main.c —— 无需改动

现有顺序已满足:`wifi_sta_init()`(WAN)先于 `ethernet_init()`(LAN)。NAPT 在 WiFi 拿到 IP 后自动启用,与以太网初始化时机无关。

---

## 四、解耦设计说明

| 模块 | 负责 | 是否引用对侧 |
| --- | --- | --- |
| `wifi.c` | WAN:STA 连接 + **NAPT 启用**(只用自己的 STA 句柄) | 否 |
| `ethernet.c` | LAN:DHCP server + **DNS 下发**(只用自己的 eth 句柄 + 固定公网 DNS) | 否 |
| `main.c` | 仅编排调用顺序 | 是(仅头文件) |

NAT 的「连接」发生在 lwIP 内核层(`IP_FORWARD` + `NAPT`),应用层两模块零耦合。

> 备选:若将来想让 LAN 客户端使用「上游热点动态下发的 DNS」而非固定公网 DNS,需要 WiFi 把拿到的 DNS 传给以太网,这会打破严格解耦;届时可用全局配置结构体或事件做一次间接传递。当前方案用固定公网 DNS,保持解耦且实现最简单。

---

## 五、数据流(运行时)

```
1. 有线设备 DHCP 请求 → 以太网 dhcps 响应(IP 192.168.4.x + 网关 192.168.4.1 + DNS 8.8.8.8)
2. 设备访问外网:源 192.168.4.x:端口 → 到达 ESP32 以太网口
3. lwIP IP_FORWARD 把包转发到 WiFi STA 网卡
4. NAPT 把源 IP/端口改写为 WiFi STA 的上行 IP(WAN 地址)并记录映射
5. 经热点出互联网;回包按 NAPT 映射表还原目的 IP/端口,送回有线设备
```

---

## 六、验证方法

1. 编译烧录后,串口应出现:
   ```
   wifi: WiFi got IP: <热点分配的IP>
   wifi: NAPT enabled on WAN
   ethernet: DHCP server started, serving 192.168.4.0/24
   ```
2. 有线设备接 RJ45,拿到 `192.168.4.2`,网关 `192.168.4.1`,DNS `8.8.8.8`。
3. 设备上执行 `ping 8.8.8.8`(验证 NAT 转发)和 `ping www.baidu.com`(验证 DNS + NAT)。

---

## 七、注意事项与风险

1. **NAPT 必须在 WAN 网卡 up 后启用** → 放在 `IP_EVENT_STA_GOT_IP`,不要放在 `wifi_sta_init()` 里(那时网卡还没拿到 IP)。
2. **WiFi 重连**:STA 网卡句柄在重连期间不变,NAPT 一般仍生效;若保守,可在每次 `STA_GOT_IP` 重复调 `esp_netif_napt_enable`(幂等,内部会先关其它网卡的 NAPT)。
3. **吞吐量**:ESP32 软件 NAT 约 10~20 Mbps,够几台设备浏览/视频,跑满 100M 不现实。
4. **内存**:WiFi + 以太网 + NAPT 叠加,留意堆剩余(必要时 `esp_get_free_heap_size()` 观察)。
5. **DNS 若换国内环境**:8.8.8.8 可能延迟高,可改用 `114.114.114.114`(代码里改 `ESP_IP4TOADDR(114,114,114,114)` 即可)。
6. **若热点无外网**:NAT 通了但公网 DNS 解析不出域名;此时只 `ping IP` 能通、域名不通,属上游问题而非本机 NAT。

---

## 八、改动文件清单

| 文件 | 改动 |
| --- | --- |
| `sdkconfig.defaults`(新增) | 加 `IP_FORWARD`、`IPV4_NAPT` |
| `CODE/src/wifi.c` | 保存 STA 句柄;`STA_GOT_IP` 里 `esp_netif_napt_enable()` |
| `CODE/src/ethernet.c` | `esp_eth_start` 前加 DNS 下发(两步) |
| `main.c` / `CMakeLists.txt` | 无改动 |
