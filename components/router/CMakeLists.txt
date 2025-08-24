idf_component_register(
    SRCS "router.c" 
    INCLUDE_DIRS "include"
    REQUIRES parser json mesh_link parser mqtt_link
)