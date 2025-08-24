#include "config_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_log.h"
#include <string.h>
#include "esp_check.h"

static const char *TAG = "config_store";
#define NS "cfg"    // NVS namespace
#define VER_CUR 1

// eenvoudige mutex (FreeRTOS)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_mtx;

static cfg_t s_cfg;        // cache in RAM
static nvs_handle_t s_nvs; // open handle

/* --- helpers --- */
static void lock(void){ if(s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock(void){ if(s_mtx) xSemaphoreGive(s_mtx); }

static void default_dev_name(char out[32]){
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, 32, "ESP32_%02X%02X%02X%02X%02X%02X",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t config_reset_defaults(cfg_t *out){
    if(!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->version = VER_CUR;

    default_dev_name(out->dev_name);

    // RELAY
    out->relay_count = 0;
    for(int i=0;i<RELAY_CH_MAX;i++){
        out->relay_gpio[i] = -1;
        out->relay_autoff_sec[i] = 0;
    }
    out->relay_active_low_mask = 0;
    out->relay_open_drain_mask = 0;

    // PWM
    out->pwm_count = 0;
    for(int i=0;i<PWM_CH_MAX;i++) out->pwm_gpio[i] = -1;
    out->pwm_inverted_mask = 0;
    out->pwm_freq_hz = 5000;

    // INPUT
    out->input_count = 0;
    for(int i=0;i<INPUT_CH_MAX;i++){
        out->input_gpio[i] = -1;
        out->input_debounce_ms[i] = 30;
    }
    out->input_pullup_mask = 0;
    out->input_pulldown_mask = 0;
    out->input_inverted_mask = 0;

    return ESP_OK;
}

bool config_validate(const cfg_t *c){
    if(!c) return false;
    // simpele checks
    if(c->relay_count<0 || c->relay_count>RELAY_CH_MAX) return false;
    if(c->pwm_count  <0 || c->pwm_count  >PWM_CH_MAX)   return false;
    if(c->input_count<0 || c->input_count>INPUT_CH_MAX) return false;

    // gpio range/unique check (lichte)
    // (optioneel uitbreiden: geen -1 dubbel controleren)
    return true;
}

/* --- NVS read/write primitives --- */
static esp_err_t nvs_write_blob(const char* key, const void* data, size_t len){
    return nvs_set_blob(s_nvs, key, data, len);
}
static esp_err_t nvs_read_blob(const char* key, void* data, size_t len){
    size_t got=len; return nvs_get_blob(s_nvs, key, data, &got);
}
static esp_err_t nvs_write_u32(const char* key, uint32_t v){
    return nvs_set_u32(s_nvs, key, v);
}
static esp_err_t nvs_read_u32(const char* key, uint32_t *v){
    return nvs_get_u32(s_nvs, key, v);
}
static esp_err_t nvs_write_str(const char* key, const char* s){
    return nvs_set_str(s_nvs, key, s);
}
static esp_err_t nvs_read_str(const char* key, char* buf, size_t buflen){
    size_t n=buflen; return nvs_get_str(s_nvs, key, buf, &n);
}

/* --- load/save --- */
esp_err_t config_load(cfg_t *out){
    if(!out) return ESP_ERR_INVALID_ARG;
    cfg_t tmp;
    config_reset_defaults(&tmp);

    uint32_t ver=0;
    if(nvs_read_u32("v", &ver)!=ESP_OK || ver!=VER_CUR){
        // geen config of andere versie -> defaults
        *out = tmp; return ESP_OK;
    }

    // device
    nvs_read_str("dev", tmp.dev_name, sizeof(tmp.dev_name));

    // relay
    nvs_read_u32("ry_n", (uint32_t*)&tmp.relay_count);
    nvs_read_blob("ry_p", tmp.relay_gpio, sizeof(tmp.relay_gpio));
    nvs_read_u32("ry_al", &tmp.relay_active_low_mask);
    nvs_read_u32("ry_od", &tmp.relay_open_drain_mask);
    nvs_read_blob("ry_ao", tmp.relay_autoff_sec, sizeof(tmp.relay_autoff_sec));

    // pwm
    nvs_read_u32("pw_n", (uint32_t*)&tmp.pwm_count);
    nvs_read_blob("pw_p", tmp.pwm_gpio, sizeof(tmp.pwm_gpio));
    nvs_read_u32("pw_inv", &tmp.pwm_inverted_mask);
    nvs_read_u32("pw_f", &tmp.pwm_freq_hz);

    // input
    nvs_read_u32("in_n", (uint32_t*)&tmp.input_count);
    nvs_read_blob("in_p", tmp.input_gpio, sizeof(tmp.input_gpio));
    nvs_read_u32("in_pu", &tmp.input_pullup_mask);
    nvs_read_u32("in_pd", &tmp.input_pulldown_mask);
    nvs_read_u32("in_inv", &tmp.input_inverted_mask);
    nvs_read_blob("in_db", tmp.input_debounce_ms, sizeof(tmp.input_debounce_ms));

    if(!config_validate(&tmp)) return ESP_ERR_INVALID_STATE;
    *out = tmp;
    return ESP_OK;
}

static esp_err_t save_to_nvs_atomic(const cfg_t *c){
    // simpele “atomic”: eerst alles schrijven, dan versie updaten, dan commit
    ESP_RETURN_ON_FALSE(config_validate(c), ESP_ERR_INVALID_ARG, TAG, "invalid config");

    // device
    ESP_ERROR_CHECK(nvs_write_str("dev", c->dev_name));

    // relay
    ESP_ERROR_CHECK(nvs_write_u32("ry_n", c->relay_count));
    ESP_ERROR_CHECK(nvs_write_blob("ry_p", c->relay_gpio, sizeof(c->relay_gpio)));
    ESP_ERROR_CHECK(nvs_write_u32("ry_al", c->relay_active_low_mask));
    ESP_ERROR_CHECK(nvs_write_u32("ry_od", c->relay_open_drain_mask));
    ESP_ERROR_CHECK(nvs_write_blob("ry_ao", c->relay_autoff_sec, sizeof(c->relay_autoff_sec)));

    // pwm
    ESP_ERROR_CHECK(nvs_write_u32("pw_n", c->pwm_count));
    ESP_ERROR_CHECK(nvs_write_blob("pw_p", c->pwm_gpio, sizeof(c->pwm_gpio)));
    ESP_ERROR_CHECK(nvs_write_u32("pw_inv", c->pwm_inverted_mask));
    ESP_ERROR_CHECK(nvs_write_u32("pw_f", c->pwm_freq_hz));

    // input
    ESP_ERROR_CHECK(nvs_write_u32("in_n", c->input_count));
    ESP_ERROR_CHECK(nvs_write_blob("in_p", c->input_gpio, sizeof(c->input_gpio)));
    ESP_ERROR_CHECK(nvs_write_u32("in_pu", c->input_pullup_mask));
    ESP_ERROR_CHECK(nvs_write_u32("in_pd", c->input_pulldown_mask));
    ESP_ERROR_CHECK(nvs_write_u32("in_inv", c->input_inverted_mask));
    ESP_ERROR_CHECK(nvs_write_blob("in_db", c->input_debounce_ms, sizeof(c->input_debounce_ms)));

    // als alles ok: versie schrijven
    ESP_ERROR_CHECK(nvs_write_u32("v", VER_CUR));

    // commit
    return nvs_commit(s_nvs);
}

/* --- public API --- */
esp_err_t config_init(void){
    if(!s_mtx) s_mtx = xSemaphoreCreateMutex();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &s_nvs));

    cfg_t tmp;
    config_load(&tmp); // defaults of NVS
    lock(); s_cfg = tmp; unlock();

    ESP_LOGI(TAG, "init: dev=%s", s_cfg.dev_name);
    return ESP_OK;
}

const cfg_t* config_get_cached(void){
    return &s_cfg; // read-only gebruiken!
}

esp_err_t config_save(const cfg_t *in){
    if(!in) return ESP_ERR_INVALID_ARG;
    lock(); s_cfg = *in; esp_err_t r = save_to_nvs_atomic(&s_cfg); unlock();
    return r;
}

esp_err_t config_erase_all(void){
    lock();
    esp_err_t r = nvs_erase_all(s_nvs);
    if(r==ESP_OK) r = nvs_commit(s_nvs);
    unlock();
    return r;
}

/* setters op cache */
esp_err_t config_set_dev_name(const char *name){
    if(!name) return ESP_ERR_INVALID_ARG;
    lock(); strncpy(s_cfg.dev_name, name, sizeof(s_cfg.dev_name)-1); s_cfg.dev_name[31]='\0'; unlock();
    return ESP_OK;
}

/* relay */
esp_err_t config_set_relays(const int *gpio, int count){
    if(!gpio || count<0 || count>RELAY_CH_MAX) return ESP_ERR_INVALID_ARG;
    lock();
    s_cfg.relay_count = count;
    memset(s_cfg.relay_gpio, -1, sizeof(s_cfg.relay_gpio));
    memcpy(s_cfg.relay_gpio, gpio, sizeof(int)*count);
    unlock();
    return ESP_OK;
}
esp_err_t config_set_relay_masks(uint32_t active_low, uint32_t open_drain){
    lock(); s_cfg.relay_active_low_mask=active_low; s_cfg.relay_open_drain_mask=open_drain; unlock();
    return ESP_OK;
}
esp_err_t config_set_relay_autoff(int ch, uint32_t sec){
    if(ch<0 || ch>=RELAY_CH_MAX) return ESP_ERR_INVALID_ARG;
    lock(); s_cfg.relay_autoff_sec[ch]=sec; unlock();
    return ESP_OK;
}

/* pwm */
esp_err_t config_set_pwm_channels(const int *gpio, int count){
    if(!gpio || count<0 || count>PWM_CH_MAX) return ESP_ERR_INVALID_ARG;
    lock();
    s_cfg.pwm_count = count;
    memset(s_cfg.pwm_gpio, -1, sizeof(s_cfg.pwm_gpio));
    memcpy(s_cfg.pwm_gpio, gpio, sizeof(int)*count);
    unlock();
    return ESP_OK;
}
esp_err_t config_set_pwm_inverted(uint32_t mask){
    lock(); s_cfg.pwm_inverted_mask=mask; unlock(); return ESP_OK;
}
esp_err_t config_set_pwm_freq(uint32_t hz){
    if(hz==0) return ESP_ERR_INVALID_ARG;
    lock(); s_cfg.pwm_freq_hz=hz; unlock(); return ESP_OK;
}

/* input */
esp_err_t config_set_inputs(const int *gpio, int count){
    if(!gpio || count<0 || count>INPUT_CH_MAX) return ESP_ERR_INVALID_ARG;
    lock();
    s_cfg.input_count = count;
    memset(s_cfg.input_gpio, -1, sizeof(s_cfg.input_gpio));
    memcpy(s_cfg.input_gpio, gpio, sizeof(int)*count);
    unlock();
    return ESP_OK;
}
esp_err_t config_set_input_masks(uint32_t pullup, uint32_t pulldown, uint32_t inverted){
    lock(); s_cfg.input_pullup_mask=pullup; s_cfg.input_pulldown_mask=pulldown; s_cfg.input_inverted_mask=inverted; unlock();
    return ESP_OK;
}
esp_err_t config_set_input_debounce(int ch, uint32_t ms){
    if(ch<0 || ch>=INPUT_CH_MAX) return ESP_ERR_INVALID_ARG;
    lock(); s_cfg.input_debounce_ms[ch]=ms; unlock();
    return ESP_OK;
}

/* commit */
esp_err_t config_commit(void){
    lock(); esp_err_t r = save_to_nvs_atomic(&s_cfg); unlock();
    return r;
}
