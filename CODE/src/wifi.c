#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "wifi.h"

static const char *TAG = "wifi";

static esp_netif_t *s_sta_netif = NULL;

#define WIFI_SSID     "OPPOFindx7"
#define WIFI_PASSWORD "8gtx8gtx"

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_buf[16] = {0};
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_buf, sizeof(ip_buf));
        ESP_LOGI(TAG, "WiFi got IP: %s", ip_buf);

        /* 强制 WiFi STA 为默认路由,保证转发出去的包走 WAN */
        ESP_ERROR_CHECK(esp_netif_set_default_netif(s_sta_netif));
    }
}

void wifi_sta_init(void)
{
    /* 1. 创建默认 STA 网卡(默认 DHCP 客户端,IP 由热点分配) */
    s_sta_netif = esp_netif_create_default_wifi_sta();

    /* 2. 初始化 WiFi 底层栈 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 3. 注册 WiFi / IP 事件 */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    /* 4. 配置 STA 并启动 */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA started, connecting to \"%s\"", WIFI_SSID);
}
