#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "parser.h"   // voor parser_msg_t + helpers
#include "mesh_link.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ROUTER_OK = 0,
  ROUTER_ERR_INVALID,
  ROUTER_ERR_OUT_OF_RANGE,
  ROUTER_ERR_NO_ROUTE,
  ROUTER_ERR_TIMEOUT,
  ROUTER_ERR_INTERNAL
} router_status_t;

// Publish naar MQTT (bv. mqtt_link_publish_cb)
typedef void (*router_pub_fn)(const char *topic, const char *payload, int qos, bool retain);

// Driver-exec callbacks (door app ingevuld)
typedef router_status_t (*router_exec_relay_fn)(const parser_msg_t *m);
typedef router_status_t (*router_exec_pwm_fn)  (const parser_msg_t *m, /*out*/int *applied_pct);
typedef router_status_t (*router_exec_input_fn)(const parser_msg_t *m, /*out*/int *value);

typedef struct {
  router_pub_fn         mqtt_pub;
  router_exec_relay_fn  exec_relay;
  router_exec_pwm_fn    exec_pwm;
  router_exec_input_fn  exec_input;
} router_cbs_t;

// API
void            router_init(const router_cbs_t *cbs);
void            router_set_local_dev(const char *dev_name);
router_status_t router_handle(const parser_msg_t *msg);

void router_handle_mesh_request(const mesh_envelope_t *req);
void router_handle_mesh_event(const mesh_envelope_t *evt);

// Optioneel helper voor Set→remote pad
void router_send_cmd_to_target(const char *target_dev,
                               const char *origin_set_topic,
                               mesh_kind_t kind,
                               const cJSON *payload,
                               uint32_t corr_id);

// voor drivers die EVENT willen uitsturen (child)
void router_emit_event(mesh_kind_t kind, uint32_t corr_id,
                       const char *origin_set_topic, const cJSON *state_payload);

// (mag ook in .h staan; of laat hem 'extern' weg en maak hem static in router.c)
void derive_state_topic(const char *set_topic, char *out, size_t out_sz);


#ifdef __cplusplus
}
#endif
