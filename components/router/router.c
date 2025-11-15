#include "router.h"
#include <string.h>
#include "mesh_link.h"
#include "mqtt_link.h" 
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "parser.h"
#include <strings.h>   // strcasecmp
#include "esp_log.h"

static router_cbs_t CB;
static char g_local_dev[32] = "ESP32_ROOT"; // pas evt. aan jouw lengte aan

// helper: 32-bit corr_id uit string (FNV-1a)
static uint32_t corr_id_u32(const char *s){
    uint32_t h = 2166136261u;
    if (!s) return h;
    for (; *s; s++){ h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

// --- HELLO detectie ---
static inline bool json_is_hello(const cJSON *p){
    if (!p) return false;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(p, "type");
    if (cJSON_IsString(t) && t->valuestring &&
        strcasecmp(t->valuestring, "HELLO")==0) return true;
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(p, "hello");
    return (cJSON_IsBool(h) && cJSON_IsTrue(h));
}

static void publish_status_for(const char *dev, bool online){
    char topic[128], payload[160];
    snprintf(topic, sizeof(topic), "Devices/%s/Status", dev);
    if (online) snprintf(payload, sizeof(payload), "{\"status\":\"online\",\"dev\":\"%s\"}", dev);
    else        snprintf(payload, sizeof(payload), "{\"status\":\"offline\"}");
    mqtt_link_publish(topic, payload, 1, true);  // retain
}

// helper: mapping parser → mesh_kind_t
static mesh_kind_t kind_from_msg(const parser_msg_t *m){
    switch (m->io_kind){
        case IO_RELAY: return ML_KIND_RELAY;
        case IO_PWM:   return ML_KIND_PWM;
        case IO_INPUT: return ML_KIND_INPUT;
        default:       return ML_KIND_DIAG;
    }
}

// helper: minimal payload voor child
static cJSON* mesh_payload_from_msg(const parser_msg_t *m){
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "io",     parser_iokind_str(m->io_kind));
    cJSON_AddNumberToObject(o, "io_id",  m->io_id);
    cJSON_AddStringToObject(o, "action", parser_action_str(m->action));
    // TODO (PWM): als je later params nodig hebt, hier toevoegen als "params": {...}
    return o;
}

static const char* stat_str(router_status_t s){
    switch(s){
        case ROUTER_OK:             return "OK";
        case ROUTER_ERR_INVALID:    return "INVALID";
        case ROUTER_ERR_OUT_OF_RANGE:return "OUT_OF_RANGE";
        case ROUTER_ERR_NO_ROUTE:   return "NO_ROUTE";
        case ROUTER_ERR_TIMEOUT:    return "TIMEOUT";
        default:                    return "ERROR";
    }
}

void router_init(const router_cbs_t *cbs){
    if (cbs) CB = *cbs;
}

void router_set_local_dev(const char *dev_name){
    if (!dev_name) return;
    snprintf(g_local_dev, sizeof(g_local_dev), "%s", dev_name);
}

static void publish_state(const parser_msg_t *m, router_status_t st,
                          const char *detail, int extra_val, bool has_val,
                          int extra_pct, bool has_pct)
{
    if (!CB.mqtt_pub) return;

    char topic[96];
    snprintf(topic, sizeof(topic), "Devices/%s/State", m->target_dev);

    char body[256];
    int n = snprintf(body, sizeof(body),
        "{ \"corr_id\":\"%s\",\"dev\":\"%s\",\"status\":\"%s\","
        "\"io\":\"%s\",\"io_id\":%d,\"action\":\"%s\"",
        m->corr_id, m->target_dev, stat_str(st),
        parser_iokind_str(m->io_kind), m->io_id, parser_action_str(m->action));

    if (has_val) n += snprintf(body+n, sizeof(body)-n, ",\"value\":%d", extra_val);
    if (has_pct) n += snprintf(body+n, sizeof(body)-n, ",\"brightness_pct\":%d", extra_pct);
    if (detail && *detail) n += snprintf(body+n, sizeof(body)-n, ",\"detail\":\"%s\"", detail);

    snprintf(body+n, sizeof(body)-n, " }");
    CB.mqtt_pub(topic, body, /*qos=*/1, /*retain=*/false);
}

router_status_t router_handle(const parser_msg_t *m){
    if (!m) return ROUTER_ERR_INTERNAL;

    // --- Remote? → via mesh versturen en NIET lokaal publishen ---
    if (strcmp(m->target_dev, g_local_dev) != 0) {
        cJSON *payload = mesh_payload_from_msg(m);
        mesh_kind_t kind = kind_from_msg(m);
        uint32_t cid = corr_id_u32(m->corr_id);      // maak 32-bit corr_id
        const char *origin = m->topic_hint;    // als je die al bijhoudt; anders NULL

        router_send_cmd_to_target(m->target_dev, origin, kind, payload, cid);
        cJSON_Delete(payload);
        return ROUTER_OK;  // accepted; uiteindelijke State volgt via EVENT
    }

    // --- Lokaal pad ---
    router_status_t st = ROUTER_ERR_INVALID;
    int value = 0, pct = 0; bool has_val=false, has_pct=false;

    switch (m->io_kind) {
        case IO_RELAY:
            if (CB.exec_relay) st = CB.exec_relay(m);
            else st = ROUTER_ERR_INTERNAL;
            break;

        case IO_PWM:
            if (CB.exec_pwm) { st = CB.exec_pwm(m, &pct); has_pct = (st==ROUTER_OK); }
            else st = ROUTER_ERR_INTERNAL;
            break;

        case IO_INPUT:
            if (m->action == ACT_READ) {
                if (CB.exec_input) { st = CB.exec_input(m, &value); has_val = (st==ROUTER_OK); }
                else st = ROUTER_ERR_INTERNAL;
            } else {
                st = ROUTER_ERR_INVALID;
            }
            break;

        default:
            st = ROUTER_ERR_INVALID;
            break;
    }

    publish_state(m, st, (st==ROUTER_OK? NULL : "exec failed"), value, has_val, pct, has_pct);
    return st;
}

// --- hieronder tbv Wifi Mesh

static int io_from_str(const char *s){
    if (!s) return IO_RELAY;
    if (!strcasecmp(s,"relay")) return IO_RELAY;
    if (!strcasecmp(s,"pwm"))   return IO_PWM;
    if (!strcasecmp(s,"input")) return IO_INPUT;
    return IO_RELAY;
}
static int action_from_str(const char *s){
    if (!s) return ACT_SET;
    if (!strcasecmp(s,"ON"))      return ACT_ON;
    if (!strcasecmp(s,"OFF"))     return ACT_OFF;
    if (!strcasecmp(s,"TOGGLE"))  return ACT_TOGGLE;
    if (!strcasecmp(s,"READ"))    return ACT_READ;
    if (!strcasecmp(s,"SET"))     return ACT_SET;
    return ACT_SET;
}

void router_execute_local(mesh_kind_t kind, const cJSON *payload,
                          uint32_t corr_id, const char *origin_set_topic)
{
    (void)kind; (void)origin_set_topic;

    const cJSON *jio   = cJSON_GetObjectItemCaseSensitive(payload, "io");
    const cJSON *jioid = cJSON_GetObjectItemCaseSensitive(payload, "io_id");
    const cJSON *jact  = cJSON_GetObjectItemCaseSensitive(payload, "action");
    // const cJSON *jpar  = cJSON_GetObjectItemCaseSensitive(payload, "params"); // TODO: later voor PWM

    parser_msg_t m = (parser_msg_t){0};

    // arrays invullen met snprintf (géén pointer-assign!)
    snprintf(m.target_dev, sizeof m.target_dev, "%s", g_local_dev);
    snprintf(m.corr_id,   sizeof m.corr_id,   "%08X", (unsigned)corr_id);

    m.io_kind = io_from_str(cJSON_IsString(jio)   ? jio->valuestring   : NULL);
    m.io_id   =            cJSON_IsNumber(jioid) ? jioid->valueint     : 0;
    m.action  = action_from_str(cJSON_IsString(jact)  ? jact->valuestring  : NULL);

    // m.params voorlopig niet vullen (jouw type ≠ cJSON)

    switch (m.io_kind){
        case IO_RELAY:
            if (CB.exec_relay) (void)CB.exec_relay(&m);
            break;
        case IO_PWM: {
            int pct = 0;
            if (CB.exec_pwm) (void)CB.exec_pwm(&m, &pct);
            break;
        }
        case IO_INPUT: {
            if (m.action == ACT_READ && CB.exec_input){
                int val=0; (void)CB.exec_input(&m, &val);
            }
            break;
        }
        default: break;
    }
    // GEEN MQTT publish hier; drivers sturen later EVENT via router_emit_event(...)
}



// ---------- derive_state_topic ----------
void derive_state_topic(const char *set_topic, char *out, size_t out_sz){
    if (set_topic){
        const char *p = strstr(set_topic, "/Cmd/Set");
        if (p){
            size_t n = (size_t)(p - set_topic);
            snprintf(out, out_sz, "%.*s/State", (int)n, set_topic);
            return;
        }
        snprintf(out, out_sz, "%s", set_topic);
        return;
    }
    snprintf(out, out_sz, "Devices/%s/State", g_local_dev);
}

// ---------- router_emit_event ----------
// drivers (child) → EVENT naar root sturen na fysieke actie
void router_emit_event(mesh_kind_t kind, uint32_t corr_id,
                       const char *origin_set_topic, const cJSON *state_payload)
{
    mesh_envelope_t ev = {
        .schema="v1",
        .corr_id=corr_id,
        .ts_ms=0,
        .src_dev=g_local_dev,
        .dst_dev="*ROOT*",
        .kind=kind,
        .ttl=3, .hop=0,
        .origin_set_topic=origin_set_topic,
        .payload=(cJSON*)state_payload
    };
    (void)mesh_send_event(&ev);
}

// 1) Root: stuur naar child via mesh (non-local target)
void router_send_cmd_to_target(const char *target_dev,
                               const char *origin_set_topic,
                               mesh_kind_t kind,
                               const cJSON *payload,
                               uint32_t corr_id)
{
    const char *local = g_local_dev;

    if (strcmp(target_dev, local)==0){
        router_execute_local(kind, payload, corr_id, origin_set_topic);
        return;
    }

    mesh_envelope_t env = {
        .schema="v1",
        .corr_id=corr_id,
        .ts_ms=0,                    // backend vult evt. in
        .src_dev=local,
        .dst_dev=target_dev,
        .kind=kind,
        .ttl=3, .hop=0,
        .origin_set_topic=origin_set_topic,
        .payload=(cJSON*)payload
    };
    mesh_status_t st = mesh_request(&env, 1000);
    if (st!=MESH_OK){
        // publiceer jouw ERROR zoals je al deed
        // router_publish_error(corr_id, st==MESH_NO_ROUTE?"NO_ROUTE":"TIMEOUT");
    }
    // Succes: uiteindelijke State komt als EVENT via router_handle_mesh_event()
}

// 2a) Child: ontvangen REQUEST → voer lokaal uit (géén MQTT publish hier)
void router_handle_mesh_request(const mesh_envelope_t *req){
    router_execute_local(req->kind, req->payload, req->corr_id, req->origin_set_topic);
}

// 2b) Root: ontvangen EVENT → publiceer naar juiste State-topic
void router_handle_mesh_event(const mesh_envelope_t *evt){

    // 1) HELLO van child? -> Status=online (retain) + Info (retain)
    bool is_hello = (evt->kind == ML_KIND_DIAG) && json_is_hello(evt->payload);

    ESP_LOGI("router", "EVENT kind=%d from=%s", (int)evt->kind,
             evt->src_dev ? evt->src_dev : "(null)");

    if (is_hello){
        ESP_LOGI("router", "HELLO from %s -> publish Status/Info", evt->src_dev);
        publish_status_for(evt->src_dev, true);               // online (retain)
        char tinfo[160];
        snprintf(tinfo, sizeof(tinfo), "Devices/%s/Info", evt->src_dev);
        char *js = cJSON_PrintUnformatted(evt->payload);
        mqtt_link_publish(tinfo, js, 1, true);                // retain
        free(js);
        return;
    }

    // 2) default: publiceer naar State (niet-retained)
    char topic[160];
    if (evt->origin_set_topic && *evt->origin_set_topic)
        derive_state_topic(evt->origin_set_topic, topic, sizeof topic);
    else
        snprintf(topic, sizeof topic, "Devices/%s/State", evt->src_dev);

    char *js = cJSON_PrintUnformatted(evt->payload);
    mqtt_link_publish(topic, js, /*qos*/1, /*retain*/false);
    free(js);
}