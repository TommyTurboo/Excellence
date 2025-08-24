// components/mqtt_link/include/mqtt_link.h
#pragma once
// MQTT link (root of standalone): connect, subscribe, route RX → parser/config,
// TX voor Router (QoS1) + eenvoudige offline queue met TTL.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MQTT_BASE_MAX
#define MQTT_BASE_MAX 32
#endif
#ifndef MQTT_DEV_MAX
#define MQTT_DEV_MAX  32
#endif
#ifndef MQTT_ID_MAX
#define MQTT_ID_MAX   64
#endif
#ifndef MQTT_USER_MAX
#define MQTT_USER_MAX 64
#endif
#ifndef MQTT_PASS_MAX
#define MQTT_PASS_MAX 64
#endif
#ifndef MQTT_HOST_MAX
#define MQTT_HOST_MAX 64
#endif

typedef void (*mqtt_parser_entry_cb)(const char *json, const char *topic);
typedef void (*mqtt_config_entry_cb)(const char *json, const char *topic);
typedef uint64_t (*mqtt_now_ms_cb)(void);

// Context / settings
typedef struct {
    char   host[MQTT_HOST_MAX]; // bv. "192.168.1.10" of "broker.hivemq.com"
    int    port;                // 1883 / 8883
    bool   use_tls;
    const char *ca_cert_pem;    // optioneel (TLS)
    const char *client_cert_pem;// optioneel (mTLS)
    const char *client_key_pem; // optioneel (mTLS)

    char   client_id[MQTT_ID_MAX];       // default = dev_name
    char   username[MQTT_USER_MAX];      // optioneel
    char   password[MQTT_PASS_MAX];      // optioneel
    char   base_prefix[MQTT_BASE_MAX];   // default "Devices"
    char   local_dev[MQTT_DEV_MAX];      // bv. "ESP32_ROOT"
    int    keepalive_s;                  // bv. 30

    // reconnect backoff
    uint32_t backoff_min_ms; // bv. 500
    uint32_t backoff_max_ms; // bv. 5000

    // offline queue
    int      offline_max_items; // bv. 16
    uint32_t offline_ttl_ms;    // bv. 30000

    bool   is_root; // root subscribe: base/+/Cmd/Set (+ Config/Set)
} mqtt_ctx_t;

// Callbacks naar jouw app
typedef struct {
    mqtt_parser_entry_cb   parser_entry;     // voor .../Cmd/Set
    mqtt_config_entry_cb   config_set_entry; // voor .../Config/Set
    mqtt_now_ms_cb         now_ms;           // optioneel (fallback = esp_timer_get_time)
} mqtt_cbs_t;

// API
void mqtt_link_init(const mqtt_ctx_t *ctx, const mqtt_cbs_t *cbs);
bool mqtt_link_connected(void);

// Publish vanuit Router (QoS1). Retourneert true als geaccepteerd (direct of queued).
bool mqtt_link_publish(const char *topic, const char *payload, int qos, bool retain);

void mqtt_link_publish_cb(const char *topic, const char *payload, int qos, bool retain);
// Handige wrapper met dezelfde signatuur als Router’s callback (void)
// → je kan deze rechtstreeks aan router_cbs_t.mqtt_pub hangen.
void mqtt_link_publish_cb(const char *topic, const char *payload, int qos, bool retain);

#ifdef __cplusplus
}
#endif
