#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 WiFi STA,连接指定热点,IP 由 DHCP 获取
 *
 * 需在 nvs_flash_init() / esp_netif_init() / esp_event_loop_create_default() 之后调用。
 * 与以太网模块完全解耦,互不引用。
 */
void wifi_sta_init(void);

#ifdef __cplusplus
}
#endif
