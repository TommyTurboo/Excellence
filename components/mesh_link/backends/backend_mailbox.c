// components/mesh_link/backends/backend_mailbox.c
// Mailbox-MQTT backend: REQUEST/RESPONSE-ACK + EVENT via broker
#include "mesh_link.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Vereist door jouw project (implementeer in mqtt_link.c of gelijkaardig) */
typedef void (*mqtt_rx_cb_t)(const char* topic, const char* payload, void* user);
extern int mqtt_link_publish(const char* topic, const char* payload, int qos, bool retain);
extern int mqtt_link_subscribe(const char* topic, mqtt_rx_cb_t cb, void *user);

/* Instelbaar: topicvorm voor ‘mailbox’ per device */
#ifndef ML_MAILBOX_FMT
#define ML_MAILBOX_FMT "Devices/%s/Mailbox"
#endif

#define MAX_PENDING   16
#define STRLCPY(dst,src) do{ (dst)[sizeof(dst)-1]='\0'; strncpy((dst),(src),sizeof(dst)-1);}while(0)

typedef struct {
    mesh_opts_t O;
    mesh_request_cb_t on_req;
    mesh_event_cb_t   on_evt;
    char local_box[128];
    SemaphoreHandle_t lock;
    struct {
        uint32_t corr_id;
        SemaphoreHandle_t sem;
        mesh_status_t st;
        int used;
    } pend[MAX_PENDING];
} ctx_t;

static ctx_t C;

/* ---------- helpers ---------- */
static void mailbox_topic(char *buf, size_t sz, const char *dev){
    snprintf(buf, sz, ML_MAILBOX_FMT, dev);
}

static int pend_alloc(uint32_t corr_id){
    xSemaphoreTake(C.lock, portMAX_DELAY);
    int idx=-1;
    for (int i=0;i<MAX_PENDING;i++) if (!C.pend[i].used){ idx=i; break; }
    if (idx>=0){
        C.pend[idx].used=1;
        C.pend[idx].corr_id=corr_id;
        C.pend[idx].st=MESH_TIMEOUT;
        C.pend[idx].sem = xSemaphoreCreateBinary();
    }
    xSemaphoreGive(C.lock);
    return idx;
}
static void pend_signal(uint32_t corr_id, mesh_status_t st){
    xSemaphoreTake(C.lock, portMAX_DELAY);
    for (int i=0;i<MAX_PENDING;i++){
        if (C.pend[i].used && C.pend[i].corr_id==corr_id){
            C.pend[i].st = st;
            xSemaphoreGive(C.pend[i].sem);
            break;
        }
    }
    xSemaphoreGive(C.lock);
}
static mesh_status_t pend_wait_and_free(int idx, uint32_t tmo_ms){
    if (idx<0) return MESH_ERR;
    mesh_status_t st = MESH_TIMEOUT;
    if (xSemaphoreTake(C.pend[idx].sem, pdMS_TO_TICKS(tmo_ms))==pdTRUE){
        st = C.pend[idx].st;
    }
    vSemaphoreDelete(C.pend[idx].sem);
    xSemaphoreTake(C.lock, portMAX_DELAY);
    C.pend[idx].used=0;
    xSemaphoreGive(C.lock);
    return st;
}

/* ---------- JSON helpers ---------- */
static char* build_json(const char *type, const mesh_envelope_t *e){
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o,"schema","v1");
    cJSON_AddStringToObject(o,"type",type);                 // "REQUEST"|"RESPONSE"|"EVENT"
    cJSON_AddNumberToObject(o,"corr_id", e?e->corr_id:0);
    cJSON_AddNumberToObject(o,"ts_ms",   e?e->ts_ms:0);
    cJSON_AddStringToObject(o,"src_dev", e?e->src_dev:C.O.local_dev);
    if (e && e->dst_dev) cJSON_AddStringToObject(o,"dst_dev", e->dst_dev);
    if (e){
        const char *kind =
            (e->kind==ML_KIND_RELAY)?"relay":
            (e->kind==ML_KIND_PWM)?"pwm":
            (e->kind==ML_KIND_CONFIG)?"config":
            (e->kind==ML_KIND_INPUT)?"input":"diag";
        cJSON_AddStringToObject(o,"kind", kind);
        cJSON_AddNumberToObject(o,"ttl",  e->ttl);
        cJSON_AddNumberToObject(o,"hop",  e->hop);
        if (e->origin_set_topic) cJSON_AddStringToObject(o,"origin_set_topic", e->origin_set_topic);
        if (e->payload) cJSON_AddItemToObject(o,"payload", cJSON_Duplicate(e->payload, 1));
    }
    char *js = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return js;
}

/* ---------- RX ---------- */
static void send_response_ack(const char *dst_dev, uint32_t corr_id){
    mesh_envelope_t e = { .corr_id=corr_id, .src_dev=C.O.local_dev, .dst_dev=dst_dev };
    char *js = build_json("RESPONSE", &e);
    char t[128]; mailbox_topic(t, sizeof t, dst_dev);
    mqtt_link_publish(t, js, 1, false);
    free(js);
}

static void on_rx(const char* topic, const char* payload, void* user){
    (void)topic; (void)user;
    cJSON *o = cJSON_Parse(payload); if (!o) return;

    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(o,"type"));
    const char *src  = cJSON_GetStringValue(cJSON_GetObjectItem(o,"src_dev"));
    const char *dst  = cJSON_GetStringValue(cJSON_GetObjectItem(o,"dst_dev"));
    uint32_t corr_id = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o,"corr_id"));
    cJSON *payload_j = cJSON_GetObjectItem(o,"payload");

    mesh_envelope_t e = {
        .schema = cJSON_GetStringValue(cJSON_GetObjectItem(o,"schema")),
        .corr_id = corr_id,
        .ts_ms = (uint64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o,"ts_ms")),
        .src_dev = src,
        .dst_dev = dst,
        .kind = ML_KIND_DIAG,
        .ttl = (int8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o,"ttl")),
        .hop = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o,"hop")),
        .origin_set_topic = cJSON_GetStringValue(cJSON_GetObjectItem(o,"origin_set_topic")),
        .payload = payload_j
    };

    if (!type){ cJSON_Delete(o); return; }

    if (strcmp(type,"RESPONSE")==0){
        pend_signal(corr_id, MESH_OK);
    } else if (strcmp(type,"REQUEST")==0){
        if (C.on_req) C.on_req(&e);
        if (src) send_response_ack(src, corr_id);  // leverings-ACK
    } else if (strcmp(type,"EVENT")==0){
        if (C.on_evt) C.on_evt(&e);
    }
    cJSON_Delete(o);
}

/* ---------- backend vtable ---------- */
static const char* name_backend(void){ return "mailbox-mqtt"; }

static void init(const mesh_opts_t *opts){
    memset(&C, 0, sizeof C);
    C.O = *opts;
    C.lock = xSemaphoreCreateMutex();

    mailbox_topic(C.local_box, sizeof C.local_box, C.O.local_dev);
    mqtt_link_subscribe(C.local_box, on_rx, NULL);
}

static void register_rx(mesh_request_cb_t on_request, mesh_event_cb_t on_event){
    C.on_req = on_request;
    C.on_evt = on_event;
}

static mesh_status_t request(const mesh_envelope_t *req, uint32_t timeout_ms){
    if (!req || !req->dst_dev || !*req->dst_dev) return MESH_NO_ROUTE;

    int p = pend_alloc(req->corr_id, timeout_ms);
    if (p<0) return MESH_ERR;

    char *js = build_json("REQUEST", req);
    char topic[128]; mailbox_topic(topic, sizeof topic, req->dst_dev);
    mqtt_link_publish(topic, js, 1, false);
    free(js);

    return pend_wait_and_free(p, timeout_ms); // wacht op RESPONSE-ACK
}

static mesh_status_t send_event(const mesh_envelope_t *evt){
    if (!evt || !evt->dst_dev || !*evt->dst_dev) return MESH_NO_ROUTE;
    char *js = build_json("EVENT", evt);
    char topic[128]; mailbox_topic(topic, sizeof topic, evt->dst_dev);
    int rc = mqtt_link_publish(topic, js, 1, false);
    free(js);
    return (rc==0)? MESH_OK : MESH_ERR;
}

static cJSON* snapshot(void){
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateString(C.O.local_dev));
    return arr;
}

/* Zelfde vtable-struct als in mesh_link.c */
typedef struct {
    const char* (*name)(void);
    void (*init)(const mesh_opts_t*);
    void (*register_rx)(mesh_request_cb_t, mesh_event_cb_t);
    void (*register_root)(mesh_root_cb_t);
    mesh_status_t (*request)(const mesh_envelope_t*, uint32_t);
    mesh_status_t (*send_event)(const mesh_envelope_t*);
    cJSON* (*snapshot)(void);
} ml_backend_t;

static void register_root(mesh_root_cb_t cb){ (void)cb; /* mailbox-backend meldt geen root-wissels */ }

const ml_backend_t* ml_backend_mailbox(void){
    static const ml_backend_t V = { name_backend, init, register_rx, register_root, request, send_event, snapshot };
    return &V;
}
