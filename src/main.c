// src/main.c
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_random.h"

#include "wifi_link.h"
#include "mqtt_link.h"
#include "parser.h"
#include "router.h"
#include "cfg_mqtt.h"

#include "config_store.h"
#include "relay_ctrl.h"
#include "pwm_ctrl.h"
#include "input_ctrl.h"
#include "cJSON.h"

// ---- Build-time secrets (met defaults) ----
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef MQTT_HOST
#define MQTT_HOST ""
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_CLIENT_ID
#define MQTT_CLIENT_ID "ESP32_NODE"
#endif
#ifndef MQTT_USER
#define MQTT_USER ""
#endif
#ifndef MQTT_PASS
#define MQTT_PASS ""
#endif
#ifndef MQTT_BASE_PREFIX
#define MQTT_BASE_PREFIX "Devices"
#endif

// --------------------------------------------------
// Helpers
// --------------------------------------------------
static void gen_corr_id_app(char out[48]){
    uint32_t r0=esp_random(), r1=esp_random(), r2=esp_random(), r3=esp_random();
    snprintf(out, 48,
        "%08" PRIx32 "-%04" PRIx16 "-%04" PRIx16 "-%04" PRIx16 "-%08" PRIx32,
        r0, (uint16_t)(r1 & 0xFFFF), (uint16_t)((r1>>16) & 0xFFFF), (uint16_t)(r2 & 0xFFFF), r3);
}

static void publish_parse_error(const parser_result_t *r, const char *local_dev){
    char corr[48];
    const char *cid = (r->msg.corr_id[0] ? r->msg.corr_id : (gen_corr_id_app(corr), corr));

    char topic[96];
    snprintf(topic, sizeof(topic), MQTT_BASE_PREFIX "/%s/State", local_dev ? local_dev : "");

    // JSON veilig opbouwen zonder vaste buffer
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "corr_id", cid ? cid : "");
    cJSON_AddStringToObject(root, "dev",     local_dev ? local_dev : "");
    cJSON_AddStringToObject(root, "status",  "ERROR");
    cJSON_AddStringToObject(root, "code",    parser_err_str(r->error.code));
    cJSON_AddStringToObject(root, "path",   (r->error.path[0]   ? r->error.path   : ""));
    cJSON_AddStringToObject(root, "detail", (r->error.detail[0] ? r->error.detail : ""));

    char *payload = cJSON_PrintUnformatted(root);   // alloc
    mqtt_link_publish_cb(topic, payload ? payload : "{}", 1, false);
    if (payload) free(payload);
    cJSON_Delete(root);
}

// --------------------------------------------------
// Router exec-callbacks
// --------------------------------------------------
static const char *TAG_EXEC = "EXEC";

static router_status_t exec_relay(const parser_msg_t *m){
    const cfg_t *cfg = config_get_cached();
    if (!cfg) return ROUTER_ERR_INTERNAL;
    if (m->io_id < 0 || m->io_id >= cfg->relay_count) return ROUTER_ERR_OUT_OF_RANGE;

    switch (m->action) {
        case ACT_ON:     relay_ctrl_on(m->io_id);  break;
        case ACT_OFF:    relay_ctrl_off(m->io_id); break;
        case ACT_TOGGLE: relay_ctrl_toggle(m->io_id); break;
        default: return ROUTER_ERR_INVALID;
    }
    if (m->params.has_duration_ms) {
        uint32_t sec = (m->params.duration_ms + 999) / 1000; // ms→s
        relay_ctrl_set_autoff_seconds(m->io_id, sec);
    }
    ESP_LOGI(TAG_EXEC, "RELAY ch=%d -> %s", m->io_id, parser_action_str(m->action));
    return ROUTER_OK;
}

static router_status_t exec_pwm(const parser_msg_t *m, int *applied_pct){
    const cfg_t *cfg = config_get_cached();
    if (!cfg) return ROUTER_ERR_INTERNAL;
    if (m->io_id < 0 || m->io_id >= cfg->pwm_count) return ROUTER_ERR_OUT_OF_RANGE;
    if (!m->params.has_brightness_pct) return ROUTER_ERR_INVALID;

    int pct = m->params.brightness_pct;
    if (pct < 0 || pct > 100) return ROUTER_ERR_OUT_OF_RANGE;

    uint32_t max = (1u<<13) - 1;
    uint32_t duty = (uint32_t)((pct * (int)max + 50) / 100);
    if (m->params.has_ramp_ms && m->params.ramp_ms > 0)
        pwm_ctrl_fade_to(m->io_id, duty, (uint32_t)m->params.ramp_ms);
    else
        pwm_ctrl_set_duty(m->io_id, duty);

    if (applied_pct) *applied_pct = pct;
    ESP_LOGI(TAG_EXEC, "PWM ch=%d -> %d%%", m->io_id, pct);
    return ROUTER_OK;
}

static router_status_t exec_input(const parser_msg_t *m, int *value){
    const cfg_t *cfg = config_get_cached();
    if (!cfg) return ROUTER_ERR_INTERNAL;
    if (m->io_id < 0 || m->io_id >= cfg->input_count) return ROUTER_ERR_OUT_OF_RANGE;
    bool lvl = input_ctrl_get_level(m->io_id);
    if (value) *value = lvl ? 1 : 0;
    ESP_LOGI(TAG_EXEC, "INPUT ch=%d -> %d", m->io_id, (int)lvl);
    return ROUTER_OK;
}

static void hook_router_init(const char *local_dev_name){
    router_cbs_t rcbs = {
        .mqtt_pub   = mqtt_link_publish_cb,
        .exec_relay = exec_relay,
        .exec_pwm   = exec_pwm,
        .exec_input = exec_input,
    };
    router_init(&rcbs);
    router_set_local_dev(local_dev_name);
}

// --------------------------------------------------
// MQTT RX handlers
// --------------------------------------------------
static void on_cmd_set(const char *json, const char *topic) {
    ESP_LOGI("MQ_RX","topic=%s json=%s", topic, json);
    parser_meta_t meta = { .source=PARSER_SRC_MQTT, .topic_hint=topic };
    parser_result_t r = parser_parse(json, &meta);
    if (!r.ok) { publish_parse_error(&r, MQTT_CLIENT_ID); return; }
    (void)router_handle(&r.msg);
}

// light helper to read target_dev for forwarding
static const char* read_target_dev(const char *json, char *buf, size_t n){
    cJSON *root = cJSON_Parse(json);
    if (!root) return NULL;
    const char *res = NULL;
    cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "target_dev");
    if (cJSON_IsString(t) && t->valuestring && t->valuestring[0]) {
        snprintf(buf, n, "%s", t->valuestring);
        res = buf;
    }
    cJSON_Delete(root);
    return res;
}

static void on_cfg_set(const char *json, const char *topic) {
    (void)topic;
    // Root-forwarding: als target_dev aanwezig en ≠ local → forward
    char dev[32] = {0};
    const char *target = read_target_dev(json, dev, sizeof dev);
    if (target && strcmp(target, MQTT_CLIENT_ID) != 0) {
        char fwd_topic[96];
        snprintf(fwd_topic, sizeof(fwd_topic), MQTT_BASE_PREFIX "/%s/Config/Set", target);
        ESP_LOGI("CFG_RX", "forward → %s", fwd_topic);
        mqtt_link_publish_cb(fwd_topic, json, 1, false);
        return;
    }
    // Anders: lokaal toepassen (full config apply + ACK/ERROR)
    cfg_mqtt_handle(json, MQTT_CLIENT_ID);
}

// --------------------------------------------------
// Wi-Fi → MQTT boot
// --------------------------------------------------
static void on_down(void){ /* optioneel: mqtt cleanup/notify */ }

static void on_ip(void){
    mqtt_ctx_t m = {0};
    strlcpy(m.host, MQTT_HOST, sizeof m.host);
    m.port = MQTT_PORT;
    strlcpy(m.base_prefix, MQTT_BASE_PREFIX, sizeof m.base_prefix);
    strlcpy(m.local_dev,  MQTT_CLIENT_ID,   sizeof m.local_dev);
    strlcpy(m.client_id,  MQTT_CLIENT_ID,   sizeof m.client_id);
    strlcpy(m.username,   MQTT_USER,        sizeof m.username);
    strlcpy(m.password,   MQTT_PASS,        sizeof m.password);
    m.is_root = true;  // root: +/Cmd/Set & +/Config/Set; node: zet false
    mqtt_cbs_t cbs = { .parser_entry=on_cmd_set, .config_set_entry=on_cfg_set, .now_ms=NULL };
    mqtt_link_init(&m, &cbs);

    hook_router_init(MQTT_CLIENT_ID);
}

// --------------------------------------------------
// app_main
// --------------------------------------------------
void app_main(void){
    // 1) Config + NVS
    ESP_ERROR_CHECK(config_init());
    const cfg_t *cfg = config_get_cached();

    // 2) Drivers init met config
    relay_ctrl_init(cfg->relay_gpio, cfg->relay_count,
                    cfg->relay_active_low_mask, cfg->relay_open_drain_mask);
    for (int i=0; i<cfg->relay_count; ++i)
        relay_ctrl_set_autoff_seconds(i, cfg->relay_autoff_sec[i]);

    pwm_ctrl_init(cfg->pwm_gpio, cfg->pwm_count, cfg->pwm_inverted_mask, cfg->pwm_freq_hz);

    input_ctrl_init(cfg->input_gpio, cfg->input_count,
                    cfg->input_pullup_mask, cfg->input_pulldown_mask,
                    cfg->input_inverted_mask, 30);
    for (int i=0; i<cfg->input_count; ++i)
        input_ctrl_set_debounce_ms(i, cfg->input_debounce_ms[i]);

    // 3) Wi-Fi start
    wifi_ctx_t w = (wifi_ctx_t){0};
    strlcpy(w.ssid, WIFI_SSID, sizeof w.ssid);
    strlcpy(w.pass, WIFI_PASS, sizeof w.pass);
    strlcpy(w.hostname, MQTT_CLIENT_ID, sizeof w.hostname); // handig als hostnaam
    w.power_save = false;

    wifi_cbs_t cb = { .on_got_ip = on_ip, .on_disconnected = on_down };
    wifi_link_init(&w, &cb);
    wifi_link_start();
}



// // ------ WIFI Test

// #include "wifi_link.h"
// #include "mqtt_link.h"
// #include "string.h"
// #include "mqtt_link.h"
// #include "parser.h"
// #include "router.h"
// #include "esp_log.h"
// #include "relay_ctrl.h"
// #include "pwm_ctrl.h"
// #include "input_ctrl.h"
// #include "config_store.h"

// #ifndef WIFI_SSID_DEFAULT
// #define WIFI_SSID_DEFAULT ""
// #endif
// #ifndef WIFI_PASS_DEFAULT
// #define WIFI_PASS_DEFAULT ""
// #endif
// #ifndef WIFI_SSID
// #define WIFI_SSID WIFI_SSID_DEFAULT
// #endif
// #ifndef WIFI_PASS
// #define WIFI_PASS WIFI_PASS_DEFAULT
// #endif
// #ifndef MQTT_HOST
// #define MQTT_HOST ""
// #endif
// #ifndef MQTT_PORT
// #define MQTT_PORT 1883
// #endif
// #ifndef MQTT_CLIENT_ID
// #define MQTT_CLIENT_ID "ESP32"
// #endif
// #ifndef MQTT_USER
// #define MQTT_USER ""
// #endif
// #ifndef MQTT_PASS
// #define MQTT_PASS ""
// #endif

// static void on_down(void){ /* optioneel: mqtt stop/notify */ }

// static const char *TAG_EXEC = "EXEC";

// static router_status_t exec_relay(const parser_msg_t *m){
//     const cfg_t *cfg = config_get_cached();
//     if (!cfg) return ROUTER_ERR_INTERNAL;
//     if (m->io_id < 0 || m->io_id >= cfg->relay_count) return ROUTER_ERR_OUT_OF_RANGE;

//     switch (m->action) {
//         case ACT_ON:     relay_ctrl_on(m->io_id);  break;
//         case ACT_OFF:    relay_ctrl_off(m->io_id); break;
//         case ACT_TOGGLE: relay_ctrl_toggle(m->io_id); break;
//         default: return ROUTER_ERR_INVALID;
//     }
//     if (m->params.has_duration_ms) {
//         uint32_t sec = (m->params.duration_ms + 999) / 1000; // afronden
//         relay_ctrl_set_autoff_seconds(m->io_id, sec);
//     }
//     ESP_LOGI(TAG_EXEC, "RELAY ch=%d -> %s", m->io_id, parser_action_str(m->action));
//     return ROUTER_OK;
// }

// static router_status_t exec_pwm(const parser_msg_t *m, int *applied_pct){
//     const cfg_t *cfg = config_get_cached();
//     if (!cfg) return ROUTER_ERR_INTERNAL;
//     if (m->io_id < 0 || m->io_id >= cfg->pwm_count) return ROUTER_ERR_OUT_OF_RANGE;
//     if (!m->params.has_brightness_pct) return ROUTER_ERR_INVALID;

//     int pct = m->params.brightness_pct;
//     if (pct < 0 || pct > 100) return ROUTER_ERR_OUT_OF_RANGE;

//     uint32_t max = (1u<<13) - 1;
//     uint32_t duty = (uint32_t)((pct * (int)max + 50) / 100);
//     if (m->params.has_ramp_ms && m->params.ramp_ms > 0)
//         pwm_ctrl_fade_to(m->io_id, duty, (uint32_t)m->params.ramp_ms);
//     else
//         pwm_ctrl_set_duty(m->io_id, duty);

//     if (applied_pct) *applied_pct = pct;
//     ESP_LOGI(TAG_EXEC, "PWM ch=%d -> %d%%", m->io_id, pct);
//     return ROUTER_OK;
// }

// static router_status_t exec_input(const parser_msg_t *m, int *value){
//     const cfg_t *cfg = config_get_cached();
//     if (!cfg) return ROUTER_ERR_INTERNAL;
//     if (m->io_id < 0 || m->io_id >= cfg->input_count) return ROUTER_ERR_OUT_OF_RANGE;

//     bool lvl = input_ctrl_get_level(m->io_id);
//     if (value) *value = lvl ? 1 : 0;
//     ESP_LOGI(TAG_EXEC, "INPUT ch=%d -> %d", m->io_id, (int)lvl);
//     return ROUTER_OK;
// }

// // tijdens init, nadat mqtt_link_init() je client-id/naam kent:
// static void hook_router_init(const char *local_dev_name){
//     router_cbs_t rcbs = {
//         .mqtt_pub   = mqtt_link_publish_cb, // uit jouw mqtt_link
//         .exec_relay = exec_relay,
//         .exec_pwm   = exec_pwm,
//         .exec_input = exec_input,
//     };
//     router_init(&rcbs);
//     router_set_local_dev(MQTT_CLIENT_ID); // bv. jouw MQTT_CLIENT_ID
// }

// static void on_cmd_set(const char *json, const char *topic) {
//     ESP_LOGI("MQ_RX","topic=%s json=%s", topic, json);
//     parser_meta_t meta = { .source=PARSER_SRC_MQTT, .topic_hint=topic };
//     parser_result_t r = parser_parse(json, &meta);
//     if (!r.ok) {
//         ESP_LOGW("MQ_RX","parse error: %s path=%s detail=%s",
//                  parser_err_str(r.error.code), r.error.path, r.error.detail);
//         return;
//     }
//     (void)router_handle(&r.msg);
// }

// static void on_cfg_set(const char *json, const char *topic) { (void)json; (void)topic; }

// static void on_ip(void){
//     mqtt_ctx_t m = {0};
//     strlcpy(m.host, MQTT_HOST, sizeof m.host);
//     m.port = MQTT_PORT;
//     strlcpy(m.local_dev, MQTT_CLIENT_ID, sizeof m.local_dev);
//     strlcpy(m.client_id, MQTT_CLIENT_ID, sizeof m.client_id);
//     strlcpy(m.username, MQTT_USER, sizeof m.username);
//     strlcpy(m.password, MQTT_PASS, sizeof m.password);
//     m.is_root = true;           // of false voor non-root
//     mqtt_cbs_t cbs = { .parser_entry=on_cmd_set, .config_set_entry=on_cfg_set, .now_ms=NULL };
//     mqtt_link_init(&m, &cbs);
//     hook_router_init(MQTT_CLIENT_ID);
// }

// void app_main(void){

//     // 1) Config + NVS
//     ESP_ERROR_CHECK(config_init());
//     const cfg_t *cfg = config_get_cached();

//     // 2) Drivers starten met config
//     relay_ctrl_init(cfg->relay_gpio, cfg->relay_count,
//                     cfg->relay_active_low_mask, cfg->relay_open_drain_mask);
//     for (int i=0; i<cfg->relay_count; ++i) {
//         relay_ctrl_set_autoff_seconds(i, cfg->relay_autoff_sec[i]);
//     }

//     pwm_ctrl_init(cfg->pwm_gpio, cfg->pwm_count, cfg->pwm_inverted_mask, cfg->pwm_freq_hz);

//     input_ctrl_init(cfg->input_gpio, cfg->input_count,
//                     cfg->input_pullup_mask, cfg->input_pulldown_mask,
//                     cfg->input_inverted_mask, /*debounce_ms_def=*/30);
//     for (int i=0; i<cfg->input_count; ++i) {
//         input_ctrl_set_debounce_ms(i, cfg->input_debounce_ms[i]);
//     }


//     wifi_ctx_t w = (wifi_ctx_t){0};
//     strlcpy(w.ssid, WIFI_SSID, sizeof w.ssid);
//     strlcpy(w.pass, WIFI_PASS, sizeof w.pass);
//     strncpy(w.hostname, "esp32-node", sizeof w.hostname-1);
//     w.power_save = false;

//     wifi_cbs_t cb = { .on_got_ip = on_ip, .on_disconnected = on_down };
//     wifi_link_init(&w, &cb);
//     wifi_link_start();
// }

// ----------- TEST PARSER MET ROUTER

// // src/main.c  (Router + Parser demo)
// #include <stdio.h>
// #include <string.h>
// #include "esp_log.h"
// #include "parser.h"
// #include "router.h"
// #include "cJSON.h"

// static const char *TAG = "ROUTER_DEMO";
// #define LOCAL_DEV "ESP32_ROOT"

// // -------- stubs / callbacks --------
// static const char *KNOWN[] = { LOCAL_DEV, "ESP32_A", "ESP32_B", "ESP32_C", NULL };

// static bool registry_has(const char *dev){
//     for (int i=0; KNOWN[i]; ++i) if (strcmp(KNOWN[i], dev)==0) return true;
//     return false;
// }

// static void mqtt_pub(const char *topic, const char *payload, int qos, bool retain){
//     (void)qos; (void)retain;
//     ESP_LOGI("MQTT", "[%s] %s", topic, payload);
// }

// static bool exec_relay(int id, action_t act, const parser_params_t *p){
//     ESP_LOGI("DRV", "RELAY id=%d act=%s dur_ms=%s%d", id, parser_action_str(act),
//              p->has_duration_ms ? "" : "(n/a) ", p->has_duration_ms ? p->duration_ms : 0);
//     // hier zou je relay_ctrl_* aanroepen; return false bij unsupported
//     return (act==ACT_ON || act==ACT_OFF || act==ACT_TOGGLE);
// }
// static bool exec_pwm(int id, action_t act, const parser_params_t *p){
//     ESP_LOGI("DRV", "PWM   id=%d act=%s pct=%s%d ramp=%s%d",
//              id, parser_action_str(act),
//              p->has_brightness_pct?"":"(n/a) ", p->has_brightness_pct?p->brightness_pct:0,
//              p->has_ramp_ms?"":"(n/a) ", p->has_ramp_ms?p->ramp_ms:0);
//     return (act==ACT_ON || act==ACT_OFF || act==ACT_SET);
// }
// static bool exec_input(int id, action_t act, const parser_params_t *p){
//     (void)p; ESP_LOGI("DRV", "INPUT id=%d act=%s", id, parser_action_str(act));
//     return (act==ACT_READ);
// }

// // Simuleer mesh roundtrip: succes voor bekende devices, timeout voor speciale naam
// static bool mesh_request(const parser_msg_t *msg, void **reply, uint32_t timeout_ms){
//     (void)timeout_ms;
//     if (!registry_has(msg->target_dev) || strcmp(msg->target_dev, "ESP32_TIMEOUT")==0) return false;
//     // optionele extra’s die in State-payload worden bijgeplakt
//     cJSON *extra = cJSON_CreateObject();
//     cJSON_AddStringToObject(extra, "via", "mesh");
//     cJSON_AddNumberToObject(extra, "hop_count", 1);
//     *reply = extra;
//     ESP_LOGI("MESH", "-> %s (corr=%s)", msg->target_dev, msg->corr_id);
//     return true;
// }

// // -------- kleine runner --------
// static void run_route(const char *title, const char *json, const char *topic){
//     ESP_LOGI(TAG, "CASE: %s", title);
//     ESP_LOGI(TAG, "JSON: %s", json);

//     parser_meta_t meta = { .source = topic?PARSER_SRC_MQTT:PARSER_SRC_LOCAL,
//                            .topic_hint = topic, .received_ts_ms = 123456789 };
//     parser_result_t r = parser_parse(json, &meta);

//     if (r.unknown_keys[0]) ESP_LOGW(TAG, "Unknown fields: %s", r.unknown_keys);

//     if (!r.ok) {
//         ESP_LOGE(TAG, "PARSER ERR: %s path=%s detail=%s",
//                  parser_err_str(r.error.code), r.error.path, r.error.detail);
//         return;
//     }
//     router_status_t st = router_handle(&r.msg);
//     ESP_LOGI(TAG, "router_status=%d\n", st);
// }

// void app_main(void){
//     parser_init();

//     router_ctx_t ctx = {
//         .local_dev = LOCAL_DEV,
//         .is_root = true,              // zet false om child-gedrag te zien
//         .mesh_timeout_ms = 1500
//     };
//     router_cbs_t cbs = {
//         .exec_relay = exec_relay,
//         .exec_pwm   = exec_pwm,
//         .exec_input = exec_input,
//         .mesh_request = mesh_request,
//         .mqtt_pub   = mqtt_pub,
//         .now_ms     = NULL,
//         .registry_has = registry_has
//     };
//     router_init(&ctx, &cbs);

//     // --- tests ---
//     run_route("LOCAL relay ON → exec + State",
//         "{ \"target_dev\":\"" LOCAL_DEV "\", \"io_kind\":\"RELAY\", \"io_id\":1, \"action\":\"ON\", \"minutes\":1 }",
//         "Garden/LO/Switch/Set");

//     run_route("REMOTE relay OFF (known) → mesh + State",
//         "{ \"target_dev\":\"ESP32_C\", \"io_kind\":\"RELAY\", \"io_id\":2, \"action\":\"OFF\" }",
//         "Garden/LO/Switch/Set");

//     run_route("REMOTE PWM SET (known) → mesh + State",
//         "{ \"target_dev\":\"ESP32_A\", \"io_kind\":\"PWM\", \"io_id\":0, \"action\":\"SET\", \"brightness\":42, \"ramp\":300 }",
//         "Lights/Hall/Set");

//     run_route("UNKNOWN target (root) → ERROR",
//         "{ \"target_dev\":\"ESP32_XXX\", \"io_kind\":\"RELAY\", \"io_id\":0, \"action\":\"ON\" }",
//         "Devices/ESP32_XXX/Cmd/Set");

//     run_route("REMOTE timeout (simulated) → TIMEOUT",
//         "{ \"target_dev\":\"ESP32_TIMEOUT\", \"io_kind\":\"RELAY\", \"io_id\":0, \"action\":\"ON\" }",
//         "Any/Set");
// }





// ------------ TEST PARSER

// #include <stdio.h>
// #include "esp_log.h"
// #include "parser.h"

// static const char *TAG = "PARSER_DEMO";

// static void dump_msg(const parser_msg_t *m) {
//     ESP_LOGI(TAG, "---- Canoniek Msg ----");
//     ESP_LOGI(TAG, "type=%s  source=%d  ts=%llu",
//              parser_msgtype_str(m->type), (int)m->meta.source, (unsigned long long)m->meta.received_ts_ms);
//     ESP_LOGI(TAG, "target_dev=\"%s\"  io_kind=%s  io_id=%d  action=%s",
//              m->target_dev, parser_iokind_str(m->io_kind), m->io_id, parser_action_str(m->action));
//     ESP_LOGI(TAG, "topic_hint=\"%s\"  corr_id=%s%s",
//              m->topic_hint, m->corr_id, m->meta.corr_generated ? " (generated)" : "");
//     if (m->params.has_duration_ms)   ESP_LOGI(TAG, "duration_ms=%d", m->params.duration_ms);
//     if (m->params.has_brightness_pct)ESP_LOGI(TAG, "brightness_pct=%d", m->params.brightness_pct);
//     if (m->params.has_ramp_ms)       ESP_LOGI(TAG, "ramp_ms=%d", m->params.ramp_ms);
//     ESP_LOGI(TAG, "-----------------------");
// }

// static void run_case(const char *title, const char *json, const char *topic) {
//     parser_meta_t meta = {
//         .source = topic ? PARSER_SRC_MQTT : PARSER_SRC_LOCAL,
//         .topic_hint = topic,
//         .received_ts_ms = 123456789
//     };
//     ESP_LOGI(TAG, "CASE: %s", title);
//     ESP_LOGI(TAG, "JSON: %s", json);

//     parser_result_t r = parser_parse(json, &meta);
//     if (r.unknown_keys[0]) {
//         ESP_LOGW(TAG, "Unknown fields: %s", r.unknown_keys);
//     }
//     if (r.ok) {
//         dump_msg(&r.msg);
//     } else {
//         ESP_LOGI(TAG, "ERROR: code=%s  path=\"%s\"  detail=\"%s\"",
//                  parser_err_str(r.error.code), r.error.path, r.error.detail);
//     }
//     ESP_LOGI(TAG, " ");
// }

// void app_main(void) {
//     parser_init();

//     run_case("Relay ON + minutes",
//         "{ \"target\":\"ESP32_C\", \"command\":\"ON\", \"relayy\":900, \"minutes\":0 }",
//         "Garden/LO/Switch/Set");

//     run_case("PWM SET via duty + ramp",
//         "{ \"device\":\"ESP32_A\", \"io\":\"PWM\", \"gpio\":4, \"action\":\"SET\", \"duty\":25, \"ramp\":\"500\" }",
//         "Lights/Hall/Set");

//     run_case("Input READ",
//         "{ \"dev\":\"ESP32_B\", \"type\":\"INPUT\", \"pin\":68, \"cmd\":\"READ\", \"debounce_ms\":\"200\" }",
//         "Inputs/Door/Read");

//     run_case("PWM ON 25%%",
//         "{ \"target_dev\":\"ESP32_A\", \"io_kind\":\"PWM\", \"io_id\":0, \"action\":\"ON\", \"brightness\":\"25\" }",
//         "Lights/Box/Set");

//     run_case("Error: missing io_id",
//         "{ \"target\":\"ESP32_C\", \"command\":\"OFF\", \"io\":\"RELAY\",\"io_id\": \"10\"}",
//         "Garden/LO/Switch/Set");
// }


// //--------------------------TEST met Config in NVS

// main.c
// #include <inttypes.h>               // voor PRIu32 / PRIX32
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "esp_log.h"
// #include "esp_err.h"
// #include "config_store.h"
// #include "relay_ctrl.h"
// #include "relay_ctrl.h"
// #include "pwm_ctrl.h"
// #include "input_ctrl.h"

// static const char *TAG = "app";

// void app_main(void)
// {
//     // init NVS + config en cache laden
//     esp_err_t err = config_init();
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "config_init failed: %s", esp_err_to_name(err));
//         return;
//     }

//     const cfg_t *cfg = config_get_cached();

//     // Device
//     ESP_LOGI(TAG, "Device name: %s", cfg->dev_name);

//     // Relays
//     ESP_LOGI(TAG, "Relays: count=%d", cfg->relay_count);
//     for (int i = 0; i < cfg->relay_count; i++) {
//         int gpio        = cfg->relay_gpio[i];
//         int active_low  = (cfg->relay_active_low_mask  >> i) & 1;
//         int open_drain  = (cfg->relay_open_drain_mask  >> i) & 1;
//         uint32_t autoff = cfg->relay_autoff_sec[i];

//         ESP_LOGI(TAG, "  Relay[%d]  GPIO=%d  active_low=%d  open_drain=%d  auto_off=%" PRIu32 " sec",
//                  i, gpio, active_low, open_drain, autoff);
//     }
//     ESP_LOGI(TAG, "Relay masks: active_low=0x%" PRIX32 ", open_drain=0x%" PRIX32,
//              cfg->relay_active_low_mask, cfg->relay_open_drain_mask);

//     // PWM
//     ESP_LOGI(TAG, "PWM: count=%d, freq=%" PRIu32 " Hz", cfg->pwm_count, cfg->pwm_freq_hz);
//     for (int i = 0; i < cfg->pwm_count; i++) {
//         int gpio      = cfg->pwm_gpio[i];
//         int inverted  = (cfg->pwm_inverted_mask >> i) & 1;

//         ESP_LOGI(TAG, "  PWM[%d]    GPIO=%d  inverted=%d", i, gpio, inverted);
//     }
//     ESP_LOGI(TAG, "PWM inverted mask: 0x%" PRIX32, cfg->pwm_inverted_mask);

//     // Inputs
//     ESP_LOGI(TAG, "Inputs: count=%d", cfg->input_count);
//     for (int i = 0; i < cfg->input_count; i++) {
//         int gpio       = cfg->input_gpio[i];
//         int pullup     = (cfg->input_pullup_mask    >> i) & 1;
//         int pulldown   = (cfg->input_pulldown_mask  >> i) & 1;
//         int inverted   = (cfg->input_inverted_mask  >> i) & 1;
//         uint32_t db_ms = cfg->input_debounce_ms[i];

//         ESP_LOGI(TAG, "  Input[%d]  GPIO=%d  pullup=%d  pulldown=%d  inverted=%d  debounce=%" PRIu32 " ms",
//                  i, gpio, pullup, pulldown, inverted, db_ms);
//     }
//     ESP_LOGI(TAG, "Input masks: pullup=0x%" PRIX32 ", pulldown=0x%" PRIX32 ", inverted=0x%" PRIX32,
//              cfg->input_pullup_mask, cfg->input_pulldown_mask, cfg->input_inverted_mask);

//     // laat de taak leven
//     while (1) {
//         vTaskDelay(pdMS_TO_TICKS(1000));
//     }
// }


// void app_main(void)
// {
//     ESP_LOGI(TAG, "=== APP START ===");

//     //
//     // 1) CONFIG LADEN / INIT
//     //
    
//     ESP_ERROR_CHECK(config_init()); // opent NVS + laadt of zet defaults
//     // ESP_ERROR_CHECK(config_erase_all());
//     cfg_t tmp;
//     // config_reset_defaults(&tmp);
//     // config_save(&tmp);
//     // const cfg_t *cfg = config_get_cached(); // pointer naar huidige config in RAM

//     //
//     // 2) DRIVERS STARTEN MET CONFIG
//     //

//     // --- Relay driver
//     relay_ctrl_init(cfg->relay_gpio,
//                     cfg->relay_count,
//                     cfg->relay_active_low_mask,
//                     cfg->relay_open_drain_mask);

//     for (int i = 0; i < cfg->relay_count; i++) {
//         relay_ctrl_set_autoff_seconds(i, cfg->relay_autoff_sec[i]);
//     }

//     // --- PWM driver
//     pwm_ctrl_init(cfg->pwm_gpio,
//                   cfg->pwm_count,
//                   cfg->pwm_inverted_mask,
//                   cfg->pwm_freq_hz);

//     // --- Input driver
//     input_ctrl_init(cfg->input_gpio,
//                     cfg->input_count,
//                     cfg->input_pullup_mask,
//                     cfg->input_pulldown_mask,
//                     cfg->input_inverted_mask,
//                     30); // default debounce

//     for (int i = 0; i < cfg->input_count; i++) {
//         input_ctrl_set_debounce_ms(i, cfg->input_debounce_ms[i]);
//     }

//     ESP_LOGI(TAG, "Drivers gestart met config uit NVS");

//     //
//     // 3) CONFIG WIJZIGEN EN OPSLAAN
//     //
//     int relays[] = { 26, 27, 14, 12 };
//     config_set_relays(relays, 4);
//     config_set_relay_masks(0x0F, 0x0F); // active_low_mask, open_drain_mask
//     config_set_relay_autoff(0, 5);      // relais 0 auto-off na 5s

//     ESP_ERROR_CHECK(config_commit()); // schrijft naar NVS

//     ESP_LOGI(TAG, "Config gewijzigd en opgeslagen");

//     //
//     // 4) DEMO LOOP – laat gewoon wat zien
//     //
//     while (1) {
//         // voorbeeld: togglen van relais 0 elke 2s
//         relay_ctrl_toggle(0);
//         ESP_LOGI(TAG, "Relay 0 state: %d", relay_ctrl_get_state(0));
//         vTaskDelay(pdMS_TO_TICKS(2000));
//     }
// }


// //--------------------------TEST met input

// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "esp_log.h"
// #include "input_ctrl.h"

// static const char *TAG = "app";

// // Hook: logt stabiele wijzigingen (na debounce)
// static void in_hook(int ch, bool level) {
//     if (ch == 0) { // PIR kanaal
//         ESP_LOGI(TAG, "PIR: %s", level ? "BEWEGING" : "stil");
//     } else {
//         ESP_LOGI(TAG, "IN%d -> %d", ch, level);
//     }
// }

// void app_main(void)
// {
//     // Kanaaltoewijzing:
//     // ch0 = PIR op GPIO 25
//     // ch1 = knop op GPIO 26 (naar GND, interne pull-up)
//     // ch2 = knop op GPIO 27 (naar GND, interne pull-up)
//     int pins[] = {25, 26, 27};

//     // Bitmasks per kanaal (bit0=ch0, bit1=ch1, ...)
//     uint32_t pullup_mask   = 0b110;   // ch1 & ch2 pull-up aan (knoppen naar GND)
//     uint32_t pulldown_mask = 0b000;   // geen pulldowns
//     uint32_t inverted_mask = 0b000;   // PIR is actief-HIGH (meeste modules), knoppen niet inverteren

//     // Init (debounce standaard 30 ms; PIR heeft dat niet nodig, maar stoort niet)
//     ESP_ERROR_CHECK(input_ctrl_init(pins, 3, pullup_mask, pulldown_mask,
//                                     inverted_mask, /*debounce_ms_def=*/30));
//     input_ctrl_set_state_hook(in_hook);

//     // Event loop (optioneel: periodiek huidige levels loggen)
//     while (1) {
//         bool pir   = input_ctrl_get_level(0);
//         bool btn1  = input_ctrl_get_level(1);
//         bool btn2  = input_ctrl_get_level(2);
//         ESP_LOGD(TAG, "live: PIR=%d BTN1=%d BTN2=%d", pir, btn1, btn2);
//         vTaskDelay(pdMS_TO_TICKS(1000));
//     }
// }



//--------------------------TEST met PWM

// #include "pwm_ctrl.h"
// #include "esp_log.h"

// static const char *TAG = "app";

// static void pwm_hook(int ch, uint32_t duty) {
//     ESP_LOGI(TAG, "PWM ch%d duty=%u", ch, (unsigned int)duty);
// }

// void app_main(void) {
//     int pins[2] = {18, 19}; // twee PWM-kanalen
//     pwm_ctrl_init(pins, 2, /*inverted_mask=*/0x00, /*freq_hz=*/5000);
//     pwm_ctrl_set_state_hook(pwm_hook);

//     // pwm_ctrl_set_duty(0, 4000); // kanaal 0 ~50%
//     // pwm_ctrl_set_duty(1, 2000); // kanaal 1 ~25%

//     pwm_ctrl_fade_to(0, 1000000, 10000); // kanaal 0 → duty=2000 in 1 seconde
//     pwm_ctrl_fade_to(1, 1000, 5000);  // kanaal 1 → duty=4000 in 0,5 sec
// }



//--------------------------TEST met RELAYS

// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "esp_log.h"
// #include "relay_ctrl.h"

// static const char *TAG = "app";
// static void hook(int ch, bool on){ ESP_LOGI(TAG, "CH%d -> %s", ch, on?"ON":"OFF"); }

// void app_main(void){
//     // voorbeeld mapping: 4 kanalen
//     int map[4] = {26, 27, 14, 12};
//     relay_ctrl_init(map, 4, /*active_low_mask=*/0x0F, /*open_drain_mask=*/0x0F);
//     relay_ctrl_set_state_hook(hook);

//     // auto-off test op kanaal 0: 5 seconden
//     relay_ctrl_set_autoff_seconds(0, 0);
//     relay_ctrl_on(0);

//     int ch = 1;
//     while (1) {
//         relay_ctrl_toggle(ch);
//         ch = (ch + 1) % 4;
//         vTaskDelay(pdMS_TO_TICKS(10000));
//     }
// }
