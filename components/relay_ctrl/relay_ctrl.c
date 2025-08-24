#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "relay_ctrl.h"

#ifndef RELAY_CH_MAX
#define RELAY_CH_MAX 16   // veilige bovengrens; kan via build-flag aangepast worden
#endif

static const char *TAG = "relay_ctrl";

// Config & state
static int       s_ch_count = 0;
static int       s_gpio_map[RELAY_CH_MAX];
static bool      s_state[RELAY_CH_MAX];
static uint32_t  s_active_low_mask = 0;
static uint32_t  s_open_drain_mask = 0;
static uint32_t s_state_mask = 0;
static esp_timer_handle_t s_off_timer[RELAY_CH_MAX];
static uint32_t           s_off_secs [RELAY_CH_MAX];  // gewenste auto-off duur (s)
static relay_state_hook_t s_hook = NULL;
static bool               s_inited = false;

// Helpers
static inline bool ch_in_range(int ch){ return (ch >= 0) && (ch < s_ch_count); }
static inline bool is_active_low(int ch){ return (s_active_low_mask >> ch) & 0x1; }
static inline bool is_open_drain(int ch){ return (s_open_drain_mask >> ch) & 0x1; }

static void apply_level(int ch, bool on)
{
    const int gpio = s_gpio_map[ch];
    const uint32_t level = is_active_low(ch) ? !on : on;
    gpio_set_level(gpio, level);
    s_state[ch] = on;
    if (s_hook) s_hook(ch, on);
}

static void auto_off_cb(void *arg)
{
    const int ch = (int)(intptr_t)arg;
    if (!ch_in_range(ch)) return;
    relay_ctrl_off(ch);
}

static inline void timer_stop_safe(esp_timer_handle_t t)
{
    if (!t) return;
    esp_err_t err = esp_timer_stop(t);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "timer_stop err=%s", esp_err_to_name(err));
    }
}

// Public API
esp_err_t relay_ctrl_init(const int *gpio_map, int ch_count, uint32_t active_low_mask, uint32_t open_drain_mask)
{   
    if (s_inited) {
        // Bij her-config: eerst opruimen
        (void)relay_ctrl_deinit();
    }

    if (!gpio_map || ch_count <= 0 || ch_count > RELAY_CH_MAX) return ESP_ERR_INVALID_ARG;

    s_ch_count = ch_count;
    s_active_low_mask = active_low_mask;
    s_open_drain_mask = open_drain_mask;

    for (int ch = 0; ch < s_ch_count; ++ch) {
        s_gpio_map[ch]  = gpio_map[ch];
        s_state[ch]     = false;
        s_off_secs[ch]  = 0;
        s_off_timer[ch] = NULL;

        const int gpio = s_gpio_map[ch];
        gpio_reset_pin(gpio);

        gpio_config_t io = {
            .pin_bit_mask = (1ULL << gpio),
            .mode         = is_open_drain(ch) ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&io));

        // zet naar ‘uit’-niveau
        gpio_set_level(gpio, is_active_low(ch) ? 1 : 0);

        // maak auto-off timer (exact 1× per kanaal)
        esp_timer_create_args_t tcfg = {
            .callback = auto_off_cb,
            .arg      = (void*)(intptr_t)ch,
            .name     = "relay_off",
        };
        ESP_ERROR_CHECK(esp_timer_create(&tcfg, &s_off_timer[ch]));
    }

    s_inited = true;
    ESP_LOGI(TAG, "init ok: ch=%d, active_low_mask=0x%08x, open_drain_mask=0x%08x",
             s_ch_count, (unsigned)active_low_mask, (unsigned)open_drain_mask);
    return ESP_OK;
}

esp_err_t relay_ctrl_deinit(void)
{
    if (!s_inited) {
        return ESP_OK; // idempotent
    }

    for (int ch = 0; ch < s_ch_count; ++ch) {
        const int gpio_num = s_gpio_map[ch];

        // zet ‘OFF’-level vóór reset (veilig)
        const bool active_low = ((s_active_low_mask >> ch) & 1) != 0;
        const int  off_level  = active_low ? 1 : 0;
        gpio_set_level(gpio_num, off_level);

        if (s_off_timer[ch]) {
        esp_timer_stop(s_off_timer[ch]);
        esp_err_t er = esp_timer_delete(s_off_timer[ch]);
        if (er != ESP_OK) ESP_LOGW(TAG, "timer_delete ch%d: %s", ch, esp_err_to_name(er));
        s_off_timer[ch] = NULL;
        }

        // output uit & pin resetten naar default (input, pull disabled)
        gpio_intr_disable(gpio_num);
        gpio_reset_pin(gpio_num);
    }

    // interne state terug naar nul
    s_ch_count = 0;
    s_active_low_mask = 0;
    s_open_drain_mask = 0;
    s_state_mask = 0;
    s_inited = false;

    ESP_LOGI(TAG, "deinit done");
    return ESP_OK;
}


esp_err_t relay_ctrl_on(int ch)
{
    if (!ch_in_range(ch)) return ESP_ERR_INVALID_ARG;
    apply_level(ch, true);

    if (s_off_secs[ch] > 0) {
        timer_stop_safe(s_off_timer[ch]);
        uint64_t us = (uint64_t)s_off_secs[ch] * 1000000ULL;
        ESP_ERROR_CHECK(esp_timer_start_once(s_off_timer[ch], us));
    }
    return ESP_OK; 
}

esp_err_t relay_ctrl_off(int ch)
{
    if (!ch_in_range(ch)) return ESP_ERR_INVALID_ARG;
    apply_level(ch, false);

    timer_stop_safe(s_off_timer[ch]);
    return ESP_OK;  
}

esp_err_t relay_ctrl_toggle(int ch)
{
    if (!ch_in_range(ch)) return ESP_ERR_INVALID_ARG;
    bool new_on = !s_state[ch];
    apply_level(ch, new_on);
    if (new_on && s_off_secs[ch] > 0) {
        timer_stop_safe(s_off_timer[ch]);
        ESP_ERROR_CHECK(esp_timer_start_once(s_off_timer[ch],
                         (uint64_t)s_off_secs[ch] * 1000000ULL));
    } else if (!new_on) {
        timer_stop_safe(s_off_timer[ch]);
    }
    return ESP_OK;
}

esp_err_t relay_ctrl_set_autoff_seconds(int ch, uint32_t sec)
{
    if (ch < 0 || ch >= s_ch_count) return ESP_ERR_INVALID_ARG;
    s_off_secs[ch] = sec;

    // timer mag altijd veilig gestopt worden
    timer_stop_safe(s_off_timer[ch]);

    // enkel (her)starten als er een duur > 0 is én het kanaal nu AAN is
    if (sec > 0 && s_state[ch]) {
        uint64_t us = (uint64_t)sec * 1000000ULL;
        ESP_ERROR_CHECK(esp_timer_start_once(s_off_timer[ch], us));
    }
    return ESP_OK;
}

bool relay_ctrl_is_on(int ch)
{
    if (!ch_in_range(ch)) return false;
    return s_state[ch];
}

bool relay_ctrl_get_state(int ch)
{
    if (ch < 0 || ch >= s_ch_count) {
        ESP_LOGW(TAG, "relay_ctrl_get_state: invalid channel %d", ch);
        return false;
    }
    return s_state[ch];
}

void relay_ctrl_set_state_hook(relay_state_hook_t hook)
{
    s_hook = hook;
}
