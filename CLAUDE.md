# CLAUDE.md - Excellence ESP32 Project Gids

## Projectoverzicht

**Excellence** is een ESP32-gebaseerd WiFi mesh + MQTT project voor het aansturen van relais, PWM-uitgangen en het uitlezen van digitale ingangen. Het integreert met Home Assistant via MQTT en ondersteunt root nodes (direct verbonden met MQTT broker) en child nodes (communicerend via ESP-MESH).

### Snel Overzicht
- **Platform**: ESP32 (espressif32)
- **Framework**: ESP-IDF (via PlatformIO)
- **Taal**: C
- **Flash Grootte**: 2MB (geen OTA)
- **Primair Gebruik**: Domotica relais/PWM/input besturing
- **Communicatie**: WiFi → MQTT (root nodes), ESP-MESH (child nodes)

## Architectuur

### High-Level Flow
```
WiFi ↔ MQTT Broker (Home Assistant)
         ↕
    Root Node (ESP32)
         ↕
    ESP-MESH Netwerk
         ↕
   Child Nodes (ESP32s)
         ↕
   Relais/PWM/Input Hardware
```

### Component Laag Model
```
┌─────────────────────────────────────────┐
│       Applicatie (main.c)                │
├─────────────────────────────────────────┤
│  Parser → Router → Driver Executie       │
├─────────────────────────────────────────┤
│  MQTT Link  │  Mesh Link  │  WiFi Link  │
├─────────────────────────────────────────┤
│  Relay Ctrl │  PWM Ctrl   │  Input Ctrl │
├─────────────────────────────────────────┤
│         Config Store (NVS)               │
├─────────────────────────────────────────┤
│           ESP-IDF / FreeRTOS             │
└─────────────────────────────────────────┘
```

## Directory Structuur

```
Excellence/
├── src/
│   └── main.c                    # Applicatie startpunt
├── components/                   # Modulaire ESP-IDF componenten
│   ├── cfg_mqtt/                 # MQTT-gebaseerd configuratiebeheer
│   ├── config_store/             # NVS-backed configuratieopslag
│   ├── input_ctrl/               # Digitale input driver (debounced)
│   ├── mesh_link/                # ESP-MESH communicatielaag
│   ├── mqtt_link/                # MQTT client wrapper
│   ├── parser/                   # JSON → canonieke message parser
│   ├── pwm_ctrl/                 # PWM output driver (LED dimming)
│   ├── relay_ctrl/               # Relais output driver (auto-off ondersteuning)
│   ├── router/                   # Message routing (lokaal/remote/mesh)
│   └── wifi_link/                # WiFi connectiebeheer
├── include/                      # Project-wide headers (momenteel leeg)
├── lib/                          # Externe libraries (momenteel leeg)
├── test/                         # Unit tests (momenteel leeg)
├── partitions/
│   └── esp32-2mb-noota.csv       # Flash partitietabel (2MB, geen OTA)
├── platformio.ini                # PlatformIO configuratie
├── secrets_template.ini          # Template voor WiFi/MQTT credentials
├── secrets.ini                   # GENEGEERD - echte credentials (maak aan vanuit template)
├── CMakeLists.txt                # ESP-IDF build startpunt
├── sdkconfig.esp32dev            # ESP-IDF SDK configuratie
├── .gitignore                    # Git ignore regels
└── README.md                     # Basis project README
```

## Core Componenten

### 1. Parser (`components/parser/`)
**Doel**: Converteert JSON berichten naar canonieke C structuren.

**Belangrijkste Concepten**:
- Ondersteunt meerdere veld aliassen (bijv. `target`, `device`, `dev` → `target_dev`)
- Valideert en normaliseert input
- Genereert correlatie-ID's indien ontbrekend
- Foutrapportage met pad en details

**Data Flow**:
```
JSON string → parser_parse() → parser_result_t { ok, msg, error }
```

**Types**:
- `msg_type_t`: COMMAND, QUERY, EVENT, ACK, ERROR
- `io_kind_t`: RELAY, PWM, INPUT
- `action_t`: ON, OFF, TOGGLE, SET, READ, REPORT

**Voorbeeldgebruik** (uit main.c:147-153):
```c
parser_meta_t meta = { .source=PARSER_SRC_MQTT, .topic_hint=topic };
parser_result_t r = parser_parse(json, &meta);
if (!r.ok) {
    publish_parse_error(&r, MQTT_CLIENT_ID);
    return;
}
router_handle(&r.msg);
```

### 2. Router (`components/router/`)
**Doel**: Routeert geparseerde berichten naar lokale drivers of remote nodes.

**Verantwoordelijkheden**:
- Bepaalt of bericht voor lokaal apparaat of remote is
- Roept juiste driver execution callbacks aan
- Publiceert MQTT state updates
- Handelt mesh forwarding af (voor root nodes)

**Belangrijkste Functies**:
- `router_init()`: Initialiseer met callbacks
- `router_set_local_dev()`: Stel apparaatnaam in
- `router_handle()`: Verwerk een geparseerd bericht
- `router_emit_event()`: Verstuur events vanaf child nodes

**Status Codes**:
```c
typedef enum {
  ROUTER_OK = 0,
  ROUTER_ERR_INVALID,
  ROUTER_ERR_OUT_OF_RANGE,
  ROUTER_ERR_NO_ROUTE,
  ROUTER_ERR_TIMEOUT,
  ROUTER_ERR_INTERNAL
} router_status_t;
```

### 3. Config Store (`components/config_store/`)
**Doel**: Persistente configuratieopslag met NVS (Non-Volatile Storage).

**Configuratie Structuur** (`cfg_t`):
```c
- dev_name[32]              // Apparaat identifier
- relay_gpio[8]             // GPIO pins voor relais (max 8)
- relay_count               // Aantal actieve relais
- relay_active_low_mask     // Bitmask voor active-low logica
- relay_open_drain_mask     // Bitmask voor open-drain modus
- relay_autoff_sec[8]       // Auto-off timers per relais
- pwm_gpio[8]               // GPIO pins voor PWM uitgangen
- pwm_count                 // Aantal PWM kanalen
- pwm_inverted_mask         // Bitmask voor geïnverteerde PWM
- pwm_freq_hz               // PWM frequentie
- input_gpio[8]             // GPIO pins voor inputs
- input_count               // Aantal input kanalen
- input_pullup_mask         // Bitmask voor pull-up weerstanden
- input_pulldown_mask       // Bitmask voor pull-down weerstanden
- input_inverted_mask       // Bitmask voor geïnverteerde logica
- input_debounce_ms[8]      // Debounce tijd per input
```

**API**:
```c
esp_err_t config_init(void);              // Laad of maak defaults
const cfg_t* config_get_cached(void);     // Haal huidige config op
esp_err_t config_commit(void);            // Opslaan naar NVS
void config_set_relays(int *gpios, int count);
void config_set_pwm(int *gpios, int count, uint32_t freq_hz);
// ... en meer setters
```

### 4. Relay Control (`components/relay_ctrl/`)
**Doel**: GPIO-gebaseerde relaisbesturing met auto-off timers.

**Features**:
- Active-high of active-low logica per kanaal
- Open-drain of push-pull modus
- Per-kanaal auto-off timers (voor pulse toepassingen)
- State change callbacks

**API**:
```c
esp_err_t relay_ctrl_init(const int *gpio_map, int count,
                           uint32_t active_low_mask, uint32_t open_drain_mask);
void relay_ctrl_on(int ch);
void relay_ctrl_off(int ch);
void relay_ctrl_toggle(int ch);
bool relay_ctrl_get_state(int ch);
void relay_ctrl_set_autoff_seconds(int ch, uint32_t sec);
void relay_ctrl_set_state_hook(void (*hook)(int ch, bool on));
```

### 5. PWM Control (`components/pwm_ctrl/`)
**Doel**: PWM output besturing voor LED dimming, motorsnelheid, etc.

**Features**:
- 13-bit resolutie (0-8191)
- Hardware fade ondersteuning
- Per-kanaal inversie
- Configureerbare frequentie

**API**:
```c
esp_err_t pwm_ctrl_init(const int *gpio_map, int count,
                         uint32_t inverted_mask, uint32_t freq_hz);
void pwm_ctrl_set_duty(int ch, uint32_t duty);
void pwm_ctrl_fade_to(int ch, uint32_t target_duty, uint32_t duration_ms);
uint32_t pwm_ctrl_get_duty(int ch);
void pwm_ctrl_set_state_hook(void (*hook)(int ch, uint32_t duty));
```

### 6. Input Control (`components/input_ctrl/`)
**Doel**: Debounced digitale input uitlezing (knoppen, sensoren).

**Features**:
- Software debouncing (configureerbaar per kanaal)
- Pull-up/pull-down configuratie
- Logica inversie ondersteuning
- State change callbacks

**API**:
```c
esp_err_t input_ctrl_init(const int *gpio_map, int count,
                           uint32_t pullup_mask, uint32_t pulldown_mask,
                           uint32_t inverted_mask, uint32_t debounce_ms_def);
bool input_ctrl_get_level(int ch);
void input_ctrl_set_debounce_ms(int ch, uint32_t ms);
void input_ctrl_set_state_hook(void (*hook)(int ch, bool level));
```

### 7. WiFi Link (`components/wifi_link/`)
**Doel**: WiFi station mode connectiebeheer.

**Features**:
- Auto-reconnect
- Hostname configuratie
- Power save besturing
- Connectie callbacks

**Context Structuur**:
```c
typedef struct {
    char ssid[32];
    char pass[64];
    char hostname[32];
    bool power_save;
} wifi_ctx_t;
```

### 8. MQTT Link (`components/mqtt_link/`)
**Doel**: MQTT client wrapper met topic management.

**Topic Structuur**:
```
{base_prefix}/{device_name}/Cmd/Set      # Commands (root subscribes)
{base_prefix}/{device_name}/Config/Set   # Configuratie updates
{base_prefix}/{device_name}/State        # Status publicaties
```

**Context Structuur**:
```c
typedef struct {
    char host[64];
    uint16_t port;
    char base_prefix[32];   // bijv. "Devices"
    char local_dev[32];     // bijv. "ESP32_ROOT"
    char client_id[32];
    char username[32];
    char password[64];
    bool is_root;           // true = subscribe op Cmd/Set & Config/Set
} mqtt_ctx_t;
```

### 9. Mesh Link (`components/mesh_link/`)
**Doel**: ESP-MESH communicatie voor child node forwarding.

**Status**: Component bestaat maar implementatie is mogelijk minimaal in huidige main.c.

### 10. Config MQTT (`components/cfg_mqtt/`)
**Doel**: Runtime configuratie via MQTT berichten.

**Features**:
- Toepassen van volledige configuratie vanuit JSON
- Doorsturen config naar specifieke apparaten (root)
- ACK/ERROR responses

## Build Systeem

### PlatformIO Configuratie

**Bestand**: `platformio.ini`

```ini
[platformio]
extra_configs = secrets.ini    # Extern credentials bestand

[env:esp32dev]
platform = espressif32
board = esp32dev
framework = espidf
board_build.flash_size = 2MB
monitor_speed = 115200
board_build.partitions = partitions/esp32-2mb-noota.csv
```

### Build Flags (uit secrets.ini)

**Bestand**: `secrets_template.ini` → **MAAK AAN** `secrets.ini`

Vereiste defines:
```ini
[env:esp32dev]
build_flags =
  -DWIFI_SSID="jouw_wifi_ssid"
  -DWIFI_PASS="jouw_wifi_wachtwoord"
  -DMQTT_HOST="192.168.1.100"
  -DMQTT_PORT=1883
  -DMQTT_CLIENT_ID="ESP32_ROOT"
  -DMQTT_USER="mqtt_gebruiker"
  -DMQTT_PASS="mqtt_wachtwoord"
  -DCONFIG_MESH_CHANNEL=6
  -DCONFIG_MESH_AP_CONNECTIONS=6
  -DCONFIG_MESH_ID_0=0x11
  # ... mesh ID bytes
  -DCONFIG_MESH_ROUTER_SSID="mesh_router_ssid"
  -DCONFIG_MESH_ROUTER_PASSWD="mesh_router_pass"
```

## Development Workflows

### Initiële Setup

1. **Installeer Vereisten**:
   - VS Code + PlatformIO extensie
   - Git

2. **Clone Repository**:
   ```bash
   git clone https://github.com/TommyTurboo/Excellence.git
   cd Excellence
   ```

3. **Configureer Secrets**:
   ```bash
   cp secrets_template.ini secrets.ini
   # Bewerk secrets.ini met echte credentials
   ```

4. **Build & Upload**:
   ```bash
   pio run -e esp32dev -t upload
   pio device monitor -b 115200
   ```

### Veelvoorkomende Development Taken

#### Toevoegen van Nieuw Component

1. Maak component directory:
   ```bash
   mkdir -p components/my_component/include
   mkdir -p components/my_component
   ```

2. Voeg `CMakeLists.txt` toe in component map:
   ```cmake
   idf_component_register(
       SRCS "my_component.c"
       INCLUDE_DIRS "include"
       REQUIRES esp_log  # voeg dependencies toe
   )
   ```

3. Maak header in `include/my_component.h`
4. Maak implementatie in `my_component.c`
5. Include in `src/main.c` of andere componenten

#### Wijzigen van Configuratie Schema

1. Bewerk `components/config_store/include/config_store.h` (`cfg_t` struct)
2. Update `config_reset_defaults()` in `config_store.c`
3. Voeg getter/setter functies toe
4. Update NVS save/load logica
5. **Belangrijk**: Overweeg versie migratie voor bestaande apparaten

#### Toevoegen van Nieuw Command/Action

1. **Parser**: Voeg enum toe aan `action_t` of `io_kind_t` in `parser.h`
2. **Parser**: Update alias tabellen in `parser.c`
3. **Router**: Voeg execution callback toe aan `router_cbs_t`
4. **Main**: Implementeer exec functie (bijv. `exec_new_feature()`)
5. **Main**: Registreer callback in `hook_router_init()`

#### Testen van Wijzigingen

**Huidige Staat**: Geen geautomatiseerde tests aanwezig.

**Handmatige Test Aanpak**:
1. Gebruik serial monitor om logs te observeren
2. Verstuur MQTT berichten naar `Devices/{device}/Cmd/Set`
3. Voorbeeld MQTT commando:
   ```json
   {
     "target_dev": "ESP32_ROOT",
     "io_kind": "RELAY",
     "io_id": 0,
     "action": "TOGGLE"
   }
   ```
4. Controleer `Devices/{device}/State` voor responses

**Test Code**: main.c bevat uitgebreide uitgecommen testcode (regels 242-851):
- Parser tests (regel 536)
- Router tests (regel 413)
- Config tests (regel 604)
- Driver tests (relais: 826, PWM: 801, input: 753)

**Om testcode te gebruiken**: Comment productie `app_main()` uit, uncomment gewenste test sectie.

## Belangrijkste Conventies

### Code Stijl

1. **Naamgeving**:
   - Component functies: `component_action()` (bijv. `relay_ctrl_on()`)
   - Static functies: `snake_case`
   - Types: `snake_case_t` (bijv. `parser_msg_t`)
   - Enums: `UPPER_CASE` waarden

2. **Error Handling**:
   - Gebruik `ESP_ERROR_CHECK()` voor kritieke operaties
   - Return `esp_err_t` of custom status enums
   - Log met ESP_LOG macros (`ESP_LOGI`, `ESP_LOGW`, `ESP_LOGE`)

3. **Memory Management**:
   - Geef voorkeur aan stack allocatie voor kleine structs
   - Gebruik `cJSON_Delete()` na `cJSON_Parse()`
   - Free gealloceerde strings van `cJSON_PrintUnformatted()`

4. **Logging**:
   - Definieer `TAG` constante per bestand: `static const char *TAG = "COMPONENT";`
   - Gebruik context in logs: `ESP_LOGI(TAG, "Relay %d -> %s", ch, on?"ON":"OFF");`

### Component Design Principes

1. **Separation of Concerns**:
   - Elk component heeft één verantwoordelijkheid
   - Duidelijke interfaces via header bestanden
   - Minimale dependencies

2. **Callback-Based Integration**:
   - Drivers gebruiken callbacks voor state changes
   - Router gebruikt callbacks voor executie
   - Maakt loose coupling mogelijk

3. **Configuration-Driven**:
   - Hardware mapping via config (niet hardcoded)
   - Runtime herconfiguratie ondersteund
   - Persistente opslag in NVS

### MQTT Bericht Formaat

**Command** (`/Cmd/Set`):
```json
{
  "target_dev": "ESP32_A",
  "io_kind": "RELAY",
  "io_id": 0,
  "action": "ON",
  "duration_ms": 5000,
  "corr_id": "optioneel-uuid"
}
```

**State** (`/State`):
```json
{
  "corr_id": "bijpassend-uuid",
  "dev": "ESP32_A",
  "status": "OK",
  "io_kind": "RELAY",
  "io_id": 0,
  "state": true
}
```

**Error** (`/State`):
```json
{
  "corr_id": "bijpassend-uuid",
  "dev": "ESP32_A",
  "status": "ERROR",
  "code": "OUT_OF_RANGE",
  "path": "io_id",
  "detail": "io_id=99 overschrijdt relay_count=4"
}
```

### Veld Aliassen (Parser)

De parser ondersteunt meerdere JSON veldnamen voor flexibiliteit:

| Canoniek Veld | Geaccepteerde Aliassen |
|---------------|------------------------|
| `target_dev` | `target`, `device`, `dev` |
| `io_kind` | `io`, `type`, `kind` |
| `io_id` | `gpio`, `pin`, `channel`, `ch`, `id` |
| `action` | `command`, `cmd`, `act` |
| `duration_ms` | `seconds`, `minutes` (auto-geconverteerd) |
| `brightness_pct` | `brightness`, `duty`, `level`, `pct` |
| `ramp_ms` | `ramp`, `fade`, `transition` |

## Applicatie Flow (main.c)

### Opstart Sequentie

```c
void app_main(void) {
    // 1. Initialiseer NVS en laad config
    config_init();
    const cfg_t *cfg = config_get_cached();

    // 2. Initialiseer drivers met config
    relay_ctrl_init(cfg->relay_gpio, cfg->relay_count, ...);
    pwm_ctrl_init(cfg->pwm_gpio, cfg->pwm_count, ...);
    input_ctrl_init(cfg->input_gpio, cfg->input_count, ...);

    // 3. Stel driver parameters in vanuit config
    for (int i=0; i<cfg->relay_count; ++i)
        relay_ctrl_set_autoff_seconds(i, cfg->relay_autoff_sec[i]);
    // ... vergelijkbaar voor PWM, input

    // 4. Start WiFi (triggert on_ip callback bij succes)
    wifi_link_init(&w, &cb);
    wifi_link_start();
}
```

### Runtime Flow (na WiFi connectie)

```c
on_ip() {
    // 1. Initialiseer MQTT client
    mqtt_link_init(&m, &cbs);

    // 2. Initialiseer router
    hook_router_init(MQTT_CLIENT_ID);
}

on_cmd_set(json, topic) {
    // 1. Parse JSON → canoniek msg
    parser_result_t r = parser_parse(json, &meta);

    // 2. Handle met router
    router_handle(&r.msg);
        ↓
    // 3. Router roept exec_relay/exec_pwm/exec_input aan
    exec_relay(msg) → relay_ctrl_on/off/toggle()
        ↓
    // 4. Router publiceert State response via MQTT
}
```

## Belangrijke Notities voor AI Assistenten

### Bij Maken van Wijzigingen

1. **Controleer altijd config schema**: Hardware mapping is config-driven, niet hardcoded.
2. **Behoud error handling**: ESP32 ontwikkeling vereist robuuste error checks.
3. **Onderhoud logging**: Seriële logs zijn primair debug mechanisme.
4. **Test met echte hardware**: Embedded systemen kunnen niet volledig gesimuleerd worden.
5. **Let op geheugengebruik**: ESP32 heeft beperkt RAM (~520KB totaal, minder beschikbaar).
6. **Vermijd blocking operaties**: FreeRTOS tasks moeten regelmatig yielden.

### Beveiligingsoverwegingen

1. **secrets.ini is gitignored**: Commit nooit credentials.
2. **MQTT authenticatie**: Momenteel ondersteund maar optioneel.
3. **Geen TLS**: MQTT connectie is plaintext (overweeg toevoegen mbedTLS).
4. **Geen autorisatie**: Elk geldig MQTT bericht wordt uitgevoerd.
5. **Command injection**: JSON parser valideert types, maar geen input sanitization voor downstream systemen.

### Performance Overwegingen

1. **Flash is beperkt (2MB)**: Geen OTA partitie om ruimte te besparen.
2. **WiFi power save uitgeschakeld**: Voor betere MQTT latency.
3. **MQTT QoS 1**: Garandeert berichtaflevering (at-least-once).
4. **Geen retain op State**: Vermindert broker geheugengebruik.

### Bekende Beperkingen

1. **Geen OTA updates**: Vereist fysieke toegang voor firmware updates.
2. **Geen mesh implementatie in main.c**: Mesh componenten bestaan maar niet volledig geïntegreerd.
3. **Geen unit tests**: Alle testing is handmatig/integratie.
4. **Geen web interface**: Configuratie alleen via MQTT of NVS.
5. **Enkele MQTT broker**: Geen failover of multi-broker ondersteuning.

## Debugging Tips

### Serial Monitor

```bash
pio device monitor -b 115200
```

**Belangrijke logs om te volgen**:
- `[WIFI]`: Connectie status
- `[MQTT]`: Broker connectie, pub/sub activiteit
- `[MQ_RX]`: Inkomende MQTT berichten
- `[PARSER]`: JSON parsing fouten
- `[EXEC]`: Driver executie
- `[CFG]`: Configuratie wijzigingen

### Veelvoorkomende Problemen

1. **WiFi maakt geen verbinding**: Controleer `WIFI_SSID` en `WIFI_PASS` in secrets.ini
2. **MQTT maakt geen verbinding**: Verifieer `MQTT_HOST`, poort, credentials
3. **Commando's genegeerd**: Controleer of `target_dev` overeenkomt met `MQTT_CLIENT_ID`
4. **Relais schakelt niet**: Verifieer GPIO mapping, active_low_mask, bedrading
5. **PWM niet zichtbaar**: Controleer frequentie, duty cycle, hardware connectie
6. **Input leest altijd hetzelfde**: Controleer pull weerstanden, inverted_mask, debounce

### Gebruik van Test Code

De uitgebreide testcode in main.c (uitgecommen) is waardevol:

1. **Om parser te testen**:
   - Comment productie `app_main()` uit (regel 209-238)
   - Uncomment parser test (regel 579-601)
   - Build & monitor serial output

2. **Om drivers te testen**:
   - Uncomment relevante test (relais: 836, PWM: 812, input: 761)
   - Pas GPIO pins aan naar jouw hardware
   - Build & observeer gedrag

## Git Workflow

### Branching Strategie

- **Main branch**: Stabiele releases
- **Feature branches**: `claude/claude-md-*` (AI-assisted development)

### Huidige Branch

```
claude/claude-md-mhxw6n9spbm3z1om-01SR2RwNciiMtq3CbCwdm25P
```

Alle development moet op deze branch plaatsvinden tot klaar voor PR.

### Committen

1. **Stage wijzigingen**: Typisch via PlatformIO/VS Code UI
2. **Beschrijvende berichten**: Leg "waarom" uit, niet alleen "wat"
3. **Test voor commit**: Op z'n minst, zorg dat build slaagt
4. **Push regelmatig**: `git push -u origin <branch-name>`

### .gitignore Regels

- `secrets.ini`: Bevat credentials
- `.pio/`: PlatformIO build artifacts
- Build outputs: `.elf`, `.bin`, `.map`, etc.
- VSCode internals: `.vscode/ipch/`, `.vscode/c_cpp_properties.json`

## Integratie met Home Assistant

### Discovery

Apparaten discoveren niet automatisch. Handmatige MQTT sensor/switch configuratie vereist.

### Voorbeeld HA Configuratie

```yaml
# configuration.yaml
switch:
  - platform: mqtt
    name: "Garage Relais 0"
    state_topic: "Devices/ESP32_ROOT/State"
    command_topic: "Devices/ESP32_ROOT/Cmd/Set"
    payload_on: '{"target_dev":"ESP32_ROOT","io_kind":"RELAY","io_id":0,"action":"ON"}'
    payload_off: '{"target_dev":"ESP32_ROOT","io_kind":"RELAY","io_id":0,"action":"OFF"}'
    value_template: '{{ value_json.state if value_json.io_id == 0 else states("switch.garage_relais_0") }}'
    optimistic: false
```

## Toekomstige Verbeteringen

1. **OTA Ondersteuning**: Grotere flash of externe opslag
2. **Web UI**: ESP32 webserver voor lokale config
3. **Mesh Integratie**: Volledig implementeren child node forwarding
4. **TLS/Encryptie**: Beveilig MQTT met mbedTLS
5. **Autorisatie**: Role-based command filtering
6. **Metrics**: Uptime, geheugengebruik, commando statistieken
7. **Event Streaming**: Continue input monitoring
8. **Home Assistant Discovery**: Auto-configureer entities
9. **Backup/Restore**: Export/import volledige configuratie
10. **Unit Tests**: Geautomatiseerd testing framework

## Snelle Referentie

### Build Commando's

```bash
# Volledige build
pio run -e esp32dev

# Upload firmware
pio run -e esp32dev -t upload

# Monitor serial
pio device monitor -b 115200

# Clean build
pio run -e esp32dev -t clean

# Build + upload + monitor
pio run -e esp32dev -t upload && pio device monitor -b 115200
```

### Bestandslocatie Referentie

| Doel | Locatie |
|------|---------|
| Applicatie startpunt | `src/main.c` |
| Parser logica | `components/parser/parser.c` |
| Router logica | `components/router/router.c` |
| Config opslag | `components/config_store/config_store.c` |
| Relais driver | `components/relay_ctrl/relay_ctrl.c` |
| PWM driver | `components/pwm_ctrl/pwm_ctrl.c` |
| Input driver | `components/input_ctrl/input_ctrl.c` |
| WiFi beheer | `components/wifi_link/wifi_link.c` |
| MQTT client | `components/mqtt_link/mqtt_link.c` |
| Credentials | `secrets.ini` (gitignored) |
| Partitietabel | `partitions/esp32-2mb-noota.csv` |

---

**Laatst Bijgewerkt**: 2025-11-13
**Project Versie**: Initiële ontwikkeling
**Beheerd Door**: TommyTurboo
**AI Assistent**: Geoptimaliseerd voor Claude Code begrip
