#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef INPUT_CH_MAX
#define INPUT_CH_MAX 16
#endif

/**
 * Hook wordt aangeroepen bij een GESTABILISEERDE statuswijziging
 * (na debounce). level = logisch niveau (na inversion).
 */
typedef void (*input_state_hook_t)(int ch, bool level);

/**
 * @brief Initialiseer digitale ingangen (interrupt + debounce).
 *
 * @param gpio_map        array met GPIO-nummers per kanaal
 * @param ch_count        aantal kanalen (<= INPUT_CH_MAX)
 * @param pullup_mask     bitmask: 1 = interne pull-up aan
 * @param pulldown_mask   bitmask: 1 = interne pull-down aan (negeert pull-up als beide 1)
 * @param inverted_mask   bitmask: 1 = logisch niveau wordt omgekeerd
 * @param debounce_ms_def default debounce tijd in ms voor alle kanalen
 */
esp_err_t input_ctrl_init(const int *gpio_map, int ch_count,
                          uint32_t pullup_mask, uint32_t pulldown_mask,
                          uint32_t inverted_mask, uint32_t debounce_ms_def);

esp_err_t input_ctrl_deinit(void);

/** Stel per kanaal de debounce in milliseconden in. */
esp_err_t input_ctrl_set_debounce_ms(int ch, uint32_t ms);

/** Lees huidige LOGISCHE staat (na inversion). */
bool      input_ctrl_get_level(int ch);

/** Stel hook voor statuswijzigingen in. */
void      input_ctrl_set_state_hook(input_state_hook_t hook);

/** (Optioneel) Enable/disable interrupts (globaal). */
esp_err_t input_ctrl_enable_irq(bool enable);

#ifdef __cplusplus
}
#endif
