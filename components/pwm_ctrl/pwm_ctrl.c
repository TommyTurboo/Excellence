#include "pwm_ctrl.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "pwm_ctrl";

static int s_ch_count = 0;
static int s_gpio_map[PWM_CH_MAX];
static uint32_t s_inverted_mask = 0;
static uint32_t s_freq_hz = 1000;
static uint32_t s_duty[PWM_CH_MAX];
static pwm_state_hook_t s_hook = NULL;
static bool s_fade_installed = false;

static inline bool ch_in_range(int ch) { return (ch >= 0) && (ch < s_ch_count); }
static inline bool is_inverted(int ch) { return (s_inverted_mask >> ch) & 0x1; }

esp_err_t pwm_ctrl_init(const int *gpio_map, int ch_count,
                        uint32_t inverted_mask, uint32_t freq_hz)
{
    if (!gpio_map || ch_count <= 0 || ch_count > PWM_CH_MAX) return ESP_ERR_INVALID_ARG;

    s_ch_count = ch_count;
    s_inverted_mask = inverted_mask;
    s_freq_hz = freq_hz;

    ledc_timer_config_t timer = {
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = s_freq_hz,
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    esp_err_t err = ESP_OK;
        if (!s_fade_installed) {
            err = ledc_fade_func_install(0);
            if (err == ESP_ERR_INVALID_STATE) {
                // was al geïnstalleerd door vorige init
                s_fade_installed = true;
                err = ESP_OK;
            } else if (err == ESP_OK) {
                s_fade_installed = true;
            }
            ESP_ERROR_CHECK(err);
        }

    for (int ch = 0; ch < s_ch_count; ch++) {
        s_gpio_map[ch] = gpio_map[ch];
        s_duty[ch] = 0;

        ledc_channel_config_t channel = {
            .channel    = ch,
            .duty       = 0,
            .gpio_num   = s_gpio_map[ch],
            .speed_mode = LEDC_HIGH_SPEED_MODE,
            .hpoint     = 0,
            .timer_sel  = LEDC_TIMER_0
        };
        ESP_ERROR_CHECK(ledc_channel_config(&channel));
    }

    ESP_LOGI(TAG, "init ok: ch=%d, inverted_mask=0x%08x, freq=%u Hz", s_ch_count, (unsigned int)s_inverted_mask, (unsigned int)s_freq_hz);
    return ESP_OK;
}

esp_err_t pwm_ctrl_deinit(void) {
    for (int ch = 0; ch < s_ch_count; ch++) {
        ledc_stop(LEDC_HIGH_SPEED_MODE, ch, 0);
    }
    if (s_fade_installed) {
        ledc_fade_func_uninstall();
        s_fade_installed = false;
    }
    s_ch_count = 0;
    return ESP_OK;
}

esp_err_t pwm_ctrl_set_duty(int ch, uint32_t duty)
{
    if (!ch_in_range(ch)) return ESP_ERR_INVALID_ARG;

    uint32_t max_duty = (1 << LEDC_TIMER_13_BIT) - 1;
    if (duty > max_duty) duty = max_duty;

    uint32_t set_duty = is_inverted(ch) ? max_duty - duty : duty;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_HIGH_SPEED_MODE, ch, set_duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_HIGH_SPEED_MODE, ch));

    s_duty[ch] = duty;
    if (s_hook) s_hook(ch, duty);

    return ESP_OK;
}

esp_err_t pwm_ctrl_get_duty(int ch, uint32_t *duty_out)
{
    if (!ch_in_range(ch) || !duty_out) return ESP_ERR_INVALID_ARG;
    *duty_out = s_duty[ch];
    return ESP_OK;
}

esp_err_t pwm_ctrl_fade_to(int ch, uint32_t duty, uint32_t fade_time_ms)
{
    if (!ch_in_range(ch)) return ESP_ERR_INVALID_ARG;
    uint32_t max_duty = (1 << LEDC_TIMER_13_BIT) - 1;
    if (duty > max_duty) duty = max_duty;

    uint32_t set_duty = is_inverted(ch) ? max_duty - duty : duty;

    ESP_ERROR_CHECK(ledc_set_fade_with_time(LEDC_HIGH_SPEED_MODE, ch,
                                            set_duty, fade_time_ms));
    ESP_ERROR_CHECK(ledc_fade_start(LEDC_HIGH_SPEED_MODE, ch,
                                    LEDC_FADE_NO_WAIT));

    s_duty[ch] = duty;
    if (s_hook) s_hook(ch, duty);
    return ESP_OK;
}

esp_err_t pwm_ctrl_start(int ch)
{
    return pwm_ctrl_set_duty(ch, s_duty[ch]);
}

esp_err_t pwm_ctrl_stop(int ch)
{
    return pwm_ctrl_set_duty(ch, 0);
}

void pwm_ctrl_set_state_hook(pwm_state_hook_t hook)
{
    s_hook = hook;
}
