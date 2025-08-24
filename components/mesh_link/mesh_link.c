// components/mesh_link/mesh_link.c
// Dunne façade: API → backend vtable (géén esp_* includes hier)
#include "mesh_link.h"

typedef struct {
    const char* (*name)(void);
    void (*init)(const mesh_opts_t*);
    void (*register_rx)(mesh_request_cb_t, mesh_event_cb_t);
    mesh_status_t (*request)(const mesh_envelope_t*, uint32_t);
    mesh_status_t (*send_event)(const mesh_envelope_t*);
    cJSON* (*snapshot)(void);
} ml_backend_t;

// Backends (alleen declaraties; implementatie zit in backends/*.c)
const ml_backend_t* ml_backend_espmesh(void);
const ml_backend_t* ml_backend_mailbox(void); // optioneel

static const ml_backend_t *B = NULL;

static inline const ml_backend_t* pick_backend(void) {
#if defined(CONFIG_USE_ESP_MESH_BACKEND)
    return ml_backend_espmesh();
#elif defined(CONFIG_ML_ENABLE_MAILBOX)
    return ml_backend_mailbox();
#else
    // Default naar ESP-MESH als er geen Kconfig is
    return ml_backend_espmesh();
#endif
}

void mesh_init(const mesh_opts_t *opts) {
    if (!B) B = pick_backend();
    B->init(opts);
}

void mesh_register_rx(mesh_request_cb_t on_request, mesh_event_cb_t on_event) {
    if (!B) B = pick_backend();
    B->register_rx(on_request, on_event);
}

mesh_status_t mesh_request(const mesh_envelope_t *req, uint32_t timeout_ms) {
    if (!B) B = pick_backend();
    return B->request(req, timeout_ms);
}

mesh_status_t mesh_send_event(const mesh_envelope_t *evt) {
    if (!B) B = pick_backend();
    return B->send_event(evt);
}

cJSON* mesh_get_routing_snapshot(void) {
    if (!B) B = pick_backend();
    return B->snapshot();
}

const char* mesh_backend_name(void) {
    if (!B) B = pick_backend();
    return B->name();
}

// Zwakke hook – implementeer in jouw mqtt_link om diag naar MQTT te publishen
__attribute__((weak)) void mesh_diag_publish_route_table(const char *event, const cJSON *snapshot) {
    (void)event; (void)snapshot;
}
