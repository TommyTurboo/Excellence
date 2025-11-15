// components/mesh_link/backends/backend_espmesh.c
// ESP-WIFI-MESH backend: request/response + event + route diag with MQTT publishing

#include "mesh_link.h"
#include "mqtt_link.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_mesh.h"
#include "esp_timer.h"
#include <esp_rom_crc.h>

#include <cJSON.h>

#ifndef CONFIG_MESH_CHANNEL
#define CONFIG_MESH_CHANNEL        1
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

#define LOG_TAG "backend_espmesh"
#define MESH_PAYLOAD_MAX   1024
#define MAX_PEERS          16
#define MAX_PENDING        16
#define MAX_RT_SNAPSHOT    128

static QueueHandle_t s_workq = NULL;

typedef struct { char name[32]; mesh_addr_t mac; bool valid; uint64_t last_ms; } peer_t;
typedef struct { uint32_t corr_id; SemaphoreHandle_t sem; mesh_status_t st; bool used; } pend_t;

typedef struct {
    mesh_opts_t O;
    mesh_request_cb_t on_req;
    mesh_event_cb_t   on_evt;
    mesh_root_cb_t    on_root;
    mesh_addr_t       local_mac;
    mesh_addr_t       root_mac;
    bool              root_mac_known;
    bool              is_root;
    SemaphoreHandle_t lock;
    TaskHandle_t      rx_task;

    // Mesh ID (binary + printable)
    uint8_t mesh_id[6];
    char    mesh_id_hex[13];

    // Root epoch + topology hash
    uint32_t root_epoch;
    uint32_t last_topo_crc;

    peer_t peers[MAX_PEERS];
    pend_t pend [MAX_PENDING];

    // previous RT snapshot for diffs
    mesh_addr_t rt_prev[MAX_RT_SNAPSHOT];
    int         rt_prev_n;

    // Heartbeat
    TimerHandle_t hb_timer;
    int           hb_interval_ms;
} ctx_t;

static ctx_t C;

typedef enum { W_RT_ADD, W_RT_REMOVE, W_CHILD_ADD, W_CHILD_REMOVE, W_ROOT_CHANGE, W_HEARTBEAT } work_t;
typedef struct { uint8_t type; bool now_root; } work_msg_t;

// forward
static void publish_route_event(const char *ev_name);
static void subscribe_root_current_stream(void);
static void on_mqtt_root_current(const char *topic, const char *payload);

// utils
static uint64_t now_ms(void){ return esp_timer_get_time()/1000ULL; }
static const char* name_backend(void){ return "esp-wifi-mesh"; }

static void mac_str(const uint8_t m[6], char *out, size_t n){
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x", m[0],m[1],m[2],m[3],m[4],m[5]);
}

static void mesh_id_to_hex(char out[13]){
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x",
             C.mesh_id[0],C.mesh_id[1],C.mesh_id[2],C.mesh_id[3],C.mesh_id[4],C.mesh_id[5]);
}

static int cmp_mac(const void *aa, const void *bb){
    const mesh_addr_t *a = (const mesh_addr_t*)aa;
    const mesh_addr_t *b = (const mesh_addr_t*)bb;
    return memcmp(a->addr, b->addr, 6);
}

static int rt_snapshot(mesh_addr_t *buf, int max){
    int cap = esp_mesh_get_routing_table_size();
    int got = 0;
    mesh_addr_t *tmp = (cap>0) ? (mesh_addr_t*)calloc(cap, sizeof(mesh_addr_t)) : NULL;
    if (tmp) esp_mesh_get_routing_table(tmp, cap*sizeof(mesh_addr_t), &got);
    int n = (got < max) ? got : max;
    if (n>0 && tmp) memcpy(buf, tmp, n*sizeof(mesh_addr_t));
    free(tmp);
    return n;
}

// peer cache
static bool mac_equal(const mesh_addr_t *a, const mesh_addr_t *b){ return memcmp(a->addr, b->addr, 6)==0; }
static int  peer_find_by_name_unsafe(const char *name){ for(int i=0;i<MAX_PEERS;i++) if(C.peers[i].valid && strcmp(C.peers[i].name,name)==0) return i; return -1; }
static int  peer_find_by_mac_unsafe(const mesh_addr_t *mac){ for(int i=0;i<MAX_PEERS;i++) if(C.peers[i].valid && mac_equal(&C.peers[i].mac,mac)) return i; return -1; }
static int  peer_free_slot_unsafe(void){ for(int i=0;i<MAX_PEERS;i++) if(!C.peers[i].valid) return i; int o=0; for(int i=1;i<MAX_PEERS;i++) if(C.peers[i].last_ms<C.peers[o].last_ms) o=i; return o; }
static void peer_upsert(const char *name, const mesh_addr_t *mac){ if(!name||!*name||!mac) return; xSemaphoreTake(C.lock,portMAX_DELAY); int idx=peer_find_by_name_unsafe(name); if(idx<0) idx=peer_free_slot_unsafe(); strlcpy(C.peers[idx].name,name,sizeof(C.peers[idx].name)); C.peers[idx].mac=*mac; C.peers[idx].valid=true; C.peers[idx].last_ms=now_ms(); xSemaphoreGive(C.lock); }
static bool peer_resolve(const char *name, mesh_addr_t *out){ if(!name||!*name) return false; bool ok=false; xSemaphoreTake(C.lock,portMAX_DELAY); int idx=peer_find_by_name_unsafe(name); if(idx>=0){ *out=C.peers[idx].mac; ok=true; C.peers[idx].last_ms=now_ms(); } xSemaphoreGive(C.lock); return ok; }

// mqtt helpers
static inline void mqtt_retained_clear(const char *topic){ mqtt_link_publish_cb(topic, "", 1, true); }

// topology fingerprint
static uint32_t compute_topology_crc(void){
    mesh_addr_t *snap = (mesh_addr_t*)calloc(MAX_RT_SNAPSHOT, sizeof(mesh_addr_t));
    if (!snap) return 0;
    int n = rt_snapshot(snap, MAX_RT_SNAPSHOT);
    qsort(snap, n, sizeof(mesh_addr_t), cmp_mac);
    uint8_t header[1+6+4+6];
    header[0] = C.is_root ? 1 : 0;
    memcpy(&header[1], C.root_mac.addr, 6);
    memcpy(&header[7], &C.root_epoch, 4);
    memcpy(&header[11], C.mesh_id, 6);
    uint32_t crc = esp_rom_crc32_le(0, header, sizeof(header));
    crc = esp_rom_crc32_le(crc, (const uint8_t*)snap, n*sizeof(mesh_addr_t));
    free(snap);
    return crc;
}

// publish per-root Current
static void publish_root_current(int node_count, uint32_t topo_hash){
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "mesh_id", C.mesh_id_hex);
    char root_mac_s[18]; mac_str(C.root_mac.addr, root_mac_s, sizeof root_mac_s);
    cJSON_AddStringToObject(o, "root_mac", root_mac_s);
    cJSON_AddStringToObject(o, "root_dev", C.O.local_dev ? C.O.local_dev : "?");
    cJSON_AddNumberToObject(o, "root_epoch", C.root_epoch);
    cJSON_AddBoolToObject(o,   "is_root", C.is_root);
    cJSON_AddNumberToObject(o, "published_ms", now_ms());
    cJSON_AddNumberToObject(o, "node_count", node_count);
    cJSON_AddNumberToObject(o, "topology_hash", topo_hash);
    char topic[128]; snprintf(topic, sizeof topic, "Mesh/%s/Root/Current/%s", C.mesh_id_hex, root_mac_s);
    char *payload = cJSON_PrintUnformatted(o);
    if (payload){ mqtt_link_publish_cb(topic, payload, 1, true); free(payload); }
    cJSON_Delete(o);
}

// janitor: only clear topics for nodes in our current tree (former roots), never our own root
static void janitor_cleanup_nodes(const mesh_addr_t *snap, int n){
    char mac_s[18], topic[160];
    for (int i=0;i<n;i++){
        if (mac_equal(&snap[i], &C.root_mac)) continue;
        mac_str(snap[i].addr, mac_s, sizeof mac_s);
        snprintf(topic, sizeof topic, "Mesh/%s/Root/%s/RouteTable", C.mesh_id_hex, mac_s);
        mqtt_retained_clear(topic);
        snprintf(topic, sizeof topic, "Mesh/%s/Root/Current/%s", C.mesh_id_hex, mac_s);
        mqtt_retained_clear(topic);
    }
}

// routing diff baseline
static void rt_diff_and_update_baseline(bool publish_offline){
    (void)publish_offline; // not implemented in this trimmed backend
    if (!C.is_root) return;
    mesh_addr_t *now = (mesh_addr_t*)calloc(MAX_RT_SNAPSHOT, sizeof(mesh_addr_t));
    int now_n = now ? rt_snapshot(now, MAX_RT_SNAPSHOT) : 0;
    uint32_t topo = compute_topology_crc();
    if (topo != C.last_topo_crc){ publish_route_event("ROUTE_DIFF"); C.last_topo_crc = topo; }
    C.rt_prev_n = (now_n <= MAX_RT_SNAPSHOT) ? now_n : MAX_RT_SNAPSHOT;
    if (C.rt_prev_n>0 && now) memcpy(C.rt_prev, now, C.rt_prev_n*sizeof(mesh_addr_t));
    free(now);
}

// publish route event + pointer + janitor
static void publish_route_event(const char *ev_name){
    mesh_addr_t *snap = (mesh_addr_t*)calloc(MAX_RT_SNAPSHOT, sizeof(mesh_addr_t));
    int n = snap ? rt_snapshot(snap, MAX_RT_SNAPSHOT) : 0;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", ev_name);
    cJSON_AddStringToObject(root, "mesh_id", C.mesh_id_hex);
    char root_mac_s[18]; mac_str(C.root_mac.addr, root_mac_s, sizeof root_mac_s);
    cJSON_AddStringToObject(root, "root_mac", root_mac_s);
    cJSON_AddStringToObject(root, "root_dev", C.O.local_dev ? C.O.local_dev : "?");
    cJSON_AddBoolToObject(root,   "is_root", C.is_root);
    cJSON_AddNumberToObject(root, "root_epoch", C.root_epoch);
    uint32_t topo = compute_topology_crc();
    cJSON_AddNumberToObject(root, "topology_hash", topo);
    cJSON_AddNumberToObject(root, "published_ms", now_ms());
    cJSON *nodes = cJSON_CreateArray();
    for (int i=0;i<n;i++){ char mac[18]; mac_str(snap[i].addr, mac, sizeof mac); cJSON_AddItemToArray(nodes, cJSON_CreateString(mac)); }
    cJSON_AddItemToObject(root, "nodes", nodes);
    char topic[160]; snprintf(topic, sizeof topic, "Mesh/%s/Root/%s/RouteTable", C.mesh_id_hex, root_mac_s);
    char *payload = cJSON_PrintUnformatted(root);
    if (payload){ mqtt_link_publish_cb(topic, payload, 1, true); free(payload); }
    cJSON_Delete(root);
    publish_root_current(n, topo);
    if (ev_name && strcmp(ev_name, "HEARTBEAT") != 0 && snap) janitor_cleanup_nodes(snap, n);
    if (snap) free(snap);
}

// ---- TTL sweep for stale roots ----
#define MAX_SEEN_ROOTS 12
typedef struct { char mac[18]; uint64_t last_ms; } seen_root_t;
static seen_root_t SEEN[MAX_SEEN_ROOTS];
static uint32_t ROOT_TTL_MS = 90000; // 90s

static void seen_root_touch(const char *mac){
    if (!mac||!*mac) return;
    for (int i=0;i<MAX_SEEN_ROOTS;i++){
        if (SEEN[i].mac[0] && strcmp(SEEN[i].mac, mac)==0){ SEEN[i].last_ms=now_ms(); return; }
    }
    for (int i=0;i<MAX_SEEN_ROOTS;i++) if (!SEEN[i].mac[0]){ strlcpy(SEEN[i].mac, mac, sizeof(SEEN[i].mac)); SEEN[i].last_ms=now_ms(); return; }
}

static bool mac_in_snapshot(const mesh_addr_t *snap, int n, const char *mac_s){
    char buf[18];
    for (int i=0;i<n;i++){ mac_str(snap[i].addr, buf, sizeof buf); if (strcmp(buf, mac_s)==0) return true; }
    return false;
}

static void sweep_stale_roots(void){
    if (!C.is_root) return;
    mesh_addr_t *snap = (mesh_addr_t*)calloc(MAX_RT_SNAPSHOT, sizeof(mesh_addr_t));
    int n = snap ? rt_snapshot(snap, MAX_RT_SNAPSHOT) : 0;
    char my_mac[18]; mac_str(C.root_mac.addr, my_mac, sizeof my_mac);
    uint64_t now = now_ms();
    for (int i=0;i<MAX_SEEN_ROOTS;i++){
        if (!SEEN[i].mac[0]) continue;
        if (strcmp(SEEN[i].mac, my_mac)==0) continue; // never self
        if (mac_in_snapshot(snap, n, SEEN[i].mac)) continue; // merged under us -> janitor already handles
        if (now - SEEN[i].last_ms > ROOT_TTL_MS){
            char topic[160];
            snprintf(topic, sizeof topic, "Mesh/%s/Root/Current/%s", C.mesh_id_hex, SEEN[i].mac); mqtt_retained_clear(topic);
            snprintf(topic, sizeof topic, "Mesh/%s/Root/%s/RouteTable", C.mesh_id_hex, SEEN[i].mac); mqtt_retained_clear(topic);
            SEEN[i].mac[0] = '\0';
        }
    }
    if (snap) free(snap);
}

// RX handling (lightweight): forwards decoded envelopes
static void handle_packet(const mesh_addr_t *from, const char *json, size_t len){
    (void)len;
    cJSON *o = cJSON_Parse(json); if (!o) return;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(o,"type"));
    const char *src  = cJSON_GetStringValue(cJSON_GetObjectItem(o,"src_dev"));
    const char *dst  = cJSON_GetStringValue(cJSON_GetObjectItem(o,"dst_dev"));
    uint32_t corr_id = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(o,"corr_id"));
    cJSON *payload   = cJSON_GetObjectItem(o,"payload");
    if (src) peer_upsert(src, from);
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
    if      (type && strcmp(type,"RESPONSE")==0){
        // no special handling
    } else if (type && strcmp(type,"REQUEST")==0){ if (C.on_req) C.on_req(&e); }
      else if (type && strcmp(type,"EVENT")==0){ if (C.on_evt) C.on_evt(&e); }
    cJSON_Delete(o);
}

static void rx_loop(void *arg){
    (void)arg;
    uint8_t *buf = malloc(MESH_PAYLOAD_MAX);
    if (!buf) { vTaskDelete(NULL); return; }
    for(;;){
        mesh_addr_t from = {0};
        mesh_data_t data = { .data = buf, .size = MESH_PAYLOAD_MAX, .proto = 0, .tos = 0 };
        int flag = 0;
        esp_err_t err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        if (err == ESP_OK && data.data && data.size>0){
            size_t n = data.size; if (n >= MESH_PAYLOAD_MAX) n = MESH_PAYLOAD_MAX-1; ((char*)data.data)[n]='\0';
            handle_packet(&from, (const char*)data.data, n);
        }
    }
}

// worker + events
static void root_hb_timer_cb(TimerHandle_t xTimer){ (void)xTimer; if (!C.is_root) return; work_msg_t w={.type=W_HEARTBEAT}; xQueueSend(s_workq,&w,0); }
static void root_hb_start(void){ if (!C.hb_interval_ms) C.hb_interval_ms = 20000; if (!C.hb_timer) C.hb_timer = xTimerCreate("mesh_hb", pdMS_TO_TICKS(C.hb_interval_ms), pdTRUE, NULL, root_hb_timer_cb); if (C.hb_timer) xTimerStart(C.hb_timer,0); }
static void root_hb_stop(void){ if (C.hb_timer) xTimerStop(C.hb_timer,0); }

static void backend_worker(void *arg){
    (void)arg;
    work_msg_t m;
    for(;;){
        if (xQueueReceive(s_workq, &m, portMAX_DELAY) != pdTRUE) continue;
        switch(m.type){
            case W_ROOT_CHANGE:
                C.is_root = m.now_root; if (C.on_root) C.on_root(m.now_root);
                if (C.is_root){ C.rt_prev_n=0; C.last_topo_crc=0; C.root_epoch++; publish_route_event("ROOT_ELECTED"); rt_diff_and_update_baseline(false); root_hb_start(); }
                else { C.rt_prev_n=0; root_hb_stop(); }
                break;
            case W_RT_ADD:    publish_route_event("ADD");    rt_diff_and_update_baseline(false); break;
            case W_RT_REMOVE: publish_route_event("REMOVE"); rt_diff_and_update_baseline(true);  break;
            case W_CHILD_ADD: rt_diff_and_update_baseline(false); break;
            case W_CHILD_REMOVE: rt_diff_and_update_baseline(true); break;
            case W_HEARTBEAT: publish_route_event("HEARTBEAT"); sweep_stale_roots(); break;
        }
    }
}

static void espmesh_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data){
    (void)arg; (void)base; (void)data;
    switch(id){
        case MESH_EVENT_ROUTING_TABLE_ADD:    { work_msg_t w={.type=W_RT_ADD}; xQueueSend(s_workq,&w,0); break; }
        case MESH_EVENT_ROUTING_TABLE_REMOVE: { work_msg_t w={.type=W_RT_REMOVE}; xQueueSend(s_workq,&w,0); break; }
        case MESH_EVENT_CHILD_CONNECTED:      { work_msg_t w={.type=W_CHILD_ADD}; xQueueSend(s_workq,&w,0); break; }
        case MESH_EVENT_CHILD_DISCONNECTED:   { work_msg_t w={.type=W_CHILD_REMOVE}; xQueueSend(s_workq,&w,0); break; }
        case MESH_EVENT_ROOT_ADDRESS: {
            const mesh_event_root_address_t *ev = (const mesh_event_root_address_t*)data; memcpy(C.root_mac.addr, ev->addr, 6); C.root_mac_known=true; work_msg_t w={.type=W_ROOT_CHANGE, .now_root=esp_mesh_is_root()}; xQueueSend(s_workq,&w,0); break; }
        default: break;
    }
}

// mesh stack
static void apply_mesh_cfg(void){
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    cfg.channel = CONFIG_MESH_CHANNEL;
    const size_t r_ssid_len = strlen(CONFIG_MESH_ROUTER_SSID);
    const size_t r_pass_len = strlen(CONFIG_MESH_ROUTER_PASSWD);
    cfg.router.ssid_len = r_ssid_len;
    memcpy((uint8_t*)cfg.router.ssid, CONFIG_MESH_ROUTER_SSID, r_ssid_len);
    if (r_pass_len) memcpy((uint8_t*)cfg.router.password, CONFIG_MESH_ROUTER_PASSWD, r_pass_len);
    const uint8_t mesh_id[6] = { CONFIG_MESH_ID_0, CONFIG_MESH_ID_1, CONFIG_MESH_ID_2, CONFIG_MESH_ID_3, CONFIG_MESH_ID_4, CONFIG_MESH_ID_5 };
    memcpy(cfg.mesh_id.addr, mesh_id, 6);
    memcpy(C.mesh_id, mesh_id, 6);
    mesh_id_to_hex(C.mesh_id_hex);
    cfg.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
}

static void start_rx_task_once(void){ if (!C.rx_task) xTaskCreate(rx_loop, "mesh_rx", 4096, NULL, 5, &C.rx_task); }

static void init_mesh_stack(const mesh_opts_t *opts){
    (void)opts;
    ESP_ERROR_CHECK(esp_mesh_init());
    apply_mesh_cfg();
    esp_mesh_set_self_organized(true, true);
    esp_mesh_fix_root(false);
    esp_mesh_allow_root_conflicts(false);
    esp_mesh_set_max_layer(6);
    ESP_ERROR_CHECK(esp_event_handler_instance_register(MESH_EVENT, ESP_EVENT_ANY_ID, espmesh_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_mesh_start());
    esp_wifi_get_mac(WIFI_IF_STA, C.local_mac.addr);
    C.root_epoch = 0; C.last_topo_crc = 0; C.is_root = esp_mesh_is_root(); if (C.on_root) C.on_root(C.is_root);
    // subscribe to per-root Current stream for TTL tracking
    subscribe_root_current_stream();
}

// backend API
static void init(const mesh_opts_t *opts){
    mesh_request_cb_t saved_req  = C.on_req;
    mesh_event_cb_t   saved_evt  = C.on_evt;
    mesh_root_cb_t    saved_root = C.on_root;
    memset(&C,0,sizeof C);
    C.O = *opts;
    C.on_req  = saved_req; C.on_evt = saved_evt; C.on_root = saved_root;
    C.lock = xSemaphoreCreateMutex();
    s_workq = xQueueCreate(8, sizeof(work_msg_t));
    xTaskCreate(backend_worker, "mesh_bkw", 6144, NULL, 5, NULL);
    C.is_root=false; C.root_mac_known=false;
    init_mesh_stack(opts);
    start_rx_task_once();
}

static void register_rx(mesh_request_cb_t on_request, mesh_event_cb_t on_event){ C.on_req=on_request; C.on_evt=on_event; }
static void register_root(mesh_root_cb_t cb){ C.on_root=cb; }

static bool resolve_dst(const char *dst_dev, mesh_addr_t *out){
    if (!dst_dev || !*dst_dev || strcmp(dst_dev,"*ROOT*")==0){ if (C.root_mac_known){ *out=C.root_mac; return true; } return false; }
    return peer_resolve(dst_dev, out);
}

static int pend_alloc(uint32_t corr_id){ xSemaphoreTake(C.lock,portMAX_DELAY); int idx=-1; for(int i=0;i<MAX_PENDING;i++) if(!C.pend[i].used){ idx=i; break; } if(idx>=0){ C.pend[idx].corr_id=corr_id; C.pend[idx].sem=xSemaphoreCreateBinary(); C.pend[idx].st=MESH_TIMEOUT; C.pend[idx].used=true; } xSemaphoreGive(C.lock); return idx; }
static mesh_status_t pend_wait_and_free(int idx, uint32_t timeout_ms){ if(idx<0) return MESH_ERR; mesh_status_t st=MESH_TIMEOUT; if(xSemaphoreTake(C.pend[idx].sem,pdMS_TO_TICKS(timeout_ms))==pdTRUE){ st=C.pend[idx].st; } vSemaphoreDelete(C.pend[idx].sem); xSemaphoreTake(C.lock,portMAX_DELAY); C.pend[idx].used=false; xSemaphoreGive(C.lock); return st; }

static char* build_json(const char *type, const mesh_envelope_t *e){ cJSON *o=cJSON_CreateObject(); cJSON_AddStringToObject(o,"schema","v1"); cJSON_AddStringToObject(o,"type",type); cJSON_AddNumberToObject(o,"corr_id", e?e->corr_id:0); cJSON_AddNumberToObject(o,"ts_ms", e?e->ts_ms:now_ms()); cJSON_AddStringToObject(o,"src_dev", e?e->src_dev:C.O.local_dev); if(e&&e->dst_dev) cJSON_AddStringToObject(o,"dst_dev", e->dst_dev); if(e&&e->payload) cJSON_AddItemToObject(o,"payload", cJSON_Duplicate(e->payload,1)); char *js=cJSON_PrintUnformatted(o); cJSON_Delete(o); return js; }

static esp_err_t mesh_send_json(const mesh_addr_t *to, const char *js){ mesh_data_t md={0}; md.data=(uint8_t*)js; md.size=strlen(js)+1; md.proto=MESH_PROTO_BIN; md.tos=MESH_TOS_P2P; return esp_mesh_send((mesh_addr_t*)to,&md,MESH_DATA_P2P,NULL,0); }

static mesh_status_t request(const mesh_envelope_t *req, uint32_t timeout_ms){ mesh_addr_t dst; if(!resolve_dst(req->dst_dev,&dst)) return MESH_NO_ROUTE; int p=pend_alloc(req->corr_id); if(p<0) return MESH_ERR; char *js=build_json("REQUEST",req); esp_err_t er=mesh_send_json(&dst, js); free(js); if(er!=ESP_OK){ (void)pend_wait_and_free(p,0); return MESH_NO_ROUTE; } return pend_wait_and_free(p, timeout_ms); }
static mesh_status_t send_event(const mesh_envelope_t *evt){ mesh_addr_t dst; if(!resolve_dst(evt->dst_dev,&dst)) return MESH_NO_ROUTE; char *js=build_json("EVENT",evt); esp_err_t er=mesh_send_json(&dst, js); free(js); return (er==ESP_OK)?MESH_OK:MESH_ERR; }

static cJSON* snapshot(void){ int cap=esp_mesh_get_routing_table_size(); int got=0; mesh_addr_t *tbl=(cap>0)?(mesh_addr_t*)calloc(cap,sizeof(mesh_addr_t)):NULL; if(tbl) esp_mesh_get_routing_table(tbl,cap*sizeof(mesh_addr_t),&got); cJSON *arr=cJSON_CreateArray(); for(int i=0;i<got;i++){ char mac[18]; snprintf(mac,sizeof mac,"%02x:%02x:%02x:%02x:%02x:%02x", tbl[i].addr[0],tbl[i].addr[1],tbl[i].addr[2],tbl[i].addr[3],tbl[i].addr[4],tbl[i].addr[5]); cJSON_AddItemToArray(arr, cJSON_CreateString(mac)); } free(tbl); return arr; }

// vtable export
typedef struct { const char* (*name)(void); void (*init)(const mesh_opts_t*); void (*register_rx)(mesh_request_cb_t, mesh_event_cb_t); void (*register_root)(mesh_root_cb_t); mesh_status_t (*request)(const mesh_envelope_t*, uint32_t); mesh_status_t (*send_event)(const mesh_envelope_t*); cJSON* (*snapshot)(void); } ml_backend_t;

const ml_backend_t* ml_backend_espmesh(void){ static const ml_backend_t V={ name_backend, init, register_rx, register_root, request, send_event, snapshot }; return &V; }

// ----- MQTT extra subscribe (Root/Current/#) -----
static void on_mqtt_root_current(const char *topic, const char *payload){
    (void)payload;
    // topic: Mesh/<mesh_id>/Root/Current/<root_mac>
    const char *last = strrchr(topic, '/');
    if (!last || !*(last+1)) return;
    seen_root_touch(last+1);
}

static void subscribe_root_current_stream(void){
    char pat[96];
    snprintf(pat, sizeof pat, "Mesh/%s/Root/Current/+", C.mesh_id_hex);
    mqtt_link_subscribe_extra(pat, 1, on_mqtt_root_current);
}
