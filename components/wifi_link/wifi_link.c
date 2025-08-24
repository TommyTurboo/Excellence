// components/wifi_link/wifi_link.c
#include "wifi_link.h"
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_link";

static wifi_ctx_t G;
static wifi_cbs_t C;
static esp_netif_t *s_netif = NULL;
static bool s_connected = false;

static esp_event_handler_instance_t s_wifi_any = NULL;
static esp_event_handler_instance_t s_ip_got  = NULL;

static esp_timer_handle_t s_reconnect_timer = NULL;
static uint32_t s_backoff_ms = 0;

static inline uint32_t clamp32(uint32_t v, uint32_t lo, uint32_t hi){ return v<lo?lo:(v>hi?hi:v); }

static void schedule_reconnect(uint32_t delay_ms){
    if (!s_reconnect_timer) return;
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
}

static void reconnect_timer_cb(void*){
    ESP_LOGI(TAG, "reconnect...");
    esp_wifi_connect();
}

static void apply_static_ip_if_needed(void){
    if (!G.use_static_ip || !s_netif) return;
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(s_netif));
    esp_netif_ip_info_t info = {
        .ip.addr = G.ip,
        .gw.addr = G.gw,
        .netmask.addr = G.mask
    };
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &info));
}

static void ensure_nvs(void){
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
}

static void wifi_events(void* arg, esp_event_base_t base, int32_t id, void* data){
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA_START");
                apply_static_ip_if_needed();
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                s_connected = false;
                wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t*)data;
                ESP_LOGW(TAG, "DISCONNECTED reason=%d", e ? e->reason : -1);
                if (C.on_disconnected) C.on_disconnected();
                // exp backoff
                if (s_backoff_ms == 0) s_backoff_ms = clamp32(G.backoff_min_ms?G.backoff_min_ms:500, 100, 60000);
                else s_backoff_ms = clamp32(s_backoff_ms*2, G.backoff_min_ms?G.backoff_min_ms:500, G.backoff_max_ms?G.backoff_max_ms:10000);
                schedule_reconnect(s_backoff_ms);
                break;
            }
            default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_backoff_ms = G.backoff_min_ms?G.backoff_min_ms:500;
        ESP_LOGI(TAG, "GOT_IP");
        if (C.on_got_ip) C.on_got_ip();
    }
}

void wifi_link_init(const wifi_ctx_t *ctx, const wifi_cbs_t *cbs){
    memset(&G, 0, sizeof(G));
    memset(&C, 0, sizeof(C));
    if (ctx) G = *ctx;
    if (cbs) C = *cbs;

    // defaults
    if (G.backoff_min_ms == 0) G.backoff_min_ms = 500;
    if (G.backoff_max_ms == 0) G.backoff_max_ms = 10000;

    // core init (idempotent ok)
    ensure_nvs(); 
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // netif + hostname
    if (!s_netif) s_netif = esp_netif_create_default_wifi_sta();
    if (G.hostname[0]) (void)esp_netif_set_hostname(s_netif, G.hostname);

    // Wi-Fi init
    wifi_init_config_t wicfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wicfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Config SSID/PASS
    wifi_config_t wcfg = {0};
    // ensure null-terminated
    strncpy((char*)wcfg.sta.ssid,     G.ssid, sizeof(wcfg.sta.ssid)-1);
    strncpy((char*)wcfg.sta.password, G.pass, sizeof(wcfg.sta.password)-1);
    wcfg.sta.threshold.authmode = (G.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN);
    wcfg.sta.pmf_cfg.capable = true;
    wcfg.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));

    // Power-save
    esp_wifi_set_ps(G.power_save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);

    // Events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_events, NULL, &s_wifi_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_events, NULL, &s_ip_got));

    // reconnect timer
    if (!s_reconnect_timer) {
        const esp_timer_create_args_t args = {.callback = &reconnect_timer_cb, .name = "wifi_reconn"};
        ESP_ERROR_CHECK(esp_timer_create(&args, &s_reconnect_timer));
    }
}

void wifi_link_start(void){
    ESP_ERROR_CHECK(esp_wifi_start());
    // first connect happens on STA_START
}

void wifi_link_stop(void){
    if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
    (void)esp_wifi_disconnect();
    (void)esp_wifi_stop();
    s_connected = false;
}

bool wifi_link_is_connected(void){
    return s_connected;
}

bool wifi_link_get_ip(char *buf, size_t sz){
    if (!s_netif || !buf || sz == 0) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_netif, &info) != ESP_OK) return false;

    // IDF 5.x: esp_ip4_addr_t → ip4_addr_t (compatibele layout)
    (void)ip4addr_ntoa_r((const ip4_addr_t*)&info.ip, buf, (int)sz);
    return true;
}

int wifi_link_get_rssi(void){
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return -127; // unknown
}

void wifi_link_set_ps(bool enable){
    esp_wifi_set_ps(enable ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}
