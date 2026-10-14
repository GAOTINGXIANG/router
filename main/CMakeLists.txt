idf_component_register(
SRCS 
"main.c"
"../CODE/src/ethernet.c"
"../CODE/src/w5500_ethernet.c"
"../CODE/src/wifi.c"

INCLUDE_DIRS
"."
"../CODE/inc"

REQUIRES
lan87xx
w5500
nvs_flash
esp_event
esp_netif
esp_eth
esp_driver_spi
esp_wifi
)
