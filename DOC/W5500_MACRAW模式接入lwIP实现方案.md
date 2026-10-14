# W5500 MACRAW 模式接入 lwIP 协议栈 —— 实现方案

> 目标:让 W5500(SPI 以太网控制器)工作在 **MACRAW 原始帧模式**,作为 ESP32 的以太网卡接入 lwIP 协议栈,复用现有 NAT / DHCP 服务器逻辑
> 环境:ESP-IDF v6.0.2,目标 ESP32-WROOM-32
> 依赖组件:本地 `components/w5500`(v2.0.0)+ `components/wiznet_common`(v1.0.0)
> 状态:方案(待实施)
> 结论:**可行。** 用 `esp_eth_mac_new_w5500()` + `esp_eth_phy_new_w5500()` 替换现有 `ethernet.c` 里的 LAN8720 的 MAC/PHY 两个 factory,其余(驱动安装、netif 挂载、NAPT、DHCP、静态 IP)完全复用;唯一新增的是 SPI 总线初始化。

---

## 一、方案概述

W5500 是「硬件 TCP/IP 栈 + 10/100M MAC + PHY」三合一的 SPI 芯片。本方案**弃用它的硬件 TCP/IP 栈**,把 Socket0 配成 **MACRAW 模式**,让它只做「以太网帧的收发器」,把原始帧(含 MAC 头)透传给 ESP32,由 ESP32 的 lwIP 栈完成 IP/TCP/UDP 处理。

驱动 `espressif/w5500` 已经封装好了这一切:它实现标准 `esp_eth_mac_t` / `esp_eth_phy_t` 接口,接入路径与 LAN8720 **完全相同**。所以只需替换 MAC/PHY 的创建方式,上层(NAPT、DHCP 服务器、静态 IP、DNS 下发)一行不用改。

## 二、原理:MACRAW 模式

驱动在初始化时把 Socket0 的 `Sn_MR` 寄存器写成 MACRAW 模式(`src/esp_eth_mac_w5500.c:147`):

```c
uint8_t smr = W5500_SMR_MAC_RAW | W5500_SMR_MAC_FILTER | W5500_SMR_MAC_BLOCK_MCAST;
```

- `W5500_SMR_MAC_RAW`(bit2):收发**完整以太网帧**(含 14 字节 MAC 头),芯片不做 IP 解析。
- 收:链路来的帧 → W5500 的 16KB RX 缓冲区 → 中断/轮询通知 ESP32 → `emac_wiznet_receive()` 读出交给 lwIP。
- 发:lwIP 下发的帧 → `emac_wiznet_transmit()` 写入 16KB TX 缓冲区 → `SEND` 命令 → 芯片发出。
- 芯片自带的 TCP/IP 栈**完全不使用**(README 原文 "not used in the ESP driver")。

## 三、与现有 LAN8720 方案的差异

现有 [CODE/src/ethernet.c](../CODE/src/ethernet.c) 的接入流程和 W5500 完全同构,只有第 2、3 步不同:

| 步骤 | LAN8720(现状) | W5500(本方案) |
| --- | --- | --- |
| 物理接口 | RMII 固定引脚 | **SPI**(先 `spi_bus_initialize`) |
| 创建 MAC | `esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg)` | `esp_eth_mac_new_w5500(&w5500_config, &mac_cfg)` |
| 创建 PHY | `esp_eth_phy_new_lan87xx(&phy_cfg)` | `esp_eth_phy_new_w5500(&phy_cfg)` |
| 驱动安装/挂载/启动 | `esp_eth_driver_install` → `esp_eth_new_netif_glue` → `esp_netif_attach` → `esp_eth_start` | **完全相同** |
| NAPT / DHCP / 静态 IP / DNS | `esp_netif_*` 层 | **完全相同** |

## 四、硬件接线(SPI)

W5500 走 SPI,引脚可经 GPIO 矩阵任意映射。建议默认用 VSPI(SPI3_HOST)标准引脚(可按实际板子调整):

| W5500 引脚 | ESP32 建议 GPIO | 说明 |
| --- | --- | --- |
| SCS / CS | GPIO5 | 片选(VSPI CS0 默认) |
| SCLK | GPIO18 | SPI 时钟 |
| MOSI | GPIO23 | 主出从入 |
| MISO | GPIO19 | 主入从出 |
| RSTn | 任意 GPIO 或上拉 | 复位;若模块已上拉可置 `-1` |
| INTn | 任意 GPIO 或悬空 | 中断;不用就选轮询模式(`int_gpio_num = -1`) |

> 注意:W5500 与 LAN8720 的 RMII 引脚(GPIO19/21/22/25/26/27/18/23/0)**有重叠**(MISO=19、MOSI=23 恰与 RMII 的 TXD0=19、MDC=23 冲突)。若两个网卡要**同时存在**,SPI 引脚必须改到不冲突的 GPIO;若只二选一,则无冲突问题。

## 五、组件与构建配置

1. `main/CMakeLists.txt` 的 `REQUIRES` 增加 `w5500`(`wiznet_common` 由 w5500 的 `public` 依赖自动带入,无需手写):

```cmake
REQUIRES
w5500          # 新增;替换 LAN8720 时可去掉 lan87xx
nvs_flash
esp_event
esp_netif
esp_eth
esp_wifi
```

2. 头文件(在用到 W5500 的源文件里):

```c
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "driver/spi_master.h"
```

## 六、代码实现(完整)

新增一个初始化函数(可放到新文件 `CODE/src/w5500_ethernet.c`,或直接在 `ethernet.c` 里加),逻辑照搬现有 `ethernet_init()`,只改 MAC/PHY 与 SPI。

```c
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "driver/spi_master.h"

static const char *TAG = "w5500_eth";

/* ---------- 引脚与参数(按实际板子改) ---------- */
#define W5500_SPI_HOST      SPI3_HOST   /* VSPI */
#define W5500_SPI_MISO_GPIO 19
#define W5500_SPI_MOSI_GPIO 23
#define W5500_SPI_SCLK_GPIO 18
#define W5500_SPI_CS_GPIO   5
#define W5500_SPI_CLOCK_MHZ 20          /* 先 20MHz,稳定后可逐步抬高到 40~60 */
#define W5500_SPI_INT_GPIO  -1          /* -1 = 轮询模式;给 GPIO 则用中断模式 */

static esp_netif_t      *s_eth_netif  = NULL;
static esp_eth_handle_t  s_eth_handle = NULL;

/* 事件回调:与 LAN8720 版完全一致(NAPT + DHCP 服务器) */
static void eth_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    esp_err_t err;

    switch (event_id) {
    case ETHERNET_EVENT_START:
        esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "W5500 MAC %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");
        esp_netif_napt_enable(s_eth_netif);
        err = esp_netif_dhcps_start(s_eth_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGW(TAG, "dhcps start failed: %s", esp_err_to_name(err));
        }
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet link down");
        esp_netif_dhcps_stop(s_eth_netif);
        break;
    default:
        break;
    }
}

void w5500_ethernet_init(void)
{
    /* 1. netif:DHCP 服务器(与 LAN8720 版一致) */
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base_cfg.flags = (esp_netif_flags_t)(ESP_NETIF_IPV4_ONLY_FLAGS(ESP_NETIF_DHCP_SERVER) |
                                         ESP_NETIF_DEFAULT_ARP_FLAGS |
                                         ESP_NETIF_FLAG_EVENT_IP_MODIFIED |
                                         ESP_NETIF_DEFAULT_IPV6_AUTOCONFIG_FLAGS);
    esp_netif_config_t netif_cfg = { .base = &base_cfg, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    s_eth_netif = esp_netif_new(&netif_cfg);
    assert(s_eth_netif);

    /* 2. SPI 总线初始化(新增,W5500 独有;LAN8720 没有这一步) */
    spi_bus_config_t bus_cfg = {
        .miso_io_num     = W5500_SPI_MISO_GPIO,
        .mosi_io_num     = W5500_SPI_MOSI_GPIO,
        .sclk_io_num     = W5500_SPI_SCLK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 16000,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(W5500_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* 3. SPI 设备配置(command_bits/address_bits 由驱动自动设为 16/8,无需手写) */
    spi_device_interface_config_t spi_devcfg = {
        .mode            = 0,
        .clock_speed_hz  = W5500_SPI_CLOCK_MHZ * 1000 * 1000,
        .queue_size      = 16,
        .spics_io_num    = W5500_SPI_CS_GPIO,
    };

    /* 4. 创建 MAC:W5500 的 MACRAW 驱动 */
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, &spi_devcfg);
    w5500_config.base.int_gpio_num = W5500_SPI_INT_GPIO;
#if W5500_SPI_INT_GPIO < 0
    w5500_config.base.poll_period_ms = 10;   /* 轮询周期,仅轮询模式需要 */
#endif

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;    /* 避免接收任务栈溢出(官方建议) */
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    assert(mac);

    /* 5. 创建 PHY:W5500 内置 PHY */
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    assert(phy);

    /* 6. 安装驱动 + 挂载 netif(与 LAN8720 版一致) */
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &s_eth_handle));

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, glue));

    /* 7. 事件回调(与 LAN8720 版一致) */
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));

    /* 8. 静态 IP + DNS + 启动(与 LAN8720 版一致) */
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_eth_netif));

    esp_netif_ip_info_t ip_info = {
        .ip      = { .addr = ESP_IP4TOADDR(192, 168, 4, 1) },
        .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
        .gw      = { .addr = ESP_IP4TOADDR(192, 168, 4, 1) },
    };
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_eth_netif, &ip_info));

    esp_netif_dns_info_t dns = { .ip = ESP_IP4ADDR_INIT(8, 8, 8, 8) };
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN, &dns));
    uint8_t offer_dns = 1;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_eth_netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &offer_dns, sizeof(offer_dns)));

    /* 9. 启动(链路建立后由事件回调启动 DHCP 服务器) */
    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));

    ESP_LOGI(TAG, "W5500 ethernet init done, static IP 192.168.4.1");
}
```

`main.c` 里把 `ethernet_init()` 换成 `w5500_ethernet_init()`(或在两个网卡间二选一)。

## 七、关键配置项说明

1. **中断 vs 轮询(互斥)**:驱动校验 `(int_gpio_num >= 0) != (poll_period_ms > 0)`。
   - 中断模式:`int_gpio_num = <GPIO>`、`poll_period_ms = 0`(默认 `ETH_W5500_DEFAULT_CONFIG` 给 `int_gpio_num = 4`)。
   - 轮询模式:`int_gpio_num = -1`、`poll_period_ms = 10`(省一根中断线,CPU 稍高)。
2. **SPI 帧格式**:`wiznet_spi_init()` 会自动把 `command_bits=16`、`address_bits=8` 填上(WIZnet 的地址段+控制段),不要自己写。
3. **SPI 时钟**:先 20MHz,跑通后再往上调(40/60MHz 视走线而定)。
4. **接收栈**:`mac_config.rx_task_stack_size = 4096`,否则 1518 字节帧 + 中断处理可能栈溢出。
5. **MAC 地址**:W5500 出厂烧录了唯一 MAC,`ETH_MAC_DEFAULT_CONFIG()` 无需手动指定;也可用 `mac_config.` 相关字段覆盖。
6. **PHY 地址**:W5500 内置 PHY,`phy_config` 里不用设 `phy_addr`(驱动内部处理)。

## 八、验证步骤

1. 编译烧录后接网线,W5500 网口灯亮,串口出现 `W5500 ethernet init done`。
2. 链路建立后打印 `Ethernet link up`、`DHCP server started`,接入电脑应自动获得 `192.168.4.x`。
3. 电脑 ping `192.168.4.1` 通,并可通过 NAT 经 WiFi STA 上外网(复用现有 NAPT 逻辑)。
4. 若 `spi transmit failed` 或 `W5500 version mismatched`:查 SPI 接线、CS 引脚、供电(3.3V)、时钟频率。

## 九、注意事项与风险

1. **SPI 与 RMII 引脚冲突**:W5500 的 MISO/MOSI 若沿用 19/23,会与 LAN8720 的 RMII 引脚重叠;两网卡并存必须错开 SPI 引脚。
2. **总线独占**:W5500 用到的 SPI 总线不能被其它外设(`spi_bus_initialize` 同一 host)重复初始化。
3. **速率天花板**:SPI 100M 全双工下,实际吞吐受 SPI 时钟限制(20MHz SPI ≈ 十几 Mbps),够家用路由器/上网场景,但达不到有线 100M 满速。
4. **轮询模式功耗/CPU**:轮询间隔越小 CPU 占用越高;中断模式延迟更低,优先用中断(若接线允许)。

## 十、参考文件

- `components/w5500/README.md` —— 官方用法与「TCP/IP stack not used」说明
- `components/w5500/include/esp_eth_mac_w5500.h` / `esp_eth_phy_w5500.h` —— 组件接口
- `components/wiznet_common/include/wiznet_mac_common.h` —— `eth_wiznet_config_t` 字段说明
- `components/wiznet_common/src/wiznet_spi.c` —— SPI 初始化(仅 `spi_bus_add_device`,故需先 `spi_bus_initialize`)
- `CODE/src/ethernet.c` —— 现有 LAN8720 接入代码(替换点参考)
- [W5500_MACRAW模式与lwIP接入评估报告.md](W5500_MACRAW模式与lwIP接入评估报告.md)
