idf_component_register(
    SRCS "cfg_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos mqtt_link parser router config_store relay_ctrl pwm_ctrl input_ctrl json
)
