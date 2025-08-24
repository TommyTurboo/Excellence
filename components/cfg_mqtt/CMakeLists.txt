idf_component_register(
    SRCS "cfg_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES mqtt_link parser router config_store relay_ctrl pwm_ctrl input_ctrl json
)
