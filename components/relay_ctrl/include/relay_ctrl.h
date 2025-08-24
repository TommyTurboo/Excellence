#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*relay_state_hook_t)(int ch, bool on);

esp_err_t relay_ctrl_init(const int *gpio_map, int ch_count, uint32_t active_low_mask, uint32_t open_drain_mask);

/**
 * @brief De-initialiseer relaisdriver: outputs naar ‘uit’, ISR/IO vrijgeven.
 *        Idempotent: meerdere keren aanroepen is OK.
 */
esp_err_t relay_ctrl_deinit(void);

esp_err_t relay_ctrl_on(int ch);
esp_err_t relay_ctrl_off(int ch);
esp_err_t relay_ctrl_toggle(int ch);
esp_err_t relay_ctrl_set_autoff_seconds(int ch, uint32_t seconds);
bool      relay_ctrl_is_on(int ch);

/**
 * @brief Haal de huidige staat van een kanaal op.
 * 
 * @param ch Kanaalnummer (0..count-1)
 * @return true als relais aan staat, false als uit staat
 */
bool relay_ctrl_get_state(int ch);

void      relay_ctrl_set_state_hook(relay_state_hook_t hook);

#ifdef __cplusplus
}
#endif
