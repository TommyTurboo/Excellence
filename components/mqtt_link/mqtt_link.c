// components/mqtt_link/mqtt_link.c
#include "mqtt_link.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "mqtt_link";

typedef struct {
    char   *topic;
    char   *payload;
    int     qos;
    bool    retain;
    uint64_t expire_ts;
} queued_msg_t;

static mqtt_ctx_t G;
static mqtt_cbs_t C;
static esp_mqtt_client_handle_t g_client = NULL;
static volatile bool g_connected = false;

// eenvoudige ringbuffer
static queued_msg_t *Q = NULL;
static int Q_cap = 0, Q_head = 0, Q_tail = 0, Q_count = 0;

static inline uint64_t now_ms_fallback(void){
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}
static inline uint64_t now_ms(void){
    return C.now_ms ? C.now_ms() : now_ms_fallback();
}

static void queue_free_item(queued_msg_t *m){
    if (!m) return;
    free(m->topic); m->topic = NULL;
    free(m->payload); m->payload = NULL;
}

static bool queue_push(const char *topic, const char *payload, int qos, bool retain){
    if (!Q || Q_cap <= 0) return false;
    if (Q_count == Q_cap) {
        // drop oudste
        queue_free_item(&Q[Q_head]);
        Q_head = (Q_head + 1) % Q_cap;
        Q_count--;
        ESP_LOGW(TAG, "offline queue full → dropped oldest");
    }
    int idx = Q_tail;
    Q[idx].topic   = strdup(topic);
    Q[idx].payload = strdup(payload);
    Q[idx].qos     = qos;
    Q[idx].retain  = retain;
    Q[idx].expire_ts = now_ms() + G.offline_ttl_ms;
    Q_tail = (Q_tail + 1) % Q_cap;
    Q_count++;
    return true;
}

static void queue_flush_if_connected(void){
    if (!g_connected || !g_client || Q_count == 0) return;
    int n = Q_count;
    for (int i=0; i<n; ++i){
        int idx = Q_head;
        queued_msg_t *m = &Q[idx];
        if (now_ms() > m->expire_ts) {
            ESP_LOGW(TAG, "drop expired queued msg to %s", m->topic);
            queue_free_item(m);
        } else {
            int msg_id = esp_mqtt_client_publish(g_client, m->topic, m->payload, 0, m->qos, m->retain);
            if (msg_id < 0) {
                ESP_LOGW(TAG, "publish failed (still offline?) → keep queued");
                // stop flush attempt
                return;
            }
            ESP_LOGI(TAG, "flushed queued → [%s] (%d)", m->topic, msg_id);
            queue_free_item(m);
        }
        Q_head = (Q_head + 1) % Q_cap;
        Q_count--;
    }
}

static void publish_online_status(bool online){
    if (!g_client) return;
    char topic[128];
    const char *base = (G.base_prefix[0] ? G.base_prefix : "Devices");
    snprintf(topic, sizeof(topic), "%s/%s/Status", base, G.local_dev);
    char payload[128];
    if (online) {
        snprintf(payload, sizeof(payload), "{\"status\":\"online\",\"dev\":\"%s\"}", G.local_dev);
    } else {
        snprintf(payload, sizeof(payload), "{\"status\":\"offline\"}");
    }
    int id = esp_mqtt_client_publish(g_client, topic, payload, 0, 1, true);
    ESP_LOGI(TAG, "status %s -> id=%d", online?"online":"offline", id);
}

static void do_subscriptions(void){
    if (!g_connected || !g_client) return;
    const char *base = (G.base_prefix[0] ? G.base_prefix : "Devices");
    char topic1[160], topic2[160];

    if (G.is_root) {
        snprintf(topic1, sizeof(topic1), "%s/+/Cmd/Set", base);
        snprintf(topic2, sizeof(topic2), "%s/+/Config/Set", base);
    } else {
        snprintf(topic1, sizeof(topic1), "%s/%s/Cmd/Set", base, G.local_dev);
        snprintf(topic2, sizeof(topic2), "%s/%s/Config/Set", base, G.local_dev);
    }
    int s1 = esp_mqtt_client_subscribe(g_client, topic1, 1);
    int s2 = esp_mqtt_client_subscribe(g_client, topic2, 1);
    ESP_LOGI(TAG, "subscribed: %s (%d), %s (%d)", topic1, s1, topic2, s2);
}

static bool topic_endswith(const char *topic, const char *suffix){
    size_t nt = strlen(topic), ns = strlen(suffix);
    if (ns > nt) return false;
    return (strcasecmp(topic + (nt - ns), suffix) == 0);
}

static void route_rx_message(const char *topic, const char *data){
    if (!topic || !data) return;
    if (topic_endswith(topic, "/Config/Set")) {
        if (C.config_set_entry) C.config_set_entry(data, topic);
        return;
    }
    if (topic_endswith(topic, "/Cmd/Set")) {
        if (C.parser_entry) C.parser_entry(data, topic);
        return;
    }
    // andere topics: stil houden (of loggen)
    ESP_LOGD(TAG, "ignored RX topic: %s", topic);
}

// MQTT event handler
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data){
    (void)handler_args; (void)base;
    esp_mqtt_event_handle_t e = event_data;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        g_connected = true;
        ESP_LOGI(TAG, "CONNECTED");
        publish_online_status(true);
        do_subscriptions();
        queue_flush_if_connected();
        break;
    case MQTT_EVENT_DISCONNECTED:
        g_connected = false;
        ESP_LOGW(TAG, "DISCONNECTED");
        // LWT wordt door broker verzonden bij onverwachte disconnect; wij sturen zelf niets hier
        break;
    case MQTT_EVENT_DATA: {
        // Maak null-terminated kopieën
        char *t = (char*)malloc(e->topic_len + 1);
        char *d = (char*)malloc(e->data_len + 1);
        if (!t || !d) { free(t); free(d); break; }
        memcpy(t, e->topic, e->topic_len); t[e->topic_len] = 0;
        memcpy(d, e->data,  e->data_len);  d[e->data_len]  = 0;
        ESP_LOGI(TAG, "RX [%s] %.*s", t, (int)(e->data_len>512?512:e->data_len), d);
        route_rx_message(t, d);
        free(t); free(d);
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "EVENT_ERROR (transport)");
        break;
    case MQTT_EVENT_SUBSCRIBED:
    case MQTT_EVENT_UNSUBSCRIBED:
    case MQTT_EVENT_PUBLISHED:
    default:
        break;
    }
}

void mqtt_link_init(const mqtt_ctx_t *ctx, const mqtt_cbs_t *cbs){
    memset(&G, 0, sizeof(G));
    memset(&C, 0, sizeof(C));
    if (ctx) G = *ctx;
    if (cbs) C = *cbs;

    if (G.base_prefix[0] == '\0') strlcpy(G.base_prefix, "Devices", sizeof(G.base_prefix));
    if (G.keepalive_s <= 0) G.keepalive_s = 30;
    if (G.backoff_min_ms == 0) G.backoff_min_ms = 500;
    if (G.backoff_max_ms == 0) G.backoff_max_ms = 5000;
    if (G.offline_max_items <= 0) G.offline_max_items = 16;
    if (G.offline_ttl_ms == 0) G.offline_ttl_ms = 30000;

    // offline queue alloc
    Q_cap = G.offline_max_items;
    Q = (queued_msg_t*)calloc(Q_cap, sizeof(queued_msg_t));

    // LWT payloads
    char lwt_topic[128];
    const char *base = (G.base_prefix[0] ? G.base_prefix : "Devices");
    snprintf(lwt_topic, sizeof(lwt_topic), "%s/%s/Status", base, G.local_dev);
    static const char *LWT_OFFLINE = "{\"status\":\"offline\"}";

    esp_mqtt_client_config_t cfg = {
        .broker.address.hostname = G.host,                       // <-- host
        .broker.address.port     = (G.port>0? G.port : (G.use_tls?8883:1883)),
        .broker.address.transport= G.use_tls ? MQTT_TRANSPORT_OVER_SSL
                                            : MQTT_TRANSPORT_OVER_TCP,  // <-- transport
        .session.keepalive       = G.keepalive_s,
        .credentials.client_id   = (G.client_id[0]? G.client_id : NULL),
        .credentials.username    = (G.username[0]? G.username  : NULL),
        .credentials.authentication.password =
                                (G.password[0]? G.password : NULL),
        .network.disable_auto_reconnect = false,
        .session.protocol_ver    = MQTT_PROTOCOL_V_3_1_1,
        .session.last_will.topic = lwt_topic,
        .session.last_will.msg   = LWT_OFFLINE,
        .session.last_will.msg_len = (int)strlen(LWT_OFFLINE),
        .session.last_will.retain = true,
        .session.last_will.qos    = 1,
    };

    #if ESP_IDF_VERSION_MAJOR >= 5
    if (G.use_tls) {
        cfg.broker.verification.certificate = G.ca_cert_pem;
        cfg.credentials.authentication.certificate = G.client_cert_pem;
        cfg.credentials.authentication.key = G.client_key_pem;
    }
    #endif

    g_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(g_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(g_client);
}

bool mqtt_link_connected(void){
    return g_connected;
}

bool mqtt_link_publish(const char *topic, const char *payload, int qos, bool retain){
    if (!topic || !payload) return false;
    if (g_connected && g_client) {
        int id = esp_mqtt_client_publish(g_client, topic, payload, 0, qos, retain);
        if (id >= 0) return true;
        ESP_LOGW(TAG, "publish failed (id=%d) → queueing", id);
    }
    // offline of publish fail → queue
    return queue_push(topic, payload, qos, retain);
}

void mqtt_link_publish_cb(const char *topic, const char *payload, int qos, bool retain){
    if (!g_client) return;
    int mid = esp_mqtt_client_publish(g_client, topic, payload, 0, qos, retain);
    ESP_LOGI(TAG, "TX [%s] id=%d %s", topic, mid, payload ? payload : "");
}
