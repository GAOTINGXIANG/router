#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化以太网(LAN8720),配置本机静态 IP 并作为 DHCP 服务器
 *
 * 链路建立后自动启动 DHCP 服务器,为接入设备分配同子网地址。
 */
void ethernet_init(void);

#ifdef __cplusplus
}
#endif
