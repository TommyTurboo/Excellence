// components/mesh_link/backends/backend_espmesh.c
// ESP-WIFI-MESH backend v1: REQUEST/RESPONSE-ACK + EVENT + route diag
#include "mesh_link.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_mesh.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>

#ifndef CONFIG_MESH_CHANNEL
#define CONFIG_MESH_CHANNEL        6
#endif
#ifndef CONFIG_MESH_AP_CONNECTIONS
#define CONFIG_MESH_AP_CONNECTIONS 6
#endif
#ifndef CONFIG_MESH_ID_0
// 6-byte mesh ID (maak deze uniek voor jouw netwerk)
#define CONFIG_MESH_ID_0 0x11
#define CONFIG_MESH_ID_1 0x22
#define CONFIG_MESH_ID_2 0x33
#define CONFIG_MESH_ID_3 0x44
#define CONFIG_MESH_ID_4 0x55
#define CONFIG_MESH_ID_5 0x66
#endif
#ifndef CONFIG_MESH_ROUTER_SSID
#define CONFIG_MESH_ROUTER_SSID    "YourRouterSSID"
#endif
#ifndef CONFIG_MESH_ROUTER_PASSWD
#define CONFIG_MESH_ROUTER_PASSWD  "YourRouterPASS"
#endif

#define LOG_TAG "ml_espmesh"
#define MESH_PAYLOAD_MAX   1024
#define MAX_PEERS          16
#define MAX_PENDING        16

typedef struct { char name[32]; mesh_addr_t mac; bool valid; uint64_t last_ms; } peer_t;
typedef struct { uint32_t corr_id; SemaphoreHandle_t sem; mesh_status_t st; bool used; uint64_t deadline_ms; } pend_t;

typedef struct {
    mesh_opts_t O;
    mesh_request_cb_t on_req;
    mesh_event_cb_t   on_evt;
    mesh_addr_t       local_mac;
    mesh_addr_t root_mac;
    bool        root_mac_known;
    bool              is_root;
    SemaphoreHandle_t lock;
    TaskHandle_t      rx_task;

    peer_t peers[MAX_PEERS];
    pend_t pend [MAX_PENDING];
} ctx_t;

static ctx_t C;

// ---- mesh config ----
static void apply_mesh_cfg(void){
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();

    // Kanaal + router credentials
    cfg.channel = CONFIG_MESH_CHANNEL;
    cfg.router.ssid_len = strlen(CONFIG_MESH_ROUTER_SSID);
    memcpy((uint8_t *)cfg.router.ssid,     CONFIG_MESH_ROUTER_SSID, cfg.router.ssid_len);
    memcpy((uint8_t *)cfg.router.password, CONFIG_MESH_ROUTER_PASSWD, strlen(CONFIG_MESH_ROUTER_PASSWD));

    // Mesh ID (6 bytes)
    uint8_t mesh_id[6] = {
        CONFIG_MESH_ID_0, CONFIG_MESH_ID_1, CONFIG_MESH_ID_2,
        CONFIG_MESH_ID_3, CONFIG_MESH_ID_4, CONFIG_MESH_ID_5
    };
    memcpy(cfg.mesh_id.addr, mesh_id, 6);

    // AP-instellingen voor downstream children
    cfg.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
    esp_mesh_set_ap_authmode(WIFI_AUTH_WPA2_PSK);

    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
}



// ---- utils ----
static uint64_t now_ms(void){ return esp_timer_get_time()/1000ULL; }
static const char* name_backend(void){ return "esp-wifi-mesh"; }
// static bool mac_equal(const mesh_addr_t *a, const mesh_addr_t *b){ return memcmp(a->addr, b->addr, 6)==0; }

// ---- peer cache ----
static int peer_find_by_name(const char *name){
    for (int i=0;i<MAX_PEERS;i++) if (C.peers[i].valid && strcmp(C.peers[i].name, name)==0) return i;
    return -1;
}
static int peer_find_slot(void){
    for (int i=0;i<MAX_PEERS;i++) if (!C.peers[i].valid) return i;
    int oldest=0; for(int i=1;i<MAX_PEERS;i++) if (C.peers[i].last_ms < C.peers[oldest].last_ms) oldest=i;
    return oldest;
}
static void peer_upsert(const char *name, const mesh_addr_t *mac){
    if (!name||!*name||!mac) return;
    xSemaphoreTake(C.lock, portMAX_DELAY);
    int idx = peer_find_by_name(name);
    if (idx<0) idx = peer_find_slot();
    strncpy(C.peers[idx].name, name, sizeof(C.peers[idx].name)-1);
    C.peers[idx].mac = *mac;
    C.peers[idx].valid = true;
    C.peers[idx].last_ms = now_ms();
    xSemaphoreGive(C.lock);
}
static bool peer_resolve(const char *name, mesh_addr_t *out){
    if (!name||!*name) return false;
    bool ok=false;
    xSemaphoreTake(C.lock, portMAX_DELAY);
    int idx = peer_find_by_name(name);
    if (idx>=0 && C.peers[idx].valid){ *out = C.peers[idx].mac; ok=true; C.peers[idx].last_ms=now_ms(); }
    xSemaphoreGive(C.lock);
    return ok;
}

// ---- pending map (REQUEST → RESPONSE-ACK) ----
static int pend_alloc(uint32_t corr_id, uint32_t timeout_ms){
    xSemaphoreTake(C.lock, portMAX_DELAY);
    int idx=-1;
    for (int i=0;i<MAX_PENDING;i++) if (!C.pend[i].used){ idx=i; break; }
    if (idx>=0){
        C.pend[idx].corr_id=corr_id;
        C.pend[idx].sem = xSemaphoreCreateBinary();
        C.pend[idx].st = MESH_TIMEOUT;
        C.pend[idx].used=true;
        C.pend[idx].deadline_ms = now_ms()+timeout_ms;
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
static mesh_status_t pend_wait_and_free(int idx, uint32_t timeout_ms){
    if (idx<0) return MESH_ERR;
    mesh_status_t st = MESH_TIMEOUT;
    if (xSemaphoreTake(C.pend[idx].sem, pdMS_TO_TICKS(timeout_ms))==pdTRUE){
        st = C.pend[idx].st;
    }
    vSemaphoreDelete(C.pend[idx].sem);
    xSemaphoreTake(C.lock, portMAX_DELAY);
    C.pend[idx].used=false;
    xSemaphoreGive(C.lock);
    return st;
}

// ---- JSON helpers ----
static char* build_json(const char *type, const mesh_envelope_t *e){
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o,"schema","v1");
    cJSON_AddStringToObject(o,"type",type); // "REQUEST"|"RESPONSE"|"EVENT"|"HELLO"
    cJSON_AddNumberToObject(o,"corr_id", e?e->corr_id:0);
    cJSON_AddNumberToObject(o,"ts_ms",   e?e->ts_ms:now_ms());
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

static esp_err_t mesh_send_json(const mesh_addr_t *to, const char *js){
    mesh_data_t md = {0};
    md.data = (uint8_t*)js;
    md.size = strlen(js)+1;           // inclusief '\0'
    md.proto = MESH_PROTO_BIN;
    md.tos   = MESH_TOS_P2P;
    int flag = 0; // unicast
    return esp_mesh_send((mesh_addr_t*)to, &md, flag, NULL, 0);
}

// ---- RX handling ----
static void handle_packet(const mesh_addr_t *from, const char *json, size_t len){
    (void)len;
    cJSON *o = cJSON_Parse(json); if (!o) return;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(o,"type"));
    const char *src  = cJSON_GetStringValue(cJSON_GetObjectItem(o,"src_dev"));
    const char *dst  = cJSON_GetStringValue(cJSON_GetObjectItem(o,"dst_dev"));
    uint32_t corr_id = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o,"corr_id"));
    cJSON *payload   = cJSON_GetObjectItem(o,"payload");

    if (src) peer_upsert(src, from); // leer peer MAC

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
        .payload = payload
    };

    if (type && strcmp(type,"HELLO")==0){
        // nothing extra
    } else if (type && strcmp(type,"RESPONSE")==0){
        pend_signal(corr_id, MESH_OK);
    } else if (type && strcmp(type,"REQUEST")==0){
        if (C.on_req) C.on_req(&e);
        // stuur ontvangst-ACK terug (geen functioneel resultaat)
        mesh_addr_t back;
        if (peer_resolve(src, &back)){
            char *ack = build_json("RESPONSE", &e);
            mesh_send_json(&back, ack);
            free(ack);
        }
    } else if (type && strcmp(type,"EVENT")==0){
        if (C.on_evt) C.on_evt(&e);
    }
    cJSON_Delete(o);
}

static void rx_loop(void *arg){
    (void)arg;
    uint8_t *buf = malloc(MESH_PAYLOAD_MAX);
    if (!buf) { vTaskDelete(NULL); return; }

    for(;;){
        mesh_addr_t from = (mesh_addr_t){0};
        mesh_data_t data = (mesh_data_t){ .data = buf, .size = MESH_PAYLOAD_MAX, .proto = 0, .tos = 0 };
        int flag = 0;

        // let op: jouw IDF-signatuur → (&from, &data, timeout_ms, &flag, opt[], opt_count)
        esp_err_t err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        if (err == ESP_OK && data.data && data.size > 0){
            size_t n = data.size;
            if (n >= MESH_PAYLOAD_MAX) n = MESH_PAYLOAD_MAX - 1;  // safety cap
            ((char*)data.data)[n] = '\0';                         // zorg voor nul-terminatie
            handle_packet(&from, (const char*)data.data, n);      // decode & dispatch
        }
    }
}

// ---- diag: routing snapshot & events ----
static void publish_route_event(const char *ev_name){
    int tbl_size_bytes = esp_mesh_get_routing_table_size(); // bytes (return value)
    int entries = 0;
    mesh_addr_t *tbl = NULL;

    if (tbl_size_bytes > 0) {
        tbl = (mesh_addr_t*)calloc(1, tbl_size_bytes);
        esp_mesh_get_routing_table(tbl, tbl_size_bytes, &entries);
    }
    cJSON *snap = cJSON_CreateArray();
    for (int i = 0; i < entries; i++) {
        char mac[18];
        snprintf(mac,sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x",
                 tbl[i].addr[0],tbl[i].addr[1],tbl[i].addr[2],
                 tbl[i].addr[3],tbl[i].addr[4],tbl[i].addr[5]);
        cJSON_AddItemToArray(snap, cJSON_CreateString(mac));
    }
    free(tbl);
    mesh_diag_publish_route_table(ev_name, snap);
    cJSON_Delete(snap);
}

static void espmesh_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data){
    (void)arg; (void)base; (void)data;
    switch(id){
        case MESH_EVENT_ROUTING_TABLE_ADD:    publish_route_event("ADD"); break;
        case MESH_EVENT_ROUTING_TABLE_REMOVE: publish_route_event("REMOVE"); break;
        case MESH_EVENT_ROOT_ADDRESS: {
            const mesh_event_root_address_t *ev = (const mesh_event_root_address_t*)data;
            memcpy(C.root_mac.addr, ev->addr, 6);
            C.root_mac_known = true;
            if (!C.is_root) {
                mesh_envelope_t e = { .corr_id = 0, .ts_ms = now_ms(), .src_dev = C.O.local_dev };
                char *js = build_json("HELLO", &e);
                mesh_send_json(&C.root_mac, js);
                free(js);
            }
            break;
        }
        default: break;
    }
}

// ---- init / start ----
static void start_rx_task_once(void){
    if (!C.rx_task){
        xTaskCreate(rx_loop, "mesh_rx", 4096, NULL, 5, &C.rx_task);
    }
}

static void init_mesh_stack(const mesh_opts_t *opts){
    // Verwacht dat Wi-Fi al is geïnitialiseerd/gestart door applicatie
    ESP_ERROR_CHECK(esp_mesh_init());
    esp_mesh_set_max_layer(6);
    esp_mesh_set_self_organized(true, false);
    if (opts->role==MESH_ROLE_ROOT) esp_mesh_set_type(MESH_ROOT);
    else                           esp_mesh_set_type(MESH_NODE);

    apply_mesh_cfg();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(MESH_EVENT, ESP_EVENT_ANY_ID, espmesh_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_mesh_start());

    esp_wifi_get_mac(WIFI_IF_STA, C.local_mac.addr);
    ESP_LOGI(LOG_TAG, "Mesh gestart; role=%s", (opts->role==MESH_ROLE_ROOT)?"ROOT":"CHILD");
}

static void init(const mesh_opts_t *opts){
    memset(&C, 0, sizeof C);
    C.O = *opts;
    C.lock = xSemaphoreCreateMutex();
    C.is_root = (opts->role==MESH_ROLE_ROOT);

    init_mesh_stack(opts);
    start_rx_task_once();
    C.root_mac_known = false;

    // Child meldt zich bij root via HELLO (doen we zodra root-MAC bekend is via event)

}

static void register_rx(mesh_request_cb_t on_request, mesh_event_cb_t on_event){
    C.on_req = on_request; C.on_evt = on_event;
}

// ---- send paths ----
static bool resolve_dst(const char *dst_dev, mesh_addr_t *out){
    if (!dst_dev || !*dst_dev || strcmp(dst_dev,"*ROOT*")==0){
        if (C.root_mac_known) { *out = C.root_mac; return true; }
        return false; // root nog onbekend → NO_ROUTE
    }
    return peer_resolve(dst_dev, out);
}

static mesh_status_t request(const mesh_envelope_t *req, uint32_t timeout_ms){
    mesh_addr_t dst;
    if (!resolve_dst(req->dst_dev, &dst)) return MESH_NO_ROUTE;

    int p = pend_alloc(req->corr_id, timeout_ms);
    if (p<0) return MESH_ERR;

    char *js = build_json("REQUEST", req);
    esp_err_t er = mesh_send_json(&dst, js);
    free(js);
    if (er!=ESP_OK) { (void)pend_wait_and_free(p, 0); return MESH_NO_ROUTE; }

    mesh_status_t st = pend_wait_and_free(p, timeout_ms); // wacht op RESPONSE-ACK
    return st;
}

static mesh_status_t send_event(const mesh_envelope_t *evt){
    mesh_addr_t dst;
    if (!resolve_dst(evt->dst_dev, &dst)) return MESH_NO_ROUTE;
    char *js = build_json("EVENT", evt);
    esp_err_t er = mesh_send_json(&dst, js);
    free(js);
    return (er==ESP_OK)? MESH_OK : MESH_NO_ROUTE;
}

// ---- snapshot ----
static cJSON* snapshot(void){
    int tbl_size_bytes = esp_mesh_get_routing_table_size(); // bytes (return value)
    int entries = 0;
    mesh_addr_t *tbl = (tbl_size_bytes>0) ? (mesh_addr_t*)calloc(1, tbl_size_bytes) : NULL;
    if (tbl) esp_mesh_get_routing_table(tbl, tbl_size_bytes, &entries);
    cJSON *arr = cJSON_CreateArray();
    for (int i=0;i<entries;i++){
        char mac[18];
        snprintf(mac,sizeof mac,"%02x:%02x:%02x:%02x:%02x:%02x",
                 tbl[i].addr[0],tbl[i].addr[1],tbl[i].addr[2],
                 tbl[i].addr[3],tbl[i].addr[4],tbl[i].addr[5]);
        cJSON_AddItemToArray(arr, cJSON_CreateString(mac));
    }
    free(tbl);
    return arr;
}

// ---- vtable export (matcht mesh_link.c) ----
typedef struct {
    const char* (*name)(void);
    void (*init)(const mesh_opts_t*);
    void (*register_rx)(mesh_request_cb_t, mesh_event_cb_t);
    mesh_status_t (*request)(const mesh_envelope_t*, uint32_t);
    mesh_status_t (*send_event)(const mesh_envelope_t*);
    cJSON* (*snapshot)(void);
} ml_backend_t;

const ml_backend_t* ml_backend_espmesh(void){
    static const ml_backend_t V = { name_backend, init, register_rx, request, send_event, snapshot };
    return &V;
}
