// components/mesh_link/include/mesh_link.h
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { MESH_ROLE_ROOT=0, MESH_ROLE_CHILD } mesh_role_t;
typedef enum { MESH_OK=0, MESH_TIMEOUT, MESH_NO_ROUTE, MESH_ERR } mesh_status_t;
typedef enum { ML_KIND_RELAY=0, ML_KIND_PWM, ML_KIND_CONFIG, ML_KIND_INPUT, ML_KIND_DIAG } mesh_kind_t;

typedef struct {
    const char *schema;        // "v1"
    uint32_t    corr_id;       // door router ingesteld
    uint64_t    ts_ms;         // millis
    const char *src_dev;       // afzender (naam)
    const char *dst_dev;       // ontvanger (naam)
    mesh_kind_t kind;          // functionele soort
    int8_t      ttl;           // levensduur hops
    uint8_t     hop;           // huidig hops
    const char *origin_set_topic; // optioneel voor juiste MQTT State
    cJSON      *payload;       // inhoud (eigendom bij caller)
} mesh_envelope_t;

typedef void (*mesh_request_cb_t)(const mesh_envelope_t *req); // REQUEST rx
typedef void (*mesh_event_cb_t)(const mesh_envelope_t *evt);   // EVENT rx

typedef struct {
    mesh_role_t role;
    const char *local_dev;       // lokale dev_name
    uint32_t    default_timeout_ms;
    int8_t      default_ttl;
} mesh_opts_t;

void        mesh_init(const mesh_opts_t *opts);
void        mesh_register_rx(mesh_request_cb_t on_request, mesh_event_cb_t on_event);
mesh_status_t mesh_request(const mesh_envelope_t *req, uint32_t timeout_ms); // wacht op RESPONSE-ACK
mesh_status_t mesh_send_event(const mesh_envelope_t *evt);                   // fire & forget
cJSON*      mesh_get_routing_snapshot(void);
const char* mesh_backend_name(void);

// zwakke hook → implementeer in mqtt_link om diag te publiceren
__attribute__((weak)) void mesh_diag_publish_route_table(const char *event, const cJSON *snapshot);

#ifdef __cplusplus
}
#endif
