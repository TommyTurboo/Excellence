#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Verwerk een volledige configuratie (of patch) uit MQTT.
// - json:  payload van Devices/<node>/Config/Set
// - local_dev: naam van dit device (bv. MQTT_CLIENT_ID)
// Doet: JSON → cfg_t vullen → NVS opslaan → drivers herstarten → ACK/ERROR publish.
void cfg_mqtt_handle(const char *json, const char *local_dev);

#ifdef __cplusplus
}
#endif
