# Project Excellence - ESP32 Firmware Specificatie

## Overzicht
Dit document beschrijft de firmware architectuur voor ESP32 nodes in het Excellence mesh netwerk. Alle nodes draaien identieke firmware en configureren zich dynamisch via MQTT berichten.

**Codebase locatie**: `C:\Users\tomva\PlatformIO\Excellence`

## Kernfunctionaliteit

### Bestaande Features (v1.0)
- ✓ ESP-IDF WiFi Mesh (root + child nodes)
- ✓ MQTT client (root only)
- ✓ Dynamische IO configuratie via berichten
- ✓ Relay besturing (ON/OFF met auto-off)
- ✓ PWM dimming (LED verlichting)
- ✓ Input monitoring (drukknoppen, sensors)
- ✓ Command routing (local/remote via mesh)
- ✓ JSON parsing (flexibel, fault-tolerant)
- ✓ NVS configuratie opslag

### Nieuwe Features (v1.1+)
- **Software versie tracking** (semantic versioning)
- **MQTT tijd synchronisatie** (root → nodes via mesh)
- **Periodieke status broadcasts** (30s interval, versie/timestamp/parent/layer)
- **Offline detectie** (mesh events + 90s MQTT timeout)
- **Watchdog & Fallback systeem** (safety voor kritische actuatoren)
- **Network topology monitoring** (parent/layer tracking + events)

## Software Architectuur

### Component Structuur
```
Excellence/
├── src/
│   ├── main.c                          # App lifecycle, init, event handlers
│   └── version.h                       # FW_VERSION define
├── components/
│   ├── mesh_link/                      # WiFi mesh abstraction
│   │   ├── mesh_link.c                 # Façade + vtable
│   │   └── backends/
│   │       ├── backend_espmesh.c       # ESP-IDF mesh implementatie
│   │       └── backend_mailbox.c       # Optioneel (test/dev)
│   ├── mqtt_link/                      # MQTT client (root only)
│   │   └── mqtt_link.c                 # Connect, pub/sub, offline queue
│   ├── router/                         # Command routing logic
│   │   └── router.c                    # Local/remote dispatch, retry
│   ├── parser/                         # JSON → canonieke berichten
│   │   └── parser.c                    # Flexibel, fault-tolerant
│   ├── config_store/                   # NVS configuratie
│   │   └── config_store.c              # Device name, GPIO config
│   ├── cfg_mqtt/                       # MQTT config handler
│   │   └── cfg_mqtt.c                  # Config/Set message processing
│   ├── time_sync/                      # Nieuwe component (v1.1)
│   │   ├── include/time_sync.h
│   │   └── time_sync.c                 # MQTT tijd sync + offset tracking
│   ├── relay_ctrl/                     # Relay driver
│   ├── pwm_ctrl/                       # PWM driver
│   └── input_ctrl/                     # Input driver (GPIO)
```

## Nieuwe/Gewijzigde Componenten

### 1. Version Header
**Locatie:** `src/version.h`
```c
#pragma once
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 1
#define FW_VERSION_PATCH 0
#define FW_VERSION "1.1.0"
```

**Gebruik:**
- Include in `main.c`, status broadcasts, logging
- Root logt warning bij major version mismatch tussen nodes

---

### 2. Time Sync Component (NIEUW)
**Locatie:** `components/time_sync/`

**Doel:** MQTT-gebaseerde tijd synchronisatie (root ontvangt van MQTT, distribueert via mesh)

**API:**
```c
esp_err_t time_sync_init(void);
bool time_sync_is_synced(void);
uint64_t time_sync_get_unix_time(void);        // Returns 0 if not synced
void time_sync_set_time(uint64_t unix_time);   // Root only
esp_err_t time_sync_request(void);             // Nodes: request time van root
```

**Flow:**

**Root:**
1. Subscribe `Mesh/Time` topic (MQTT)
2. Ontvang tijd updates van MQTT broker
3. Call `time_sync_set_time(unix_time)`
4. Beantwoord mesh `TIME_REQUEST` van child nodes

**Child nodes:**
1. Bij boot: `time_sync_request()` → mesh request naar root
2. Ontvang `TIME_RESPONSE` met unix timestamp
3. Sla offset op: `boot_time_offset = unix_time - (esp_timer_get_time() / 1000000)`
4. `time_sync_get_unix_time()`: return `boot_time_offset + uptime_sec`

**Hernieuw tijd:**
- Bij mesh reconnect (na disconnect)
- Periodiek (optioneel, bijv. 1x per uur voor drift correctie)

---

### 3. Mesh Link Uitbreidingen
**Locatie:** `components/mesh_link/backends/backend_espmesh.c`

**Nieuwe functies:**
```c
const char* mesh_get_parent_name(void);        // Device name van parent (NULL if root)
int mesh_get_layer(void);                       // 0=root, 1=first hop, etc.
```

**Nieuwe event callbacks:**
```c
typedef void (*mesh_topology_cb_t)(void);

void mesh_register_topology_cb(mesh_topology_cb_t on_layer_changed,
                                 mesh_topology_cb_t on_parent_changed);
```

**Implementatie:**
- Gebruik ESP-IDF `esp_mesh_get_parent_bsid()` voor parent MAC
- Lookup parent device name via routing table
- Detect layer/parent changes in mesh event handlers
- Trigger callback → status broadcast

---

### 4. Router Uitbreidingen
**Locatie:** `components/router/router.c`

**Nieuwe message types:**
```c
typedef enum {
    ML_KIND_COMMAND = 1,
    ML_KIND_EVENT,
    ML_KIND_STATUS,           // NIEUW: periodieke status broadcasts
    ML_KIND_TIME_REQUEST,     // NIEUW: node vraagt tijd
    ML_KIND_TIME_RESPONSE,    // NIEUW: root antwoordt met tijd
} ml_kind_t;
```

**Nieuwe functies:**
```c
// Verzamel lokale status info (voor broadcast)
cJSON* router_collect_status_data(void);

// Verwerk inkomende status van child nodes
void router_handle_status_broadcast(const mesh_envelope_t *evt);
```

**Status data format:**
```json
{
  "dev": "ESP32_TUIN",
  "version": "1.1.0",
  "timestamp": 1732873200,
  "parent": "ESP32_ROOT",
  "layer": 1,
  "uptime_s": 3600,
  "reason": "periodic" // of "layer_change", "parent_change", "reconnect"
}
```

**Retry mechanisme:**
- Status broadcast via `mesh_send_event()` (fire & forget)
- Bij failure: retry tot 3x (exponential backoff: 1s, 2s, 4s)
- Bij 3x failure: log warning, blijf proberen bij volgende interval

---

### 5. MQTT Link Uitbreidingen
**Locatie:** `components/mqtt_link/mqtt_link.c`

**Nieuwe subscriptions (root only):**
```c
mqtt_link_subscribe("Mesh/Time");              // Tijd updates van broker
mqtt_link_subscribe("Devices/+/Status");       // Status monitoring (watchdog)
```

**Nieuwe functies:**
```c
// Root publiceert tijd naar MQTT (optioneel, periodiek)
void mqtt_link_publish_time(uint64_t unix_time);

// Trigger immediate status broadcast (bij topology change)
void mqtt_link_trigger_status_broadcast(void);
```

**Status broadcast timer:**
```c
static esp_timer_handle_t s_status_timer;

static void status_broadcast_callback(void* arg) {
    // Verzamel lokale status
    cJSON *status = router_collect_status_data();

    // Root: publish direct naar MQTT
    if (mesh_is_root()) {
        mqtt_link_publish("Devices/{dev}/Status", status, QOS1, RETAINED);
        update_network_status_table(status);
    }
    // Child: stuur via mesh naar root
    else {
        mesh_envelope_t env = {
            .kind = ML_KIND_STATUS,
            .payload = status
        };
        mesh_send_event(&env); // Fire & forget met retry
    }
}

// Init: start timer (30s interval)
esp_timer_create_args_t timer_args = {
    .callback = status_broadcast_callback,
    .name = "status_bcast"
};
esp_timer_create(&timer_args, &s_status_timer);
esp_timer_start_periodic(s_status_timer, 30 * 1000000); // 30s in microseconds
```

**Network status tabel (root only):**
```c
typedef struct {
    char dev_name[32];
    char version[16];
    int layer;
    char parent[32];
    uint64_t last_seen;
    bool online;
} node_status_t;

static node_status_t s_network_status[50]; // Max 50 nodes

void update_network_status_table(cJSON *status) {
    // Update entry voor device
    // Publish naar Mesh/Network/Status (retained)
    // Sorteer: layer (0 eerst), dan alfabetisch
}
```

**Offline detectie watchdog:**
```c
static void watchdog_check_callback(void* arg) {
    uint64_t now = time_sync_get_unix_time();

    for (int i = 0; i < num_nodes; i++) {
        if (s_network_status[i].online && (now - s_network_status[i].last_seen > 90)) {
            // 90s = 3x gemist (30s interval)
            s_network_status[i].online = false;

            // Publish offline status
            mqtt_link_publish("Devices/{dev}/Status",
                             "{\"status\":\"offline\",\"reason\":\"timeout\"}",
                             QOS1, RETAINED);

            // Update network tabel
            publish_network_status_table();
        }
    }
}

// Init: watchdog check elke 30s
```

---

### 6. Watchdog & Fallback Systeem (NIEUW)

**Doel:** Lokale safety voor kritische actuatoren (ventiel, pomp, etc.)

**Architectuur:**
```c
typedef struct {
    io_kind_t io_kind;        // RELAY, PWM
    int io_id;
    bool watchdog_enabled;
    uint32_t max_runtime_sec; // 0 = unlimited
    action_t safe_state;      // Bijv. OFF, CLOSE
    uint64_t start_time;      // Timestamp wanneer geactiveerd
    bool is_running;
} watchdog_entry_t;

static watchdog_entry_t s_watchdogs[16]; // Max 16 actuatoren
```

**Command parsing uitbreiding:**
```json
{
  "target_dev": "ESP32_TUIN",
  "io_kind": "RELAY",
  "io_id": 0,
  "action": "ON",
  "max_runtime_sec": 1800,   // 30 min (NIEUW)
  "safe_state": "OFF"         // Bij disconnect/timeout (NIEUW)
}
```

**Watchdog logica:**
```c
void watchdog_start(io_kind_t kind, int id, uint32_t max_runtime_sec, action_t safe_state) {
    watchdog_entry_t *wd = find_watchdog(kind, id);
    wd->watchdog_enabled = (max_runtime_sec > 0);
    wd->max_runtime_sec = max_runtime_sec;
    wd->safe_state = safe_state;
    wd->start_time = time_sync_get_unix_time();
    wd->is_running = true;
}

void watchdog_check_callback(void* arg) {
    uint64_t now = time_sync_get_unix_time();

    for (int i = 0; i < MAX_WATCHDOGS; i++) {
        watchdog_entry_t *wd = &s_watchdogs[i];

        if (!wd->is_running || !wd->watchdog_enabled) continue;

        // Check 1: Runtime timeout
        if (now - wd->start_time >= wd->max_runtime_sec) {
            execute_safe_state(wd);
            publish_event("watchdog_timeout");
        }

        // Check 2: Mesh disconnect (alleen voor child nodes)
        if (!mesh_is_root() && !mesh_is_connected()) {
            execute_safe_state(wd);
            publish_event("mesh_disconnect");
        }
    }
}

void execute_safe_state(watchdog_entry_t *wd) {
    switch (wd->io_kind) {
        case IO_KIND_RELAY:
            relay_ctrl_off(wd->io_id);
            break;
        case IO_KIND_PWM:
            pwm_ctrl_set_duty(wd->io_id, 0);
            break;
    }

    wd->is_running = false;
    ESP_LOGW(TAG, "Watchdog triggered: %s io_id=%d → safe state",
             io_kind_str(wd->io_kind), wd->io_id);
}
```

**Mesh disconnect detection:**
```c
// In backend_espmesh.c event handler
static void mesh_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data) {
    switch (id) {
        case MESH_EVENT_PARENT_DISCONNECTED:
            // Trigger alle watchdogs
            trigger_all_watchdogs("parent_disconnect");
            break;

        case MESH_EVENT_ROOT_LOST:
            // Root node offline
            trigger_all_watchdogs("root_lost");
            break;
    }
}
```

**Configuratie via MQTT:**
Safe state kan worden geconfigureerd per IO:
```json
{
  "relay": {
    "count": 2,
    "gpio": [12, 14],
    "safe_state": ["OFF", "OFF"],      // NIEUW
    "watchdog_enabled": [true, false]  // NIEUW (per relay)
  }
}
```

---

### 7. Main App Aanpassingen
**Locatie:** `src/main.c`

**Init volgorde:**
```c
void app_main(void) {
    // 1. Basis init
    config_init();

    // 2. Drivers
    relay_ctrl_init();
    pwm_ctrl_init();
    input_ctrl_init();

    // 3. Nieuwe componenten
    time_sync_init();

    // 4. Netwerk
    wifi_link_init();
    mesh_register_topology_cb(on_layer_changed, on_parent_changed);
    mesh_register_root_cb(on_mesh_root);
    mesh_init();

    // 5. Status broadcast (wordt gestart in mqtt_link na connect)
}
```

**Event handlers:**
```c
static void on_layer_changed(void) {
    ESP_LOGI(TAG, "Layer changed to %d", mesh_get_layer());
    mqtt_link_trigger_status_broadcast(); // Immediate broadcast
}

static void on_parent_changed(void) {
    ESP_LOGI(TAG, "Parent changed to %s", mesh_get_parent_name());
    mqtt_link_trigger_status_broadcast();
    time_sync_request(); // Hernieuw tijd na parent change
}

static void on_mesh_root(bool is_root) {
    if (is_root) {
        mqtt_link_start();
    } else {
        mqtt_link_stop();
        time_sync_request(); // Vraag tijd aan nieuwe root
    }
}
```

## MQTT Topics & Payloads

**Zie:** `03_MQTT_API_Specificatie.md` voor volledige details

**Quick reference:**
- Commands: `Devices/<device_name>/Cmd/Set`
- Config: `Devices/<device_name>/Config/Set`
- State: `Devices/<device_name>/State` (retained)
- Status: `Devices/<device_name>/Status` (retained, 30s interval)
- Network: `Mesh/Network/Status` (retained)
- Time: `Mesh/Time`

## Implementatie Volgorde

### Fase 1: Basis Infrastructure (Week 1)
- [x] Bestaande codebase review
- [ ] Voeg `src/version.h` toe (FW_VERSION)
- [ ] Maak `time_sync` component
  - [ ] Header + struct definitie
  - [ ] Root: MQTT subscribe + `time_sync_set_time()`
  - [ ] Node: mesh request + offset tracking
- [ ] Mesh uitbreidingen
  - [ ] `mesh_get_parent_name()`
  - [ ] `mesh_get_layer()`
  - [ ] Topology event callbacks
- [ ] Test: tijd sync root → nodes, parent/layer tracking

### Fase 2: Status System (Week 2)
- [ ] Router uitbreidingen
  - [ ] `ML_KIND_STATUS` enum
  - [ ] `router_collect_status_data()`
  - [ ] `router_handle_status_broadcast()`
  - [ ] Retry mechanisme (3x)
- [ ] MQTT_link uitbreidingen
  - [ ] Status broadcast timer (30s)
  - [ ] `mqtt_link_trigger_status_broadcast()`
  - [ ] Network status tabel (root only)
- [ ] Main.c event handlers (topology changes)
- [ ] Test: periodieke status broadcasts, topology change → immediate update

### Fase 3: Watchdog & Safety (Week 3)
- [ ] Watchdog systeem
  - [ ] Watchdog struct + array
  - [ ] `watchdog_start()`, `watchdog_stop()`
  - [ ] Timer callback (check runtime + disconnect)
  - [ ] `execute_safe_state()`
- [ ] Parser uitbreiding
  - [ ] Parse `max_runtime_sec` veld
  - [ ] Parse `safe_state` veld
- [ ] Config_store uitbreiding
  - [ ] Safe state config per IO
  - [ ] Watchdog enable/disable config
- [ ] Mesh disconnect handlers
- [ ] Test: watchdog timeout, disconnect → safe state

### Fase 4: Monitoring & Polish (Week 4)
- [ ] MQTT offline detectie watchdog (root)
- [ ] Network status tabel sorting (layer + alfabetisch)
- [ ] Version mismatch warnings (root)
- [ ] Logging improvements
- [ ] Test: offline detectie (<90s), root reboot, version mixing

## Testing Checklist

**Unit Tests:**
- [ ] Time sync offset berekening correct
- [ ] Watchdog trigger na max_runtime_sec
- [ ] Safe state execution per IO type
- [ ] Status data JSON format valide

**Integration Tests:**
- [ ] 1 root + 2 nodes setup
- [ ] Tijd sync: root → node1 → node2 (via parent)
- [ ] Status broadcast elke 30s ontvangen op MQTT
- [ ] Topology change → immediate status update
- [ ] Node disconnect → offline detectie binnen 90s
- [ ] Watchdog: disconnect → safe state binnen 5s
- [ ] Watchdog: timeout → safe state exact na max_runtime

**Stress Tests:**
- [ ] 10+ nodes, mesh reorganisatie, alle blijven online
- [ ] Root reboot → nodes reconnecten + tijd sync
- [ ] MQTT disconnect/reconnect → geen data verlies (offline queue)

**Edge Cases:**
- [ ] Node zonder tijd sync probeert status broadcast (timestamp = 0)
- [ ] Watchdog zonder tijd sync (gebruik `esp_timer_get_time()` als fallback)
- [ ] Config change tijdens actieve watchdog (reset watchdog)
- [ ] Multiple safe state triggers tegelijk (idempotent)

## Kritieke Bestanden

| Bestand | Wijzigingen |
|---------|-------------|
| `src/main.c` | Init nieuwe modules, topology event handlers |
| `src/version.h` | **NIEUW**: FW_VERSION define |
| `components/time_sync/*` | **NIEUW**: Complete component |
| `components/mesh_link/backends/backend_espmesh.c` | Parent/layer tracking, topology events |
| `components/router/router.c` | STATUS/TIME handlers, watchdog logica, retry |
| `components/mqtt_link/mqtt_link.c` | Time subscription, status timer, offline watchdog, network tabel |
| `components/mesh_link/include/mesh_link.h` | Nieuwe ML_KIND enums, parent/layer API |
| `components/parser/parser.c` | Parse max_runtime_sec, safe_state velden |
| `components/config_store/config_store.c` | Safe state config opslag |

## Backwards Compatibility

**Versie negotiatie:**
- Nodes sturen versie in status broadcasts
- Root logt WARNING bij major version mismatch
- Minor/patch verschillen zijn compatible

**Graceful degradation:**
- Oude nodes zonder status broadcast → offline na 90s (verwacht gedrag)
- Commands zonder `max_runtime_sec` → geen watchdog (zoals voorheen)
- Commands zonder `safe_state` → default safe state uit config

## Performance Overwegingen

**Memory:**
- Time sync: ~200 bytes (offset + sync flag)
- Watchdog: ~50 bytes × max 16 = 800 bytes
- Network status tabel (root only): ~100 bytes × 50 nodes = 5KB

**CPU:**
- Status broadcast timer: 1x per 30s (negligible)
- Watchdog check: 1x per 5s × max 16 entries (negligible)
- Offline watchdog (root): 1x per 30s × max 50 nodes (~10ms)

**MQTT Traffic:**
- Status: 50 nodes × ~200 bytes × 1/30Hz = ~330 bytes/sec
- Acceptable voor lokale MQTT broker

## Toekomstige Uitbreidingen

1. **OTA Updates:** Over-the-air firmware updates via MQTT
2. **Config Backup:** Periodieke backup van config naar MQTT
3. **Energy Monitoring:** Power consumption tracking per actuator
4. **Event History:** Lokale opslag van laatste 100 events (SPIFFS)
5. **Multi-network Support:** Meerdere mesh netwerken (bijv. binnen/buiten gescheiden)
