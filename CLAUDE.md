# Project Excellence - ESP32 WiFi Mesh System

## Project Informatie
- **Type**: ESP-IDF firmware voor ESP32 boards
- **Platform**: PlatformIO
- **Taal**: C
- **Framework**: ESP-IDF 5.4.0 (stable)
- **Codebase**: `C:\Users\tomva\PlatformIO\Excellence`
- **Documentatie**: `C:\Users\tomva\CC AI Map\ProjectExcellence`

## Kernvereisten & Beslissingen

**Architectuur:**
- Uniform code op alle ESP32 boards (root + child nodes)
- ESP-IDF WiFi Mesh (zelfherstellend, multi-layer)
- MQTT als centrale communicatie laag (alleen root heeft MQTT)
- Home Assistant + Node-RED voor automation logica

**Technische Keuzes:**
- **Tijd sync**: MQTT-based (root distribueert via mesh)
- **Offline detectie**: Mesh events (direct) + MQTT timeout (90s = 3x gemist)
- **Versie**: Semantic versioning (`#define FW_VERSION "1.2.3"`)
- **Safety**: Watchdog voor kritische actuatoren (ventiel, pomp)

## Documentatie Structuur

### Gedetailleerde Plan Documenten
1. **[00_Architectuur_Overzicht.md](00_Architectuur_Overzicht.md)** - Systeemdesign, componenten, data flows, ontwerpprincipes
2. **[01_ESP32_Firmware.md](01_ESP32_Firmware.md)** - Firmware spec, componenten, watchdog, ESP-IDF API best practices
3. **[02_NodeRED_HomeAssistant.md](02_NodeRED_HomeAssistant.md)** - Use cases (ventiel, verlichting), flows, automation
4. **[03_MQTT_API_Specificatie.md](03_MQTT_API_Specificatie.md)** - Topics, payloads, contract tussen componenten

**Tip:** Lees eerst 00_Architectuur_Overzicht.md voor het grote plaatje.

## Code Structuur (Bestaand + Gepland v1.1)

```
Excellence/
├── src/
│   ├── main.c                 # App lifecycle, init, event handlers
│   └── version.h              # NIEUW (v1.1): FW_VERSION define
├── components/
│   ├── mesh_link/             # WiFi mesh (ESP-IDF) + NIEUW: parent/layer tracking
│   ├── mqtt_link/             # MQTT client (root only) + NIEUW: status timer
│   ├── router/                # Command routing + NIEUW: STATUS/TIME handlers
│   ├── parser/                # JSON parsing + NIEUW: max_runtime_sec, safe_state
│   ├── config_store/          # NVS config
│   ├── time_sync/             # NIEUW (v1.1): MQTT tijd synchronisatie component
│   ├── relay_ctrl/            # Relay driver
│   ├── pwm_ctrl/              # PWM driver (LED dimming)
│   └── input_ctrl/            # Input driver (drukknoppen, PIR)
```

## Status & Roadmap

- **Huidige Versie**: v1.0.0 (stable, in productie)
- **Planning**: v1.1.0 compleet gedocumenteerd (4 fases)
- **Implementatie**: Nog niet gestart

**v1.1.0 Nieuwe Features:**
- Software versie tracking + broadcasting
- MQTT tijd synchronisatie (root → nodes)
- Periodieke status broadcasts (30s interval)
- Offline detectie (mesh events + 90s timeout)
- Watchdog & fallback systeem (safety)
- Network topology monitoring (parent/layer tracking)

## Ontwerpprincipes (Kritiek!)

### 1. MQTT als Contract
- Alle communicatie tussen componenten via MQTT
- ESP32 ↔ MQTT ↔ Node-RED ↔ Home Assistant
- Single source of truth: MQTT retained messages
- **Zie:** 03_MQTT_API_Specificatie.md

### 2. Safety First
- Watchdog voor kritische actuatoren (ventiel → auto-OFF na timeout)
- Safe state bij mesh disconnect (bijv. ventiel sluit)
- Lokale beveiliging op ESP32 (niet afhankelijk van MQTT)

### 3. Intelligence at Edge
- **Node-RED**: Complexe logica (scheduling, multi-sensor fusion, prioriteiten)
- **ESP32**: Basis uitvoering (relay ON/OFF, PWM dimming) + safety
- **Home Assistant**: UI, visualisatie, configuratie

### 4. Loose Coupling
- ESP32 nodes weten niets van HA/Node-RED (alleen MQTT)
- Node-RED weet niets van mesh topologie
- Componenten uitwisselbaar zolang MQTT API gevolgd wordt

## Implementatie Volgorde (v1.1)

### Fase 1: Basis Infrastructure
- [ ] `src/version.h` - FW_VERSION define
- [ ] `components/time_sync/` - Nieuwe component aanmaken
- [ ] Mesh API uitbreidingen: `mesh_get_parent_name()`, `mesh_get_layer()`
- [ ] Test: tijd sync root → nodes

### Fase 2: Status System
- [ ] Router: handlers voor `ML_KIND_STATUS`, `router_collect_status_data()`
- [ ] MQTT_link: status broadcast timer (30s), `mqtt_link_trigger_status_broadcast()`
- [ ] MQTT topics: `Devices/<dev>/Status`, `Mesh/Network/Status`
- [ ] Test: periodieke status broadcasts, topology change → immediate update

### Fase 3: Watchdog & Safety
- [ ] Watchdog systeem (max_runtime_sec, safe_state)
- [ ] Parser uitbreiding (parse watchdog velden)
- [ ] Mesh disconnect handlers → safe state
- [ ] Test: watchdog timeout, disconnect → safe state

### Fase 4: Monitoring & Polish
- [ ] MQTT offline detectie watchdog (90s)
- [ ] Network status tabel (layer + alfabetisch sortering)
- [ ] Version mismatch warnings (root)
- [ ] Test: offline detectie, root reboot, HA dashboard

**Zie 01_ESP32_Firmware.md voor gedetailleerde implementatie per fase.**

## Development Commands

```bash
# Navigate to codebase
cd C:\Users\tomva\PlatformIO\Excellence

# Build
pio run

# Upload naar ESP32
pio run -t upload

# Monitor serial output
pio device monitor

# Clean build
pio run -t clean

# Check deprecated API
pio run 2>&1 | grep -i "deprecated"
```

## Context voor Claude Code Sessies

### Optimale Start Prompt
```
cd C:\Users\tomva\PlatformIO\Excellence

"Start implementatie Excellence v1.1, fase X.
Lees eerst 01_ESP32_Firmware.md sectie 'Fase X'
en begin met [specifieke component]."
```

### Voor Architectuur Vragen
```
cd "C:\Users\tomva\CC AI Map\ProjectExcellence"

"Lees 00_Architectuur_Overzicht.md.
[vraag over data flow / componenten / design keuze]"
```

### Voor MQTT/API Details
```
"Volgens 03_MQTT_API_Specificatie.md moet [topic/payload] formaat zijn.
Hoe implementeer ik [specific feature]?"
```

### Bij Debugging
```
"Volgens 01_ESP32_Firmware.md sectie X moet [feature] werken als [verwacht].
Huidige gedrag: [observatie].
Help debuggen."
```

### Voor Node-RED Flows
```
"Lees 02_NodeRED_HomeAssistant.md use case [ventiel/verlichting].
Implementeer flow voor [specifieke situatie]."
```

## Kritieke Bestanden voor v1.1

| Bestand | Wijzigingen | Prioriteit |
|---------|-------------|------------|
| `src/version.h` | **NIEUW**: FW_VERSION define | Fase 1 |
| `components/time_sync/*` | **NIEUW**: Complete component | Fase 1 |
| `components/mesh_link/backends/backend_espmesh.c` | Parent/layer API, topology events | Fase 1 |
| `components/mesh_link/include/mesh_link.h` | Nieuwe ML_KIND enums, parent/layer functies | Fase 1 |
| `components/router/router.c` | STATUS/TIME handlers, watchdog, retry | Fase 2-3 |
| `components/mqtt_link/mqtt_link.c` | Status timer, offline watchdog, network tabel | Fase 2-4 |
| `components/parser/parser.c` | Parse max_runtime_sec, safe_state | Fase 3 |
| `src/main.c` | Init time_sync, topology event handlers | Alle fases |

## ESP-IDF API Verificatie

**Huidige ESP-IDF versie:** 5.4.0 (zie sdkconfig.esp32dev)

**Bij twijfel over API:**
1. Check [ESP-IDF v5.4 Mesh Docs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp-wifi-mesh.html)
2. Zoek in header: `~/.platformio/packages/framework-espidf/components/esp_wifi/include/esp_mesh.h`
3. **Zie 01_ESP32_Firmware.md sectie "ESP-IDF API Verificatie & Best Practices"**

**Belangrijke Timing Requirements:**
- `esp_mesh_get_parent_bssid()` - Alleen na `MESH_EVENT_PARENT_CONNECTED`
- `esp_mesh_get_layer()` - Alleen na `MESH_EVENT_PARENT_CONNECTED`
- **Beste praktijk**: Cache event data, vermijd polling

## Testing Checklist (v1.1)

**Unit Tests:**
- [ ] Time sync offset berekening
- [ ] Watchdog trigger na max_runtime_sec
- [ ] Safe state execution per IO type

**Integration Tests:**
- [ ] 1 root + 2 nodes: tijd sync werkt
- [ ] Status broadcast elke 30s ontvangen
- [ ] Topology change → immediate status update
- [ ] Node disconnect → offline binnen 90s
- [ ] Watchdog disconnect → safe state binnen 5s

**Stress Tests:**
- [ ] 10+ nodes, mesh reorganisatie
- [ ] Root reboot → nodes reconnecten
- [ ] MQTT disconnect/reconnect → geen data verlies

## Quick Reference

**MQTT Topics:**
- Commands: `Devices/<dev>/Cmd/Set`
- Config: `Devices/<dev>/Config/Set`
- State: `Devices/<dev>/State` (retained)
- Status: `Devices/<dev>/Status` (retained, 30s interval)
- Network: `Mesh/Network/Status` (retained)
- Time: `Mesh/Time`

**Status Fields:**
- `version` - Firmware versie (semantic)
- `timestamp` - Unix tijd (na sync)
- `parent` - Parent device naam
- `layer` - Mesh layer (0=root)
- `uptime_s` - Uptime sinds boot

**Watchdog Fields:**
- `max_runtime_sec` - Auto-OFF timeout
- `safe_state` - State bij disconnect/timeout ("ON"/"OFF")

## Toekomstige Uitbreidingen (Post v1.1)

1. **OTA Updates** - Over-the-air firmware updates via MQTT
2. **Energy Monitoring** - Power consumption tracking per actuator
3. **Config Backup** - Periodieke backup van config naar MQTT
4. **Event History** - Lokale opslag laatste 100 events (SPIFFS)
5. **Multi-network** - Meerdere mesh netwerken (binnen/buiten gescheiden)

---

**Laatste Update:** 2024-12-14
**Planning Status:** Compleet, ready for implementation
**Documentatie Versie:** 1.1
