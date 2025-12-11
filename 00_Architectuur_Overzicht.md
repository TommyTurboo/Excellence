# Project Excellence - Architectuur Overzicht

## Inleiding
Project Excellence is een gedistribueerd home automation systeem gebaseerd op ESP32 WiFi Mesh nodes, MQTT communicatie, en Home Assistant/Node-RED voor intelligente automatisering.

## Systeemcomponenten

```
┌─────────────────┐
│  Home Assistant │ ← UI, configuratie, visualisatie
└────────┬────────┘
         │
┌────────▼────────┐
│    Node-RED     │ ← Automation logica, scheduling, flows
└────────┬────────┘
         │
┌────────▼────────┐
│  MQTT Broker    │ ← Centrale communicatie hub
└────────┬────────┘
         │
┌────────▼────────┐
│   Root Node     │ ← ESP32 mesh root, WiFi + MQTT client
│   (ESP32)       │
└────────┬────────┘
         │ ESP-IDF WiFi Mesh
         │
    ┌────┴────┬────────┬────────┐
    ▼         ▼        ▼        ▼
┌───────┐ ┌───────┐ ┌───────┐ ┌───────┐
│ Node1 │ │ Node2 │ │ Node3 │ │ Node N│ ← Child nodes (relays, sensors, PWM)
└───────┘ └───────┘ └───────┘ └───────┘
```

## Verantwoordelijkheden per Component

### Home Assistant
**Rol:** Gebruikersinterface en visualisatie
- Dashboard met entities (schakelaars, sliders, sensors)
- Configuratie UI voor timers, schema's, limieten
- **State monitoring** (rechtstreeks van MQTT, niet via Node-RED)
- Historische data en grafieken

**Technologie:** YAML configuratie, Lovelace UI

### Node-RED
**Rol:** Automation engine en logica
- Complexe automatiseringen (tijd-gebaseerd, event-driven)
- Multi-sensor fusion (PIR + drukknoppen + schema's)
- Priority handling (drukknop > PIR sensor)
- Warning sequences (3x dimmen voor uitschakeling)
- Publiceert commando's naar MQTT

**Technologie:** Flow-based programming, JavaScript functions

### MQTT Broker
**Rol:** Centrale communicatie laag
- Publish/Subscribe messaging
- Retained messages voor actuele states
- QoS levels voor betrouwbaarheid
- **Single source of truth** voor device states

**Technologie:** Mosquitto of vergelijkbaar

### ESP32 Root Node
**Rol:** Gateway tussen MQTT en Mesh netwerk
- MQTT client (subscribe commands, publish states)
- WiFi mesh root node
- Command routing naar child nodes
- Status aggregatie van alle nodes
- Tijd distributie naar mesh netwerk
- Network topology monitoring

**Technologie:** ESP-IDF, C

### ESP32 Child Nodes
**Rol:** Actuatoren en sensoren
- Relay besturing (ON/OFF)
- PWM dimming (LED verlichting)
- Input monitoring (drukknoppen, PIR sensors)
- **Lokale safety** (watchdog, fallbacks)
- State reporting via mesh → root → MQTT

**Technologie:** ESP-IDF, C

## Data Flow Scenario's

### Scenario 1: Manuele Schakelaar (HA → ESP32)
```
1. User: Toggle switch in HA
2. HA → Node-RED: State change trigger
3. Node-RED → MQTT: Publish command
   Topic: Devices/ESP32_TUIN/Cmd/Set
   Payload: {"target_dev":"ESP32_TUIN", "io_kind":"RELAY", "io_id":0, "action":"ON", "max_runtime_sec":1800}
4. MQTT → Root Node: Command received
5. Root → Child Node (ESP32_TUIN): Mesh message
6. ESP32_TUIN: Execute relay ON + start watchdog timer
7. ESP32_TUIN → Root: State update (mesh)
8. Root → MQTT: Publish state
   Topic: Devices/ESP32_TUIN/State
   Payload: {"dev":"ESP32_TUIN", "io_kind":"RELAY", "io_id":0, "state":"ON", "timestamp":1732873200}
9. HA: Update UI (relay shows ON)
```

### Scenario 2: Automatische PIR Detectie (ESP32 → HA)
```
1. ESP32_BINNEN: PIR sensor detecteert beweging
2. ESP32_BINNEN → Root: Event via mesh
   {"dev":"ESP32_BINNEN", "io_kind":"INPUT", "io_id":2, "state":"HIGH"}
3. Root → MQTT: Publish event
   Topic: Devices/ESP32_BINNEN/State
4. MQTT → Node-RED: Trigger ontvangen
5. Node-RED: Automation flow
   - Check tijd (is het na zonsondergang?)
   - Check priority (geen drukknop override actief?)
   - Besluit: Verlichting AAN voor 5 minuten
6. Node-RED → MQTT: Command
   Topic: Devices/ESP32_BINNEN/Cmd/Set
   Payload: {"target_dev":"ESP32_BINNEN", "io_kind":"PWM", "io_id":0, "action":"SET", "brightness_pct":80, "max_runtime_sec":300}
7. MQTT → Root → ESP32_BINNEN: Command execution
8. ESP32_BINNEN: PWM ON (80%) + auto-OFF timer (5 min)
```

### Scenario 3: Safety Fallback (Watchdog Trigger)
```
1. ESP32_TUIN: Ventiel relay AAN (max_runtime_sec=1800)
2. ESP32_TUIN: Watchdog timer gestart (30 minuten)
3. [Root node crash / WiFi disconnect / MQTT offline]
4. ESP32_TUIN: Geen mesh connectie meer
5. ESP32_TUIN: Watchdog triggered → SAFE STATE
6. ESP32_TUIN: Relay OFF (ventiel gesloten)
7. [Connectie hersteld]
8. ESP32_TUIN → Root → MQTT: State update
   Topic: Devices/ESP32_TUIN/State
   Payload: {"dev":"ESP32_TUIN", "state":"OFF", "reason":"watchdog_timeout"}
9. Node-RED/HA: Alert naar gebruiker
```

## Ontwerpprincipes

### 1. Single Source of Truth
- **MQTT retained messages** bevatten actuele device states
- Home Assistant leest states **rechtstreeks van MQTT**
- Geen state duplication in Node-RED of HA

### 2. Intelligence at the Edge
- **Complexe logica**: Node-RED (tijd, schema's, multi-sensor)
- **Basis uitvoering**: ESP32 (relay ON/OFF, PWM, timers)
- **Safety critical**: ESP32 lokaal (watchdog, fallback)

### 3. Loose Coupling
- Componenten communiceren **alleen via MQTT**
- ESP32 nodes weten niets van HA of Node-RED
- Node-RED weet niets van mesh topologie
- **Contract**: MQTT API specificatie (zie document 03)

### 4. Graceful Degradation
- ESP32 nodes blijven functioneren zonder MQTT (lokale safety)
- Node-RED crash → laatste commando blijft actief (tot watchdog)
- HA offline → automation blijft draaien in Node-RED

### 5. Fail-Safe Defaults
- Kritische actuatoren (ventiel): **safe state bij disconnect**
- Niet-kritische actuatoren (verlichting): **geen auto-OFF**
- Configureerbaar via MQTT

## Schaalbaarheid

### Mesh Netwerk
- **Root node**: 1x (verbonden met WiFi + MQTT)
- **Child nodes**: Max ~50 nodes (ESP-IDF mesh limiet)
- **Layers**: Max 6 hops (configureerbaar)

### MQTT Traffic
- **Status broadcasts**: Elke 30s per node (50 nodes = ~1.7 msg/sec)
- **Commands**: On-demand (variabel)
- **Events**: Sensor triggers (variabel, burst mogelijk)

### Node-RED Flows
- Modulair per use case (ventiel, verlichting binnen, buiten, etc.)
- Schaalbaar naar honderden flows

## Security (Toekomstige Uitbreiding)
- MQTT: TLS + username/password
- Mesh: Encrypted (ESP-IDF built-in)
- Home Assistant: Reverse proxy + auth

## Betrouwbaarheid

### ESP32 Nodes
- **Watchdog timers** voor kritische actuatoren
- **Mesh auto-healing** bij node failures
- **Offline detectie** via status broadcasts (90s timeout)

### MQTT
- **QoS 1** voor commands (at least once delivery)
- **Retained messages** voor states (persistence)
- **Last Will Testament** voor disconnect detectie

### Node-RED
- Stateless flows waar mogelijk
- Persistent context voor kritische state

## Monitoring & Diagnostics

### Netwerk Health
- **Topic**: `Mesh/Network/Status`
- Bevat: alle nodes, layers, parents, versies, online status
- Update: bij elke topology change

### Per-Device Status
- **Topic**: `Devices/<device_name>/Status`
- Bevat: versie, timestamp, parent, layer, uptime
- Interval: 30s

### Logging
- ESP32: Serial UART (development) + MQTT events (production)
- Node-RED: Debug nodes + persistent logs
- Home Assistant: Logbook + History

## Volgende Documenten

1. **01_ESP32_Firmware.md**: Gedetailleerd firmware design (mesh, commands, safety)
2. **02_NodeRED_HomeAssistant.md**: Use cases, flows, UI configuratie
3. **03_MQTT_API_Specificatie.md**: Topics, payloads, message formats (contract!)
