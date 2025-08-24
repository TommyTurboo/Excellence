idf_component_register(
    SRCS "relay_ctrl.c"
    INCLUDE_DIRS "include"       # jouw header staat in components/relay_ctrl/include/
    REQUIRES driver esp_timer    # wat je hierbinnen gebruikt
)