// components/wifi_link/include/wifi_link.h
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h> 

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_evt_cb)(void);

typedef struct {
    char ssid[33];
    char pass[65];
    char hostname[32];

    bool     use_static_ip;         // false = DHCP
    uint32_t ip;                    // network order (e.g. PP_HTONL(IPADDR))
    uint32_t gw;
    uint32_t mask;

    // reconnect backoff
    uint32_t backoff_min_ms;        // e.g. 500
    uint32_t backoff_max_ms;        // e.g. 10000

    bool power_save;                // modem sleep
} wifi_ctx_t;

typedef struct {
    wifi_evt_cb on_got_ip;          // called after IP_EVENT_STA_GOT_IP
    wifi_evt_cb on_disconnected;    // called on WIFI_EVENT_STA_DISCONNECTED
} wifi_cbs_t;

// API
void wifi_link_init(const wifi_ctx_t *ctx, const wifi_cbs_t *cbs);
void wifi_link_start(void);
void wifi_link_stop(void);
bool wifi_link_is_connected(void);
bool wifi_link_get_ip(char *buf, size_t sz); // "192.168.1.23"
int  wifi_link_get_rssi(void);               // dBm (negatief)
void wifi_link_set_ps(bool enable);

#ifdef __cplusplus
}
#endif
