# Project Excellence - MQTT API Specificatie

## Inleiding
Dit document definieert het **contract** tussen ESP32 firmware en Node-RED/Home Assistant via MQTT. Alle componenten moeten zich aan deze specificatie houden.

**Versie:** 1.1.0
**Datum:** 2024-11-30

## MQTT Broker Configuratie

**Aanbevolen:** Mosquitto
**Poort:** 1883 (onversleuteld) of 8883 (TLS)
**QoS Levels:**
- Commands: QoS 1 (at least once)
- States: QoS 1 + Retained
- Events: QoS 0 (best effort)

**Retained Messages:**
- Device states (altijd laatste bekende state)
- Network status tabel
- **Niet** voor commands (eenmalige acties)

## Topic Structuur

### Hiërarchie
```
Devices/<device_name>/
├── Cmd/Set              # Commands naar device (Node-RED → ESP32)
├── Config/Set           # Configuratie updates (Node-RED → ESP32)
├── State                # Actuele device state (ESP32 → HA, retained)
└── Status               # Health/monitoring info (ESP32 → HA, retained)

Mesh/
├── Time                 # Tijd synchronisatie (MQTT → Root → Nodes)
├── Network/Status       # Complete netwerk tabel (Root → HA, retained)
└── RouteTable           # Diagnostische routing info (Root → HA)
```

### Naming Conventions
- **Device names**: `ESP32_<LOCATION>` (bijv. ESP32_TUIN, ESP32_BINNEN)
- **Topic separators**: `/` (forward slash)
- **Case**: Hoofdletters voor topic levels, lowercase voor JSON keys

## Message Formats

### 1. Command Messages

**Topic:** `Devices/<device_name>/Cmd/Set`
**Direction:** Node-RED/HA → ESP32
**QoS:** 1
**Retained:** false

#### Relay Command
```json
{
  "target_dev": "ESP32_TUIN",
  "io_kind": "RELAY",
  "io_id": 0,
  "action": "ON",
  "max_runtime_sec": 1800,
  "safe_state": "OFF",
  "corr_id": "uuid-1234"
}
```

**Velden:**
| Veld | Type | Verplicht | Beschrijving |
|------|------|-----------|--------------|
| `target_dev` | string | Ja | Device naam (ESP32_XXX) |
| `io_kind` | enum | Ja | `"RELAY"`, `"PWM"`, `"INPUT"` |
| `io_id` | int | Ja | IO pin index (0-based) |
| `action` | enum | Ja | `"ON"`, `"OFF"`, `"TOGGLE"` voor relay |
| `max_runtime_sec` | int | Nee | Auto-OFF na X seconden (watchdog), 0 = unlimited |
| `safe_state` | enum | Nee | `"ON"` of `"OFF"` bij disconnect/timeout |
| `corr_id` | string | Nee | Correlatie ID voor request/response matching |

**Acties:**
- `ON`: Relay/PWM inschakelen
- `OFF`: Relay/PWM uitschakelen
- `TOGGLE`: Wissel staat (alleen relay)

---

#### PWM Command
```json
{
  "target_dev": "ESP32_BINNEN",
  "io_kind": "PWM",
  "io_id": 0,
  "action": "SET",
  "brightness_pct": 80,
  "ramp_ms": 2000,
  "max_runtime_sec": 300,
  "safe_state": "OFF"
}
```

**Extra velden (PWM):**
| Veld | Type | Verplicht | Beschrijving |
|------|------|-----------|--------------|
| `brightness_pct` | int | Ja (voor SET) | Helderheid 0-100% |
| `ramp_ms` | int | Nee | Fade duur in milliseconden (smooth transition) |

**Acties (PWM):**
- `SET`: Zet helderheid naar `brightness_pct`
- `FADE`: Fade naar `brightness_pct` over `ramp_ms` duur
- `OFF`: Fade naar 0% (uit)

---

#### Input Query
```json
{
  "target_dev": "ESP32_BINNEN",
  "io_kind": "INPUT",
  "io_id": 2,
  "action": "READ"
}
```

**Acties (INPUT):**
- `READ`: Lees huidige input state (response in State topic)

---

### 2. Configuration Messages

**Topic:** `Devices/<device_name>/Config/Set`
**Direction:** Node-RED/HA → ESP32
**QoS:** 1
**Retained:** false

```json
{
  "dev_name": "ESP32_TUIN",
  "relay": {
    "count": 2,
    "gpio": [12, 14],
    "active_low_mask": 0,
    "open_drain_mask": 0,
    "autoff_sec": [0, 0],
    "safe_state": ["OFF", "OFF"],
    "watchdog_enabled": [true, false]
  },
  "pwm": {
    "count": 1,
    "gpio": [15],
    "inverted_mask": 0,
    "freq_hz": 5000,
    "safe_state": ["OFF"],
    "watchdog_enabled": [false]
  },
  "input": {
    "count": 3,
    "gpio": [16, 17, 18],
    "pullup_mask": 7,
    "pulldown_mask": 0,
    "inverted_mask": 0,
    "debounce_ms": [50, 50, 100]
  }
}
```

**Response:**
ESP32 publiceert naar `State` topic met `config_applied: true/false`

---

### 3. State Messages

**Topic:** `Devices/<device_name>/State`
**Direction:** ESP32 → Node-RED/HA
**QoS:** 1
**Retained:** true

```json
{
  "dev": "ESP32_TUIN",
  "timestamp": 1732873200,
  "relay_0": "ON",
  "relay_1": "OFF",
  "pwm_0": 80,
  "input_0": "HIGH",
  "input_1": "LOW",
  "input_2": "HIGH",
  "watchdog_active": true,
  "watchdog_remaining_sec": 1234,
  "reason": "command_executed"
}
```

**Velden:**
| Veld | Type | Beschrijving |
|------|------|--------------|
| `dev` | string | Device naam |
| `timestamp` | int | Unix timestamp (0 als tijd niet gesynchroniseerd) |
| `relay_{id}` | enum | `"ON"` of `"OFF"` per relay |
| `pwm_{id}` | int | Brightness 0-100% per PWM channel |
| `input_{id}` | enum | `"HIGH"` of `"LOW"` per input |
| `watchdog_active` | bool | Optioneel: is er een actieve watchdog? |
| `watchdog_remaining_sec` | int | Optioneel: resterende tijd voor watchdog timeout |
| `reason` | enum | Reden voor state update (zie hieronder) |

**Reason codes:**
- `command_executed`: Normaal command uitgevoerd
- `watchdog_timeout`: Watchdog triggered (auto-OFF)
- `mesh_disconnect`: Mesh connectie verloren (safe state)
- `boot`: Device is opgestart
- `config_applied`: Configuratie succesvol toegepast
- `periodic`: Periodieke state update (heartbeat)

---

### 4. Status Messages (Monitoring)

**Topic:** `Devices/<device_name>/Status`
**Direction:** ESP32 → Node-RED/HA
**QoS:** 1
**Retained:** true
**Interval:** 30 seconden

```json
{
  "dev": "ESP32_TUIN",
  "version": "1.1.0",
  "timestamp": 1732873200,
  "parent": "ESP32_ROOT",
  "layer": 1,
  "uptime_s": 3600,
  "free_heap": 81920,
  "status": "online",
  "reason": "periodic"
}
```

**Velden:**
| Veld | Type | Beschrijving |
|------|------|--------------|
| `dev` | string | Device naam |
| `version` | string | Firmware versie (semantic versioning) |
| `timestamp` | int | Unix timestamp |
| `parent` | string | Parent device naam (null voor root) |
| `layer` | int | Mesh layer (0=root, 1=first hop, etc.) |
| `uptime_s` | int | Uptime in seconden sinds boot |
| `free_heap` | int | Vrij RAM in bytes (optioneel, diagnostisch) |
| `status` | enum | `"online"` of `"offline"` |
| `reason` | enum | Reden voor status update |

**Status Reason Codes:**
- `periodic`: Normale 30s heartbeat
- `layer_change`: Mesh layer is veranderd
- `parent_change`: Parent node is veranderd
- `reconnect`: Na disconnect weer verbonden
- `boot`: Device is opgestart

**Offline Status:**
Bij timeout (>90s geen status ontvangen), publiceert **root node**:
```json
{
  "dev": "ESP32_TUIN",
  "status": "offline",
  "reason": "timeout",
  "last_seen": 1732873000
}
```

---

### 5. Network Status Table

**Topic:** `Mesh/Network/Status`
**Direction:** Root ESP32 → HA
**QoS:** 1
**Retained:** true
**Update:** Bij elke status change

```json
{
  "timestamp": 1732873200,
  "root": "ESP32_ROOT",
  "total_nodes": 5,
  "online_nodes": 4,
  "nodes": [
    {
      "dev": "ESP32_ROOT",
      "version": "1.1.0",
      "layer": 0,
      "parent": null,
      "status": "online",
      "last_seen": 1732873200,
      "uptime_s": 86400
    },
    {
      "dev": "ESP32_BINNEN",
      "version": "1.1.0",
      "layer": 1,
      "parent": "ESP32_ROOT",
      "status": "online",
      "last_seen": 1732873195,
      "uptime_s": 3600
    },
    {
      "dev": "ESP32_TUIN",
      "version": "1.1.0",
      "layer": 1,
      "parent": "ESP32_ROOT",
      "status": "offline",
      "last_seen": 1732872900,
      "uptime_s": 0
    }
  ]
}
```

**Sortering:**
1. Primair: `layer` (ascending, root eerst)
2. Secundair: `dev` (alfabetisch)

**Use case:** HA dashboard tabel voor netwerk overzicht

---

### 6. Time Sync Messages

**Topic:** `Mesh/Time`
**Direction:** MQTT Broker → Root ESP32
**QoS:** 1
**Retained:** false
**Interval:** Optioneel, 1x per minuut

```json
{
  "unix_time": 1732873200
}
```

**Flow:**
1. External service (bijv. Home Assistant automation) publiceert actuele tijd
2. Root ESP32 ontvangt en update lokale tijd
3. Child nodes vragen tijd op via mesh `TIME_REQUEST`
4. Root antwoordt met `TIME_RESPONSE` (mesh message, niet MQTT)

**Alternative:** Root kan ook NTP gebruiken (als WiFi beschikbaar), MQTT als fallback.

---

### 7. Route Table (Diagnostisch)

**Topic:** `Mesh/RouteTable`
**Direction:** Root ESP32 → HA
**QoS:** 0
**Retained:** false
**Interval:** Op verzoek of bij topology change

```json
{
  "timestamp": 1732873200,
  "routes": [
    {
      "mac": "AA:BB:CC:DD:EE:FF",
      "dev": "ESP32_BINNEN",
      "parent_mac": "00:11:22:33:44:55",
      "parent_dev": "ESP32_ROOT",
      "layer": 1,
      "rssi": -45,
      "hop_count": 1
    }
  ]
}
```

**Use case:** Troubleshooting, diagnostiek, netwerk visualisatie

---

## Message Validation

### ESP32 (Inkomend)
- **Required velden**: Controleer aanwezigheid van `target_dev`, `io_kind`, `io_id`, `action`
- **Enum waarden**: Valideer tegen toegestane waarden
- **Range checks**: `io_id` binnen configured count, `brightness_pct` tussen 0-100
- **Parse errors**: Publish naar `State` topic met `reason: "parse_error"` + details

**Error Response:**
```json
{
  "dev": "ESP32_TUIN",
  "error": "invalid_io_id",
  "detail": "io_id 5 out of range (max 1)",
  "received_message": "{...}"
}
```

### Node-RED (Inkomend States)
- **Schema validation**: Controleer expected velden
- **Type checks**: Timestamp is int, brightness is int, etc.
- **Timestamp staleness**: Waarschuw als timestamp >5 min oud (tijd sync probleem)

---

## QoS & Reliability

### Command Delivery
- **QoS 1**: At least once delivery
- **Retry**: Node-RED retry bij geen response binnen 5s (optioneel)
- **Correlation ID**: Gebruik `corr_id` voor request/response matching

### State Persistence
- **Retained flag**: Altijd enabled voor `State` en `Status` topics
- **Boot behavior**: ESP32 publiceert full state direct na boot
- **HA startup**: Leest laatste retained states van MQTT

### Offline Queue
- **ESP32**: Heeft interne offline queue (16 items, 30s TTL)
- **MQTT Broker**: Persistent sessions (clean_session=false)

---

## Security (Toekomst)

### Authentication
- MQTT username/password (basis)
- TLS encryption (1883 → 8883)

### Authorization
- ACL per device (topic restrictions)
- Read-only voor State topics
- Write-only voor Cmd topics

### Encryption
- Mesh: ESP-IDF built-in encryption
- MQTT: TLS 1.2+

---

## Testing & Debugging

### MQTT Client Tools
- **mosquitto_sub**: Subscribe naar topics
  ```bash
  mosquitto_sub -h localhost -t "Devices/+/State" -v
  ```

- **mosquitto_pub**: Publish test commands
  ```bash
  mosquitto_pub -h localhost -t "Devices/ESP32_TUIN/Cmd/Set" \
    -m '{"target_dev":"ESP32_TUIN","io_kind":"RELAY","io_id":0,"action":"ON"}'
  ```

### MQTT Explorer (GUI)
- Visuele topic browser
- Message inspector
- Publish/subscribe interface

### Node-RED Debug Nodes
- `mqtt in` → `debug` voor alle inkomende messages
- Log payloads naar Node-RED debug panel

---

## Versioning & Compatibility

### API Versie
- Huidige versie: **1.1.0**
- Breaking changes → major version bump
- Nieuwe velden (optioneel) → minor version bump

### Backwards Compatibility
- Oude firmware (v1.0) moet blijven werken met nieuwe MQTT API
- Nieuwe velden negeren indien niet ondersteund
- Deprecated velden blijven 1 major version ondersteund

### Version Negotiation
- ESP32 publiceert versie in `Status` message
- Root logt WARNING bij major version mismatch
- HA dashboard toont versie per node

---

## Voorbeelden

### Voorbeeld 1: Ventiel Inschakelen met Watchdog
**Node-RED → ESP32:**
```json
Topic: Devices/ESP32_TUIN/Cmd/Set
Payload:
{
  "target_dev": "ESP32_TUIN",
  "io_kind": "RELAY",
  "io_id": 0,
  "action": "ON",
  "max_runtime_sec": 1800,
  "safe_state": "OFF"
}
```

**ESP32 → HA (State):**
```json
Topic: Devices/ESP32_TUIN/State
Payload:
{
  "dev": "ESP32_TUIN",
  "timestamp": 1732873200,
  "relay_0": "ON",
  "watchdog_active": true,
  "watchdog_remaining_sec": 1800,
  "reason": "command_executed"
}
```

**30 minuten later → Watchdog timeout:**
```json
Topic: Devices/ESP32_TUIN/State
Payload:
{
  "dev": "ESP32_TUIN",
  "timestamp": 1732875000,
  "relay_0": "OFF",
  "watchdog_active": false,
  "reason": "watchdog_timeout"
}
```

---

### Voorbeeld 2: Verlichting Dimmen met Fade
**Node-RED → ESP32:**
```json
Topic: Devices/ESP32_BINNEN/Cmd/Set
Payload:
{
  "target_dev": "ESP32_BINNEN",
  "io_kind": "PWM",
  "io_id": 0,
  "action": "FADE",
  "brightness_pct": 50,
  "ramp_ms": 3000
}
```

**ESP32 → HA (State, tijdens fade):**
```json
Topic: Devices/ESP32_BINNEN/State
Payload:
{
  "dev": "ESP32_BINNEN",
  "timestamp": 1732873200,
  "pwm_0": 25,
  "reason": "fading"
}
```

**ESP32 → HA (State, na fade):**
```json
{
  "dev": "ESP32_BINNEN",
  "timestamp": 1732873203,
  "pwm_0": 50,
  "reason": "command_executed"
}
```

---

### Voorbeeld 3: Configuration Update
**Node-RED → ESP32:**
```json
Topic: Devices/ESP32_GARAGE/Config/Set
Payload:
{
  "dev_name": "ESP32_GARAGE",
  "relay": {
    "count": 1,
    "gpio": [12],
    "safe_state": ["OFF"],
    "watchdog_enabled": [true]
  },
  "input": {
    "count": 1,
    "gpio": [16],
    "pullup_mask": 1,
    "debounce_ms": [50]
  }
}
```

**ESP32 → HA (Acknowledgement):**
```json
Topic: Devices/ESP32_GARAGE/State
Payload:
{
  "dev": "ESP32_GARAGE",
  "timestamp": 1732873200,
  "config_applied": true,
  "reason": "config_applied"
}
```

---

## Changelog

### v1.1.0 (2024-11-30)
- Toegevoegd: `max_runtime_sec`, `safe_state` velden voor watchdog
- Toegevoegd: `Status` topic voor monitoring (30s heartbeat)
- Toegevoegd: `Mesh/Network/Status` voor netwerk tabel
- Toegevoegd: `Mesh/Time` voor tijd synchronisatie
- Toegevoegd: `reason` veld in State messages
- Toegevoegd: `watchdog_active`, `watchdog_remaining_sec` velden

### v1.0.0 (2024-10-01)
- Initiële release
- Basis command/state/config topics
- Relay, PWM, Input support
