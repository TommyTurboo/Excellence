#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RELAY_CH_MAX
#define RELAY_CH_MAX 16
#endif
#ifndef PWM_CH_MAX
#define PWM_CH_MAX 16
#endif
#ifndef INPUT_CH_MAX
#define INPUT_CH_MAX 16
#endif

typedef struct {
    char     dev_name[32];

    // RELAY
    int      relay_count;
    int      relay_gpio[RELAY_CH_MAX];
    uint32_t relay_active_low_mask;
    uint32_t relay_open_drain_mask;
    uint32_t relay_autoff_sec[RELAY_CH_MAX];

    // PWM
    int      pwm_count;
    int      pwm_gpio[PWM_CH_MAX];
    uint32_t pwm_inverted_mask;
    uint32_t pwm_freq_hz;

    // INPUT
    int      input_count;
    int      input_gpio[INPUT_CH_MAX];
    uint32_t input_pullup_mask;
    uint32_t input_pulldown_mask;
    uint32_t input_inverted_mask;
    uint32_t input_debounce_ms[INPUT_CH_MAX];

    // versie
    uint32_t version;  // =1
} cfg_t;

/* Lifecycle */
esp_err_t   config_init(void);               // nvs init/open + load or defaults
esp_err_t   config_load(cfg_t *out);         // expliciet uit NVS lezen
esp_err_t   config_save(const cfg_t *in);    // valideer + atomic save
esp_err_t   config_reset_defaults(cfg_t *out);
esp_err_t   config_erase_all(void);
const cfg_t* config_get_cached(void);        // read-only pointer

/* Setters op cache (RAM). Daarna config_commit() voor NVS. */
esp_err_t   config_set_dev_name(const char *name);

/* Relay */
esp_err_t   config_set_relays(const int *gpio, int count);
esp_err_t   config_set_relay_masks(uint32_t active_low, uint32_t open_drain);
esp_err_t   config_set_relay_autoff(int ch, uint32_t sec);

/* PWM */
esp_err_t   config_set_pwm_channels(const int *gpio, int count);
esp_err_t   config_set_pwm_inverted(uint32_t mask);
esp_err_t   config_set_pwm_freq(uint32_t hz);

/* Input */
esp_err_t   config_set_inputs(const int *gpio, int count);
esp_err_t   config_set_input_masks(uint32_t pullup, uint32_t pulldown, uint32_t inverted);
esp_err_t   config_set_input_debounce(int ch, uint32_t ms);

/* Commit RAM->NVS (atomic) */
esp_err_t   config_commit(void);

/* Extra */
bool        config_validate(const cfg_t *c);

#ifdef __cplusplus
}
#endif
