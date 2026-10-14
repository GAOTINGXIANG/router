#include <string.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_phy_lan87xx.h"
#include "ethernet.h"

static const char *TAG = "ethernet";

static esp_netif_t *s_eth_netif = NULL;
static esp_eth_handle_t s_eth_handle = NULL;

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    esp_err_t err;

    /* 只处理本网卡的事件(两个以太网口共用 ETH_EVENT,按 handle 区分) */
    if (*(esp_eth_handle_t *)event_data != s_eth_handle) {
        return;
    }

    switch (event_id) {
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2],
                 mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");

        /* 在 LAN 网卡(以太网)上启用 NAPT:入包网卡带 napt 标志才做源地址改写 */
        err = esp_netif_napt_enable(s_eth_netif);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "NAPT enabled on LAN");
        } else {
            ESP_LOGW(TAG, "NAPT enable failed: %s", esp_err_to_name(err));
        }

        err = esp_netif_dhcps_start(s_eth_netif);
        if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGI(TAG, "DHCP server already running");
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "DHCP server start failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "DHCP server started, serving 192.168.88.0/24");
        }
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet link down");
        err = esp_netif_dhcps_stop(s_eth_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            ESP_LOGW(TAG, "DHCP server stop failed: %s", esp_err_to_name(err));
        }
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

void ethernet_init(void)
{
    /* 1. 创建以太网 netif,声明为 DHCP 服务器(默认是 DHCP 客户端,不分配 dhcps 对象) */
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base_cfg.if_key = "ETH0";   /* 唯一 key,与 W5500 的 netif 区分(默认都是 ETH_DEF,会重复) */
    base_cfg.flags = (esp_netif_flags_t)(ESP_NETIF_IPV4_ONLY_FLAGS(ESP_NETIF_DHCP_SERVER) |
                                         ESP_NETIF_DEFAULT_ARP_FLAGS |
                                         ESP_NETIF_FLAG_EVENT_IP_MODIFIED |
                                         ESP_NETIF_DEFAULT_IPV6_AUTOCONFIG_FLAGS);
    esp_netif_config_t netif_cfg = {
        .base = &base_cfg,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_eth_netif = esp_netif_new(&netif_cfg);
    assert(s_eth_netif);

    /* 2. 创建 MAC:ESP32 内置 EMAC,RMII,参考时钟输入 GPIO0 */
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    assert(mac);

    /* 3. 创建 PHY:LAN8720,地址自动探测(-1 扫描 0~31),nRST 内部上拉(不用 GPIO 复位) */
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = ESP_ETH_PHY_ADDR_AUTO;
    phy_cfg.reset_gpio_num = -1;
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_cfg);
    assert(phy);

    /* 4. 安装以太网驱动 */
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &s_eth_handle));

    /* 5. 把 netif 挂载到以太网驱动 */
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, glue));

    /* 6. 注册以太网事件回调 */
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               eth_event_handler, NULL));

    /* 7. 停止 DHCP 服务器(置为 STOPPED 状态),配置本机静态 IP(192.168.88.1/24) */
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_eth_netif));

    esp_netif_ip_info_t ip_info = {
        .ip      = { .addr = ESP_IP4TOADDR(192, 168, 88, 1) },
        .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
        .gw      = { .addr = ESP_IP4TOADDR(192, 168, 88, 1) },
    };
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_eth_netif, &ip_info));

    /* 8. 给 LAN 客户端下发 DNS(两步:设 IP + 打开下发开关) */
    esp_netif_dns_info_t dns = {
        .ip = ESP_IP4ADDR_INIT(8, 8, 8, 8),   /* 可换成 114.114.114.114(国内更快) */
    };
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN, &dns));

    uint8_t offer_dns = 1;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_eth_netif, ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &offer_dns, sizeof(offer_dns)));

    /* 9. 启动以太网(链路建立后由事件回调启动 DHCP 服务器) */
    ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));

    ESP_LOGI(TAG, "Ethernet init done, static IP 192.168.88.1");
}
