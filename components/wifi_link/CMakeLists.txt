idf_component_register(
    SRCS "wifi_link.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash 
)
