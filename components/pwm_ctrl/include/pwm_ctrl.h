#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PWM_CH_MAX
#define PWM_CH_MAX 16
#endif

/**
 * @brief Hook wordt aangeroepen bij duty-wijziging.
 * @param ch   Kanaalnummer
 * @param duty Duty in raw LEDC ticks (0..max)
 */
typedef void (*pwm_state_hook_t)(int ch, uint32_t duty);

/**
 * @brief Initialiseer PWM-kanalen.
 * 
 * @param gpio_map      Array met GPIO-nummers per kanaal.
 * @param ch_count      Aantal kanalen (max PWM_CH_MAX).
 * @param inverted_mask Bitmask: 1 = duty inverted voor dat kanaal.
 * @param freq_hz       Frequentie in Hz.
 * @return esp_err_t    ESP_OK of foutcode.
 */
esp_err_t pwm_ctrl_init(const int *gpio_map, int ch_count,
                        uint32_t inverted_mask, uint32_t freq_hz);

esp_err_t pwm_ctrl_deinit(void);

/**
 * @brief Zet duty cycle voor kanaal.
 * @param ch     Kanaalnummer.
 * @param duty   Duty in raw LEDC ticks (0..max).
 */
esp_err_t pwm_ctrl_set_duty(int ch, uint32_t duty);

/**
 * @brief Lees huidige duty cycle.
 */
esp_err_t pwm_ctrl_get_duty(int ch, uint32_t *duty_out);

/**
 * @brief Fade naar een nieuwe duty in opgegeven tijd.
 * 
 * @param ch            Kanaalnummer.
 * @param duty          Nieuwe duty in raw LEDC ticks (0..max).
 * @param fade_time_ms  Fade tijd in milliseconden.
 * @return esp_err_t    ESP_OK of foutcode.
 */
esp_err_t pwm_ctrl_fade_to(int ch, uint32_t duty, uint32_t fade_time_ms);

/**
 * @brief Start PWM-output op huidig duty-niveau.
 */
esp_err_t pwm_ctrl_start(int ch);

/**
 * @brief Stop PWM-output (duty = 0).
 */
esp_err_t pwm_ctrl_stop(int ch);

/**
 * @brief Stel hookfunctie in voor duty-wijzigingen.
 */
void pwm_ctrl_set_state_hook(pwm_state_hook_t hook);

#ifdef __cplusplus
}
#endif
