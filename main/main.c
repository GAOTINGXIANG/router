#include <stdio.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "wifi.h"
#include "ethernet.h"
#include "w5500_ethernet.h"

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_sta_init();          /* WiFi STA(DHCP 客户端,WAN 侧) */
    ethernet_init();          /* LAN8720 网口(DHCP 服务器,192.168.88.1) */
    w5500_ethernet_init();    /* W5500 网口(DHCP 服务器,192.168.66.1) */
}
