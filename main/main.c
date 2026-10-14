#include <stdio.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "wifi.h"
#include "w5500_ethernet.h"

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_sta_init();          /* 先启动 WiFi STA(DHCP 客户端,WAN 侧) */
    w5500_ethernet_init();    /* 再初始化 W5500(DHCP 服务器,LAN 侧) */
}
