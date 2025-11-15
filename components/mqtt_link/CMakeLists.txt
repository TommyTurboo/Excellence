idf_component_register(
    SRCS "mqtt_link.c"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES mqtt esp_event esp_netif esp_wifi
)
