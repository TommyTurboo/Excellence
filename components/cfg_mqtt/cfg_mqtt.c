#include "cfg_mqtt.h"
#include "config_store.h"
#include "relay_ctrl.h"
#include "pwm_ctrl.h"
#include "input_ctrl.h"
#include "mqtt_link.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_system.h"   // voor esp_restart()
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"     // vTaskDelay, pdMS_TO_TICKS
#include "router.h"

#define LENOF(a) ((int)(sizeof(a)/sizeof((a)[0])))

static const char *TAG = "cfg_mqtt";

typedef enum { ROLE_NONE=0, ROLE_RELAY, ROLE_PWM, ROLE_INPUT } gpio_role_t;

static const char* role_str(gpio_role_t r){
    switch(r){
        case ROLE_RELAY: return "RELAY";
        case ROLE_PWM:   return "PWM";
        case ROLE_INPUT: return "INPUT";
        default:         return "NONE";
    }
}

static inline bool is_input_only(int gpio){ return (gpio >=34 && gpio <= 39); }
static inline bool is_spi_flash_pin(int gpio){ return (gpio >= 6 && gpio <= 11); }
static inline bool is_valid_gpio(int gpio){ return (gpio >= 0 && gpio <= 39); }

// claim één GPIO voor een bepaalde rol; faalt bij conflict/ongeldige pin
static bool claim_pin(int gpio, gpio_role_t role, gpio_role_t used[],
                      char *why, size_t why_sz,
                      int *conf_gpio, gpio_role_t *a, gpio_role_t *b)
{
    if (!is_valid_gpio(gpio)) {
        snprintf(why, why_sz, "invalid gpio %d", gpio);
        *conf_gpio = gpio; *a = ROLE_NONE; *b = role;
        return false;
    }
    if (is_spi_flash_pin(gpio)) {
        snprintf(why, why_sz, "gpio %d is reserved for SPI flash", gpio);
        *conf_gpio = gpio; *a = used[gpio]; *b = role;
        return false;
    }
    if ((role==ROLE_RELAY || role==ROLE_PWM) && is_input_only(gpio)) {
        snprintf(why, why_sz, "gpio %d is input-only; not allowed for %s", gpio, role_str(role));
        *conf_gpio = gpio; *a = used[gpio]; *b = role;
        return false;
    }
    if (used[gpio] != ROLE_NONE && used[gpio] != role) {
        snprintf(why, why_sz, "gpio %d used by %s and %s", gpio, role_str(used[gpio]), role_str(role));
        *conf_gpio = gpio; *a = used[gpio]; *b = role;
        return false;
    }
    if (used[gpio] == role) {
        snprintf(why, why_sz, "gpio %d is duplicated in %s list", gpio, role_str(role));
        *conf_gpio = gpio; *a = role; *b = role;
        return false;
    }
    used[gpio] = role;
    return true;
}

static bool validate_gpio_exclusivity(const cfg_t *cfg,
                                      char *why, size_t why_sz,
                                      int *conf_gpio, gpio_role_t *a, gpio_role_t *b)
{
    gpio_role_t used[40];
    for (int i=0; i<40; ++i) used[i] = ROLE_NONE;

    // Relays
    for (int i=0; i<cfg->relay_count; ++i)
        if (!claim_pin(cfg->relay_gpio[i], ROLE_RELAY, used, why, why_sz, conf_gpio, a, b))
            return false;

    // PWM
    for (int i=0; i<cfg->pwm_count; ++i)
        if (!claim_pin(cfg->pwm_gpio[i], ROLE_PWM, used, why, why_sz, conf_gpio, a, b))
            return false;

    // Inputs
    for (int i=0; i<cfg->input_count; ++i)
        if (!claim_pin(cfg->input_gpio[i], ROLE_INPUT, used, why, why_sz, conf_gpio, a, b))
            return false;

    return true;
}

static void publish_cfg_state(const char *local_dev, const char *corr_id,
                              const char *status, const char *detail)
{
    char topic[96];
    snprintf(topic, sizeof(topic), "Devices/%s/State", local_dev);

    char body[256];
    int n = snprintf(body, sizeof(body),
        "{ \"corr_id\":\"%s\",\"dev\":\"%s\",\"type\":\"CONFIG\",\"status\":\"%s\"",
        corr_id ? corr_id : "", local_dev, status);
    if (detail && *detail) n += snprintf(body+n, sizeof(body)-n, ",\"detail\":\"%s\"", detail);
    snprintf(body+n, sizeof(body)-n, " }");

    mqtt_link_publish_cb(topic, body, /*qos=*/1, /*retain=*/false);
}

static const char* read_opt_str(cJSON *obj, const char *key, char *out, size_t outsz){
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring) {
        snprintf(out, outsz, "%s", it->valuestring);
        return out;
    }
    return NULL;
}

static bool read_opt_u32(cJSON *obj, const char *key, uint32_t *v){
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it) return false;
    if (!cJSON_IsNumber(it)) return false;
    *v = (uint32_t)(it->valuedouble);
    return true;
}

static int read_u32_array(cJSON *arr, uint32_t *out, int maxn){
    if (!cJSON_IsArray(arr)) return -1;
    int n = cJSON_GetArraySize(arr);
    if (n > maxn) n = maxn;
    for (int i=0;i<n;i++){
        cJSON *it = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsNumber(it)) return -1;
        out[i] = (uint32_t)(it->valuedouble);
    }
    return n;
}

static int read_i32_array(cJSON *arr, int *out, int maxn){
    if (!cJSON_IsArray(arr)) return -1;
    int n = cJSON_GetArraySize(arr);
    if (n > maxn) n = maxn;
    for (int i=0;i<n;i++){
        cJSON *it = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsNumber(it)) return -1;
        out[i] = (int)(it->valuedouble);
    }
    return n;
}

// Bouw HELLO payload met samenvatting van de actuele IO-mapping
static void cfg_emit_hello_now(const cfg_t *cfg){
    if (!cfg) return;

    cJSON *h = cJSON_CreateObject();
    cJSON_AddStringToObject(h, "type", "HELLO");

    // optioneel: naam als info-blok
    cJSON *dev = cJSON_CreateObject();
    cJSON_AddStringToObject(dev, "name", cfg->dev_name);
    cJSON_AddItemToObject(h, "device", dev);

    // korte samenvatting
    cJSON_AddNumberToObject(h, "relay_count", cfg->relay_count);
    cJSON_AddNumberToObject(h, "pwm_count",   cfg->pwm_count);
    cJSON_AddNumberToObject(h, "input_count", cfg->input_count);

    // relays
    cJSON *rel = cJSON_CreateObject();
    cJSON_AddNumberToObject(rel, "count", cfg->relay_count);
    cJSON_AddNumberToObject(rel, "active_low_mask", cfg->relay_active_low_mask);
    cJSON_AddNumberToObject(rel, "open_drain_mask", cfg->relay_open_drain_mask);
    cJSON *rel_gpio = cJSON_CreateArray();
    for (int i=0;i<cfg->relay_count;i++) cJSON_AddItemToArray(rel_gpio, cJSON_CreateNumber(cfg->relay_gpio[i]));
    cJSON_AddItemToObject(rel, "gpio", rel_gpio);
    cJSON *rel_aut = cJSON_CreateArray();
    for (int i=0;i<cfg->relay_count;i++) cJSON_AddItemToArray(rel_aut, cJSON_CreateNumber((int)cfg->relay_autoff_sec[i]));
    cJSON_AddItemToObject(rel, "autoff_sec", rel_aut);
    cJSON_AddItemToObject(h, "relays", rel);

    // pwm
    cJSON *pwm = cJSON_CreateObject();
    cJSON_AddNumberToObject(pwm, "count", cfg->pwm_count);
    cJSON_AddNumberToObject(pwm, "inverted_mask", cfg->pwm_inverted_mask);
    cJSON_AddNumberToObject(pwm, "freq_hz", cfg->pwm_freq_hz);
    cJSON *pwm_gpio = cJSON_CreateArray();
    for (int i=0;i<cfg->pwm_count;i++) cJSON_AddItemToArray(pwm_gpio, cJSON_CreateNumber(cfg->pwm_gpio[i]));
    cJSON_AddItemToObject(pwm, "gpio", pwm_gpio);
    cJSON_AddItemToObject(h, "pwm", pwm);

    // inputs
    cJSON *in = cJSON_CreateObject();
    cJSON_AddNumberToObject(in, "count", cfg->input_count);
    cJSON_AddNumberToObject(in, "pullup_mask",   cfg->input_pullup_mask);
    cJSON_AddNumberToObject(in, "pulldown_mask", cfg->input_pulldown_mask);
    cJSON_AddNumberToObject(in, "inverted_mask", cfg->input_inverted_mask);
    cJSON *in_gpio = cJSON_CreateArray();
    for (int i=0;i<cfg->input_count;i++) cJSON_AddItemToArray(in_gpio, cJSON_CreateNumber(cfg->input_gpio[i]));
    cJSON_AddItemToObject(in, "gpio", in_gpio);
    cJSON *in_db = cJSON_CreateArray();
    for (int i=0;i<cfg->input_count;i++) cJSON_AddItemToArray(in_db, cJSON_CreateNumber((int)cfg->input_debounce_ms[i]));
    cJSON_AddItemToObject(in, "debounce_ms", in_db);
    cJSON_AddItemToObject(h, "inputs", in);

    // via mesh naar root → root publiceert retained Info
    router_emit_event(ML_KIND_DIAG, /*corr_id*/0, /*origin*/NULL, h);
    cJSON_Delete(h);
}


void cfg_publish_hello_now(void){
    const cfg_t *cfg = config_get_cached();
    if (cfg) cfg_emit_hello_now(cfg);
}

void cfg_mqtt_handle(const char *json, const char *local_dev)
{
    if (!json || !local_dev || !*local_dev) return;

    cJSON *root = cJSON_Parse(json);
    if (!root) { publish_cfg_state(local_dev, "", "ERROR", "INVALID_JSON"); return; }

    // corr_id (optioneel, voor correlatie)
    const char *corr_id = "";
    char corr_copy[64] = {0};
    cJSON *cid = cJSON_GetObjectItemCaseSensitive(root, "corr_id");
    if (cJSON_IsString(cid) && cid->valuestring) {
    snprintf(corr_copy, sizeof(corr_copy), "%s", cid->valuestring);
    corr_id = corr_copy;  // <-- blijven gebruiken na cJSON_Delete
    }

    // target_dev (optioneel) – we zijn al op de juiste node (root heeft geforward)
    cJSON *tgt = cJSON_GetObjectItemCaseSensitive(root, "target_dev");
    if (cJSON_IsString(tgt) && tgt->valuestring && strcmp(tgt->valuestring, local_dev)!=0) {
        cJSON_Delete(root);
        publish_cfg_state(local_dev, corr_id, "ERROR", "WRONG_TARGET");
        return;
    }

    const cfg_t *cur = config_get_cached();
    if (!cur) { cJSON_Delete(root); publish_cfg_state(local_dev, corr_id, "ERROR", "CONFIG_NOT_READY"); return; }

    cfg_t tmp = *cur;   // start van huidige config
    bool any_change = false;
    bool name_changed = false;
    char old_name[sizeof(tmp.dev_name)];
    strlcpy(old_name, tmp.dev_name, sizeof(old_name));

    // ===== device =====
    cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
    if (cJSON_IsObject(device)) {
        char name[LENOF(tmp.dev_name) ? LENOF(tmp.dev_name) : 64]; // fallback
        if (read_opt_str(device, "name", name, sizeof(name))) {
            if (strcmp(tmp.dev_name, name) != 0) {
                snprintf(tmp.dev_name, sizeof(tmp.dev_name), "%s", name);
                name_changed = true;
            }
            any_change = true;
        }
    }

    // ===== relays =====
    cJSON *rel = cJSON_GetObjectItemCaseSensitive(root, "relays");
    if (cJSON_IsObject(rel)) {
        // gpio array → count
        cJSON *gpio = cJSON_GetObjectItemCaseSensitive(rel, "gpio");
        if (gpio) {
            int pins[32];
            int n = read_i32_array(gpio, pins, LENOF(tmp.relay_gpio));
            if (n < 0) { cJSON_Delete(root); publish_cfg_state(local_dev, corr_id, "ERROR", "relays.gpio invalid"); return; }
            // simpele range-check GPIO
            for (int i=0;i<n;i++){ if (pins[i] < 0 || pins[i] > 39) { cJSON_Delete(root); publish_cfg_state(local_dev, corr_id, "ERROR", "relays.gpio out of range"); return; } }
            tmp.relay_count = n;
            for (int i=0;i<n;i++) tmp.relay_gpio[i] = pins[i];
            any_change = true;
        }
        // masks
        uint32_t mask;
        if (read_opt_u32(rel, "active_low_mask", &mask)) { tmp.relay_active_low_mask = mask; any_change = true; }
        if (read_opt_u32(rel, "open_drain_mask", &mask)) { tmp.relay_open_drain_mask = mask; any_change = true; }
        // autoff array
        cJSON *aut = cJSON_GetObjectItemCaseSensitive(rel, "autoff_sec");
        if (aut) {
            uint32_t sec[32];
            int n = read_u32_array(aut, sec, LENOF(tmp.relay_autoff_sec));
            if (n < 0) { cJSON_Delete(root); publish_cfg_state(local_dev, corr_id, "ERROR", "relays.autoff_sec invalid"); return; }
            int m = (n < tmp.relay_count) ? n : tmp.relay_count;
            for (int i=0;i<m;i++) tmp.relay_autoff_sec[i] = sec[i];
            any_change = true;
        }
    }

    // ===== pwm =====
    cJSON *pwm = cJSON_GetObjectItemCaseSensitive(root, "pwm");
    if (cJSON_IsObject(pwm)) {
        cJSON *gpio = cJSON_GetObjectItemCaseSensitive(pwm, "gpio");
        if (gpio) {
            int pins[32];
            int n = read_i32_array(gpio, pins, LENOF(tmp.pwm_gpio));
            if (n < 0) { cJSON_Delete(root); publish_cfg_state(local_dev, corr_id, "ERROR", "pwm.gpio invalid"); return; }
            for (int i=0;i<n;i++){ if (pins[i] < 0 || pins[i] > 39) { cJSON_Delete(root); publish_cfg_state(local_dev, corr_id, "ERROR", "pwm.gpio out of range"); return; } }
            tmp.pwm_count = n;
            for (int i=0;i<n;i++) tmp.pwm_gpio[i] = pins[i];
            any_change = true;
        }
        uint32_t mask;
        if (read_opt_u32(pwm, "inverted_mask", &mask)) { tmp.pwm_inverted_mask = mask; any_change = true; }
        uint32_t freq;
        if (read_opt_u32(pwm, "freq_hz", &freq)) {
            if (freq < 50 || freq > 40000) { cJSON_Delete(root); publish_cfg_state(local_dev, corr_id, "ERROR", "pwm.freq_hz out of range"); return; }
            tmp.pwm_freq_hz = freq; any_change = true;
        }
    }

    // ===== inputs =====
    cJSON *in = cJSON_GetObjectItemCaseSensitive(root, "inputs");
    if (cJSON_IsObject(in)) {
        cJSON *gpio = cJSON_GetObjectItemCaseSensitive(in, "gpio");
        if (gpio) {
            int pins[32];
            int n = read_i32_array(gpio, pins, LENOF(tmp.input_gpio));
            if (n < 0) { cJSON_Delete(root); publish_cfg_state(local_dev, corr_id, "ERROR", "inputs.gpio invalid"); return; }
            for (int i=0;i<n;i++){ if (pins[i] < 0 || pins[i] > 39) { cJSON_Delete(root); publish_cfg_state(local_dev, corr_id, "ERROR", "inputs.gpio out of range"); return; } }
            tmp.input_count = n;
            for (int i=0;i<n;i++) tmp.input_gpio[i] = pins[i];
            any_change = true;
        }
        uint32_t mask;
        if (read_opt_u32(in, "pullup_mask",   &mask)) { tmp.input_pullup_mask   = mask; any_change = true; }
        if (read_opt_u32(in, "pulldown_mask", &mask)) { tmp.input_pulldown_mask = mask; any_change = true; }
        if (read_opt_u32(in, "inverted_mask", &mask)) { tmp.input_inverted_mask = mask; any_change = true; }

        cJSON *db = cJSON_GetObjectItemCaseSensitive(in, "debounce_ms");
        if (db) {
            uint32_t ms[32];
            int n = read_u32_array(db, ms, LENOF(tmp.input_debounce_ms));
            if (n < 0) { cJSON_Delete(root); publish_cfg_state(local_dev, corr_id, "ERROR", "inputs.debounce_ms invalid"); return; }
            int m = (n < tmp.input_count) ? n : tmp.input_count;
            for (int i=0;i<m;i++) tmp.input_debounce_ms[i] = ms[i];
            any_change = true;
        }
    }

    if (!any_change) { cJSON_Delete(root); publish_cfg_state(local_dev, corr_id, "ERROR", "NO_EFFECT"); return; }

    // ===== Drivers herstarten (re-init met nieuwe mapping) =====
    // NB: Gebruik huidige tmp (net opgeslagen) voor init.

    char why[96];
    int bad = -1;
    gpio_role_t rA = ROLE_NONE, rB = ROLE_NONE;
    if (!validate_gpio_exclusivity(&tmp, why, sizeof(why), &bad, &rA, &rB)) {
        ESP_LOGE(TAG, "config rejected: %s (gpio=%d, %s vs %s)", why, bad, role_str(rA), role_str(rB));
        publish_cfg_state(local_dev, corr_id, "ERROR", why);
        cJSON_Delete(root);
        return;
    }

    // 2) Drivers herstarten met nieuwe mapping
    relay_ctrl_deinit();
    if (relay_ctrl_init(tmp.relay_gpio, tmp.relay_count,
                        tmp.relay_active_low_mask, tmp.relay_open_drain_mask) != ESP_OK) {
        publish_cfg_state(local_dev, corr_id, "ERROR", "RELAY_INIT_FAILED");
        cJSON_Delete(root);
        return;
    }
    for (int i = 0; i < tmp.relay_count; i++)
        relay_ctrl_set_autoff_seconds(i, tmp.relay_autoff_sec[i]);

    pwm_ctrl_deinit();
    if (pwm_ctrl_init(tmp.pwm_gpio, tmp.pwm_count, tmp.pwm_inverted_mask, tmp.pwm_freq_hz) != ESP_OK) {
        publish_cfg_state(local_dev, corr_id, "ERROR", "PWM_INIT_FAILED");
        cJSON_Delete(root);
        return;
    }

    input_ctrl_deinit();
    if (input_ctrl_init(tmp.input_gpio, tmp.input_count,
                        tmp.input_pullup_mask, tmp.input_pulldown_mask,
                        tmp.input_inverted_mask, 30) != ESP_OK) {
        publish_cfg_state(local_dev, corr_id, "ERROR", "INPUT_INIT_FAILED");
        cJSON_Delete(root);
        return;
    }
    for (int i = 0; i < tmp.input_count; i++)
        input_ctrl_set_debounce_ms(i, tmp.input_debounce_ms[i]);

    // 3) Pas nu persisteren
    if (config_save(&tmp) != ESP_OK || config_commit() != ESP_OK) {
        publish_cfg_state(local_dev, corr_id, "ERROR", "CONFIG_SAVE_FAILED");
        cJSON_Delete(root);
        return;
    }

    cJSON_Delete(root);
    publish_cfg_state(local_dev, corr_id, "OK", NULL);
    ESP_LOGI(TAG, "full config applied: relays=%d pwm=%d inputs=%d",
            tmp.relay_count, tmp.pwm_count, tmp.input_count);
    
    cfg_emit_hello_now(&tmp);

    if (name_changed) {
    ESP_LOGI(TAG, "Device name changed '%s' -> '%s' → rebooting to apply MQTT/mesh topics",
                old_name, tmp.dev_name);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
}
