# Project Excellence - ESP32 WiFi Mesh System

## Project Informatie
- **Type**: ESP-IDF firmware voor ESP32 boards
- **Platform**: PlatformIO
- **Taal**: C
- **Framework**: ESP-IDF
- **Codebase**: `C:\Users\tomva\PlatformIO\Excellence`

## Documentatie Structuur

Deze directory bevat de complete planning documentatie voor Project Excellence:

### Plan Documenten
- **00_Architectuur_Overzicht.md** - High-level systeem design, componenten, data flows
- **01_ESP32_Firmware.md** - Firmware specificatie (mesh, MQTT, watchdog, safety)
- **02_NodeRED_HomeAssistant.md** - Automation logica, use cases, flows
- **03_MQTT_API_Specificatie.md** - MQTT contract tussen componenten

## Code Structuur
```
Excellence/
├── src/
│   ├── main.c                 # App lifecycle, init, event handlers
│   └── version.h              # NIEUW (v1.1): FW_VERSION
├── components/
│   ├── mesh_link/             # WiFi mesh (ESP-IDF)
│   ├── mqtt_link/             # MQTT client (root only)
│   ├── router/                # Command routing
│   ├── parser/                # JSON parsing
│   ├── config_store/          # NVS config
│   ├── time_sync/             # NIEUW (v1.1): Tijd synchronisatie
│   ├── relay_ctrl/            # Relay driver
│   ├── pwm_ctrl/              # PWM driver
│   └── input_ctrl/            # Input driver
```

## Huidige Status
- **Firmware Versie**: v1.0.0 (stable, in productie)
- **Gepland**: v1.1.0 (status broadcasts, watchdog, tijd sync)
- **Planning**: Compleet (4 documenten)
- **Implementatie**: Nog niet gestart

## Implementatie Workflow

### Bij Start van Code Implementatie:
1. `cd C:\Users\tomva\PlatformIO\Excellence`
2. Lees **01_ESP32_Firmware.md** sectie relevante fase
3. Volg implementatie volgorde (Fase 1-4)
4. Test volgens checklist

### Bij Architectuur Vragen:
- **Systeem overzicht**: Zie 00_Architectuur_Overzicht.md
- **MQTT API**: Zie 03_MQTT_API_Specificatie.md
- **Use cases**: Zie 02_NodeRED_HomeAssistant.md
- **Firmware details**: Zie 01_ESP32_Firmware.md

### Bij Nieuwe Features:
1. Check of al gedocumenteerd in plannen
2. Zo niet: bespreek architectuur impact eerst
3. Update relevante plan documenten
4. Volg implementatie workflow

## Belangrijke Ontwerpprincipes

### 1. MQTT als Contract
- Alle communicatie tussen componenten via MQTT
- Single source of truth: retained messages
- Zie **03_MQTT_API_Specificatie.md** voor details

### 2. Safety First
- Watchdog voor kritische actuatoren (ventiel, pomp)
- Safe state bij disconnect/timeout
- Lokale beveiliging op ESP32

### 3. Intelligence at Edge
- **Complexe logica**: Node-RED (scheduling, multi-sensor)
- **Basis uitvoering**: ESP32 (relay ON/OFF, PWM)
- **Safety critical**: ESP32 lokaal (watchdog)

### 4. Loose Coupling
- ESP32 weet niets van HA/Node-RED
- Node-RED weet niets van mesh topologie
- Contract: MQTT API

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
```

## Implementatie Prioriteit (v1.1)

### Fase 1: Basis Infrastructure (Week 1)
- [ ] `src/version.h` - FW_VERSION define
- [ ] `components/time_sync/` - Nieuwe component
- [ ] Mesh API uitbreidingen (parent/layer)

### Fase 2: Status System (Week 2)
- [ ] Router: STATUS handlers
- [ ] MQTT_link: Status broadcast timer (30s)
- [ ] Network status tabel

### Fase 3: Watchdog & Safety (Week 3)
- [ ] Watchdog systeem implementatie
- [ ] Parser uitbreiding (max_runtime_sec, safe_state)
- [ ] Disconnect handlers

### Fase 4: Monitoring & Polish (Week 4)
- [ ] Offline detectie watchdog
- [ ] Version mismatch warnings
- [ ] Testing volgens checklist

## Kritieke Bestanden (v1.1 wijzigingen)

| Bestand | Wijzigingen |
|---------|-------------|
| `src/main.c` | Init time_sync, topology event handlers |
| `src/version.h` | **NIEUW**: FW_VERSION define |
| `components/time_sync/*` | **NIEUW**: Complete component |
| `components/mesh_link/backends/backend_espmesh.c` | Parent/layer API, topology events |
| `components/router/router.c` | STATUS/TIME handlers, watchdog, retry |
| `components/mqtt_link/mqtt_link.c` | Status timer, offline watchdog, network tabel |
| `components/parser/parser.c` | Parse max_runtime_sec, safe_state |

## Context voor Claude Code Sessies

### Optimale Start Prompt:
```
cd C:\Users\tomva\PlatformIO\Excellence

"Start implementatie van Excellence v1.1, fase X.
Lees eerst 01_ESP32_Firmware.md sectie 'Fase X'
en begin met [specifieke component]"
```

### Bij Problemen:
```
"Volgens 01_ESP32_Firmware.md moet [feature] werken als [verwacht gedrag].
Huidige gedrag: [observatie].
Help debuggen."
```

### Bij Onduidelijkheid:
- Verwijs naar relevante sectie in plan documenten
- Alle architectuur beslissingen zijn gedocumenteerd
- MQTT API is het contract - zie 03_MQTT_API_Specificatie.md

## Toekomstige Uitbreidingen
- OTA updates via MQTT
- Energy monitoring per actuator
- Multi-network support
- Config backup naar MQTT
- Event history (lokaal SPIFFS)

---

**Laatste Update**: 2024-11-30
**Planning Status**: Compleet, ready for implementation
**Documentatie Versie**: 1.0
