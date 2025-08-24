# components/mesh_link/CMakeLists.txt
set(srcs
    "mesh_link.c"
    "backends/backend_espmesh.c"
)

idf_component_register(
    SRCS         ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES     json
    PRIV_REQUIRES esp_event esp_wifi freertos esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
