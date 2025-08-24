#include "input_ctrl.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"

#ifndef INPUT_CH_MAX
#define INPUT_CH_MAX 16
#endif

static const char *TAG = "input_ctrl";

static int       s_ch_count = 0;
static int       s_gpio_map[INPUT_CH_MAX];
static uint32_t  s_pullup_mask   = 0;
static uint32_t  s_pulldown_mask = 0;
static uint32_t  s_inverted_mask = 0;
static bool      s_level[INPUT_CH_MAX];         // logisch, na inversion
static uint32_t  s_debounce_ms[INPUT_CH_MAX];
static esp_timer_handle_t s_db_timer[INPUT_CH_MAX];
static input_state_hook_t s_hook = NULL;
static bool      s_irq_enabled = false;
static bool s_isr_installed = false;

static inline bool ch_in_range(int ch){ return (ch >= 0) && (ch < s_ch_count); }
static inline bool is_bit(uint32_t m, int ch){ return (m >> ch) & 0x1; }

static bool read_logical(int ch)
{
    int gpio = s_gpio_map[ch];
    int raw  = gpio_get_level(gpio);
    bool lvl = (raw != 0);
    if (is_bit(s_inverted_mask, ch)) lvl = !lvl;
    return lvl;
}

static inline void timer_stop_safe(esp_timer_handle_t t)
{
    if (!t) return;
    esp_err_t e = esp_timer_stop(t);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "timer_stop err=%s", esp_err_to_name(e));
    }
}

static void debounce_cb(void *arg)
{
    int ch = (int)(intptr_t)arg;
    if (!ch_in_range(ch)) return;

    bool lvl = read_logical(ch);
    if (lvl != s_level[ch]) {
        s_level[ch] = lvl;
        if (s_hook) s_hook(ch, lvl);
    }
    // na debounce weer interrupts toelaten voor deze GPIO (al actief, ISR blijft aan)
}

static void IRAM_ATTR gpio_isr(void *arg)
{
    int ch = (int)(intptr_t)arg;
    // (re)start debounce timer; eigenlijke lees/vergelijk in task context
    if (ch >= 0 && ch < s_ch_count) {
        uint64_t us = (uint64_t)s_debounce_ms[ch] * 1000ULL;
        // stop-safe + start-once is veiliger tegen stormen
        timer_stop_safe(s_db_timer[ch]);
        esp_timer_start_once(s_db_timer[ch], us);
    }
}

esp_err_t input_ctrl_init(const int *gpio_map, int ch_count,
                          uint32_t pullup_mask, uint32_t pulldown_mask,
                          uint32_t inverted_mask, uint32_t debounce_ms_def)
{
    if (!gpio_map || ch_count <= 0 || ch_count > INPUT_CH_MAX) return ESP_ERR_INVALID_ARG;

    s_ch_count      = ch_count;
    s_pullup_mask   = pullup_mask;
    s_pulldown_mask = pulldown_mask;
    s_inverted_mask = inverted_mask;

    // init arrays + GPIO
    for (int ch = 0; ch < s_ch_count; ch++) {
        s_gpio_map[ch]  = gpio_map[ch];
        s_debounce_ms[ch] = debounce_ms_def ? debounce_ms_def : 20; // default 20ms
        s_db_timer[ch]  = NULL;

        int gpio = s_gpio_map[ch];
        gpio_reset_pin(gpio);

        gpio_config_t io = {
            .pin_bit_mask = (1ULL << gpio),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = is_bit(s_pullup_mask, ch)   ? GPIO_PULLUP_ENABLE  : GPIO_PULLUP_DISABLE,
            .pull_down_en = is_bit(s_pulldown_mask, ch) ? GPIO_PULLDOWN_ENABLE: GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_ANYEDGE,
        };
        ESP_ERROR_CHECK(gpio_config(&io));

        // init logische beginwaarde
        s_level[ch] = read_logical(ch);

        // maak debounce timer
        esp_timer_create_args_t tcfg = {
            .callback = debounce_cb,
            .arg      = (void*)(intptr_t)ch,
            .name     = "in_db",
        };
        ESP_ERROR_CHECK(esp_timer_create(&tcfg, &s_db_timer[ch]));
    }

    // ISR service installeren & handlers registreren
    if (!s_isr_installed) {
        esp_err_t err = gpio_install_isr_service(0);
        if (err == ESP_ERR_INVALID_STATE) {
            // iemand anders of vorige init had ‘m al
            s_isr_installed = true;
            err = ESP_OK;
        } else if (err == ESP_OK) {
            s_isr_installed = true;
        }
        ESP_ERROR_CHECK(err);
    }
    for (int ch = 0; ch < s_ch_count; ch++) {
        ESP_ERROR_CHECK(gpio_isr_handler_add(s_gpio_map[ch], gpio_isr, (void*)(intptr_t)ch));
    }
    s_irq_enabled = true;

    ESP_LOGI(TAG, "init ok: ch=%d, pullup=0x%08x, pulldown=0x%08x, inverted=0x%08x, debounce_def=%ums",
             s_ch_count, (unsigned)s_pullup_mask, (unsigned)s_pulldown_mask,
             (unsigned)s_inverted_mask, (unsigned)debounce_ms_def);
    return ESP_OK;
}

esp_err_t input_ctrl_deinit(void)
{
    // verwijder handlers en zet pins terug ‘veilig’
    for (int ch = 0; ch < s_ch_count; ++ch) {
        int gpio = s_gpio_map[ch];          // zorg dat je s_gpio_map[] bij init bewaart
        gpio_isr_handler_remove(gpio);
        gpio_intr_disable(gpio);
        gpio_reset_pin(gpio);               // ok als dit ontbreekt; dan weglaten
    }
    if (s_isr_installed) {
        gpio_uninstall_isr_service();
        s_isr_installed = false;
    }
    s_ch_count = 0;
    return ESP_OK;
}

esp_err_t input_ctrl_set_debounce_ms(int ch, uint32_t ms)
{
    if (!ch_in_range(ch)) return ESP_ERR_INVALID_ARG;
    s_debounce_ms[ch] = ms;
    return ESP_OK;
}

bool input_ctrl_get_level(int ch)
{
    if (!ch_in_range(ch)) return false;
    return s_level[ch];
}

void input_ctrl_set_state_hook(input_state_hook_t hook)
{
    s_hook = hook;
}

esp_err_t input_ctrl_enable_irq(bool enable)
{
    if (enable == s_irq_enabled) return ESP_OK;
    for (int ch = 0; ch < s_ch_count; ch++) {
        gpio_intr_enable(s_gpio_map[ch]);
    }
    s_irq_enabled = true;
    if (!enable) {
        for (int ch = 0; ch < s_ch_count; ch++) {
            gpio_intr_disable(s_gpio_map[ch]);
        }
        s_irq_enabled = false;
    }
    return ESP_OK;
}
