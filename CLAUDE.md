# CLAUDE.md - Excellence ESP32 Project Guide

## Project Overview

**Excellence** is an ESP32-based WiFi mesh + MQTT project for controlling relays, PWM outputs, and reading digital inputs. It integrates with Home Assistant through MQTT, supporting root nodes (directly connected to MQTT broker) and child nodes (communicating via ESP-MESH).

### Quick Facts
- **Platform**: ESP32 (espressif32)
- **Framework**: ESP-IDF (via PlatformIO)
- **Language**: C
- **Flash Size**: 2MB (no OTA)
- **Primary Use**: Home automation relay/PWM/input control
- **Communication**: WiFi → MQTT (root nodes), ESP-MESH (child nodes)

## Architecture

### High-Level Flow
```
WiFi ↔ MQTT Broker (Home Assistant)
         ↕
    Root Node (ESP32)
         ↕
    ESP-MESH Network
         ↕
   Child Nodes (ESP32s)
         ↕
   Relay/PWM/Input Hardware
```

### Component Layer Model
```
┌─────────────────────────────────────────┐
│         Application (main.c)             │
├─────────────────────────────────────────┤
│  Parser → Router → Driver Execution      │
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

## Directory Structure

```
Excellence/
├── src/
│   └── main.c                    # Application entry point
├── components/                   # Modular ESP-IDF components
│   ├── cfg_mqtt/                 # MQTT-based config management
│   ├── config_store/             # NVS-backed configuration storage
│   ├── input_ctrl/               # Digital input driver (debounced)
│   ├── mesh_link/                # ESP-MESH communication layer
│   ├── mqtt_link/                # MQTT client wrapper
│   ├── parser/                   # JSON → canonical message parser
│   ├── pwm_ctrl/                 # PWM output driver (LED dimming)
│   ├── relay_ctrl/               # Relay output driver (auto-off support)
│   ├── router/                   # Message routing (local/remote/mesh)
│   └── wifi_link/                # WiFi connection management
├── include/                      # Project-wide headers (currently empty)
├── lib/                          # External libraries (currently empty)
├── test/                         # Unit tests (currently empty)
├── partitions/
│   └── esp32-2mb-noota.csv       # Flash partition table (2MB, no OTA)
├── platformio.ini                # PlatformIO configuration
├── secrets_template.ini          # Template for WiFi/MQTT credentials
├── secrets.ini                   # IGNORED - actual credentials (create from template)
├── CMakeLists.txt                # ESP-IDF build entry point
├── sdkconfig.esp32dev            # ESP-IDF SDK configuration
├── .gitignore                    # Git ignore rules
└── README.md                     # Basic project README
```

## Core Components

### 1. Parser (`components/parser/`)
**Purpose**: Converts JSON messages into canonical C structures.

**Key Concepts**:
- Supports multiple field aliases (e.g., `target`, `device`, `dev` → `target_dev`)
- Validates and normalizes input
- Generates correlation IDs if missing
- Error reporting with path and detail

**Data Flow**:
```
JSON string → parser_parse() → parser_result_t { ok, msg, error }
```

**Types**:
- `msg_type_t`: COMMAND, QUERY, EVENT, ACK, ERROR
- `io_kind_t`: RELAY, PWM, INPUT
- `action_t`: ON, OFF, TOGGLE, SET, READ, REPORT

**Example Usage** (from main.c:147-153):
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
**Purpose**: Routes parsed messages to local drivers or remote nodes.

**Responsibilities**:
- Determines if message is for local device or remote
- Calls appropriate driver execution callbacks
- Publishes MQTT state updates
- Handles mesh forwarding (for root nodes)

**Key Functions**:
- `router_init()`: Initialize with callbacks
- `router_set_local_dev()`: Set device name
- `router_handle()`: Process a parsed message
- `router_emit_event()`: Send events from child nodes

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
**Purpose**: Persistent configuration storage using NVS (Non-Volatile Storage).

**Configuration Structure** (`cfg_t`):
```c
- dev_name[32]              // Device identifier
- relay_gpio[8]             // GPIO pins for relays (max 8)
- relay_count               // Number of active relays
- relay_active_low_mask     // Bitmask for active-low logic
- relay_open_drain_mask     // Bitmask for open-drain mode
- relay_autoff_sec[8]       // Auto-off timers per relay
- pwm_gpio[8]               // GPIO pins for PWM outputs
- pwm_count                 // Number of PWM channels
- pwm_inverted_mask         // Bitmask for inverted PWM
- pwm_freq_hz               // PWM frequency
- input_gpio[8]             // GPIO pins for inputs
- input_count               // Number of input channels
- input_pullup_mask         // Bitmask for pull-up resistors
- input_pulldown_mask       // Bitmask for pull-down resistors
- input_inverted_mask       // Bitmask for inverted logic
- input_debounce_ms[8]      // Debounce time per input
```

**API**:
```c
esp_err_t config_init(void);              // Load or create defaults
const cfg_t* config_get_cached(void);     // Get current config
esp_err_t config_commit(void);            // Save to NVS
void config_set_relays(int *gpios, int count);
void config_set_pwm(int *gpios, int count, uint32_t freq_hz);
// ... and more setters
```

### 4. Relay Control (`components/relay_ctrl/`)
**Purpose**: GPIO-based relay control with auto-off timers.

**Features**:
- Active-high or active-low logic per channel
- Open-drain or push-pull mode
- Per-channel auto-off timers (for pulse applications)
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
**Purpose**: PWM output control for LED dimming, motor speed, etc.

**Features**:
- 13-bit resolution (0-8191)
- Hardware fade support
- Per-channel inversion
- Configurable frequency

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
**Purpose**: Debounced digital input reading (buttons, sensors).

**Features**:
- Software debouncing (configurable per channel)
- Pull-up/pull-down configuration
- Logic inversion support
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
**Purpose**: WiFi station mode connection management.

**Features**:
- Auto-reconnect
- Hostname configuration
- Power save control
- Connection callbacks

**Context Structure**:
```c
typedef struct {
    char ssid[32];
    char pass[64];
    char hostname[32];
    bool power_save;
} wifi_ctx_t;
```

### 8. MQTT Link (`components/mqtt_link/`)
**Purpose**: MQTT client wrapper with topic management.

**Topic Structure**:
```
{base_prefix}/{device_name}/Cmd/Set      # Commands (root subscribes)
{base_prefix}/{device_name}/Config/Set   # Configuration updates
{base_prefix}/{device_name}/State        # Status publications
```

**Context Structure**:
```c
typedef struct {
    char host[64];
    uint16_t port;
    char base_prefix[32];   // e.g., "Devices"
    char local_dev[32];     // e.g., "ESP32_ROOT"
    char client_id[32];
    char username[32];
    char password[64];
    bool is_root;           // true = subscribe to Cmd/Set & Config/Set
} mqtt_ctx_t;
```

### 9. Mesh Link (`components/mesh_link/`)
**Purpose**: ESP-MESH communication for child node forwarding.

**Status**: Component exists but implementation may be minimal in current main.c.

### 10. Config MQTT (`components/cfg_mqtt/`)
**Purpose**: Runtime configuration via MQTT messages.

**Features**:
- Apply full configuration from JSON
- Forward config to specific devices (root)
- ACK/ERROR responses

## Build System

### PlatformIO Configuration

**File**: `platformio.ini`

```ini
[platformio]
extra_configs = secrets.ini    # External credentials file

[env:esp32dev]
platform = espressif32
board = esp32dev
framework = espidf
board_build.flash_size = 2MB
monitor_speed = 115200
board_build.partitions = partitions/esp32-2mb-noota.csv
```

### Build Flags (from secrets.ini)

**File**: `secrets_template.ini` → **CREATE** `secrets.ini`

Required defines:
```ini
[env:esp32dev]
build_flags =
  -DWIFI_SSID="your_wifi_ssid"
  -DWIFI_PASS="your_wifi_password"
  -DMQTT_HOST="192.168.1.100"
  -DMQTT_PORT=1883
  -DMQTT_CLIENT_ID="ESP32_ROOT"
  -DMQTT_USER="mqtt_user"
  -DMQTT_PASS="mqtt_password"
  -DCONFIG_MESH_CHANNEL=6
  -DCONFIG_MESH_AP_CONNECTIONS=6
  -DCONFIG_MESH_ID_0=0x11
  # ... mesh ID bytes
  -DCONFIG_MESH_ROUTER_SSID="mesh_router_ssid"
  -DCONFIG_MESH_ROUTER_PASSWD="mesh_router_pass"
```

## Development Workflows

### Initial Setup

1. **Install Prerequisites**:
   - VS Code + PlatformIO extension
   - Git

2. **Clone Repository**:
   ```bash
   git clone https://github.com/TommyTurboo/Excellence.git
   cd Excellence
   ```

3. **Configure Secrets**:
   ```bash
   cp secrets_template.ini secrets.ini
   # Edit secrets.ini with actual credentials
   ```

4. **Build & Upload**:
   ```bash
   pio run -e esp32dev -t upload
   pio device monitor -b 115200
   ```

### Common Development Tasks

#### Adding a New Component

1. Create component directory:
   ```bash
   mkdir -p components/my_component/include
   mkdir -p components/my_component
   ```

2. Add `CMakeLists.txt` in component folder:
   ```cmake
   idf_component_register(
       SRCS "my_component.c"
       INCLUDE_DIRS "include"
       REQUIRES esp_log  # add dependencies
   )
   ```

3. Create header in `include/my_component.h`
4. Create implementation in `my_component.c`
5. Include in `src/main.c` or other components

#### Modifying Configuration Schema

1. Edit `components/config_store/include/config_store.h` (`cfg_t` struct)
2. Update `config_reset_defaults()` in `config_store.c`
3. Add getter/setter functions
4. Update NVS save/load logic
5. **Important**: Consider version migration for existing devices

#### Adding a New Command/Action

1. **Parser**: Add enum to `action_t` or `io_kind_t` in `parser.h`
2. **Parser**: Update alias tables in `parser.c`
3. **Router**: Add execution callback to `router_cbs_t`
4. **Main**: Implement exec function (e.g., `exec_new_feature()`)
5. **Main**: Register callback in `hook_router_init()`

#### Testing Changes

**Current State**: No automated tests present.

**Manual Testing Approach**:
1. Use serial monitor to observe logs
2. Send MQTT messages to `Devices/{device}/Cmd/Set`
3. Example MQTT command:
   ```json
   {
     "target_dev": "ESP32_ROOT",
     "io_kind": "RELAY",
     "io_id": 0,
     "action": "TOGGLE"
   }
   ```
4. Check `Devices/{device}/State` for responses

**Test Code**: main.c contains extensive commented-out test code (lines 242-851):
- Parser tests (line 536)
- Router tests (line 413)
- Config tests (line 604)
- Driver tests (relay: 826, PWM: 801, input: 753)

**To use test code**: Comment out production `app_main()`, uncomment desired test section.

## Key Conventions

### Code Style

1. **Naming**:
   - Component functions: `component_action()` (e.g., `relay_ctrl_on()`)
   - Static functions: `snake_case`
   - Types: `snake_case_t` (e.g., `parser_msg_t`)
   - Enums: `UPPER_CASE` values

2. **Error Handling**:
   - Use `ESP_ERROR_CHECK()` for critical operations
   - Return `esp_err_t` or custom status enums
   - Log with ESP_LOG macros (`ESP_LOGI`, `ESP_LOGW`, `ESP_LOGE`)

3. **Memory Management**:
   - Prefer stack allocation for small structs
   - Use `cJSON_Delete()` after `cJSON_Parse()`
   - Free allocated strings from `cJSON_PrintUnformatted()`

4. **Logging**:
   - Define `TAG` constant per file: `static const char *TAG = "COMPONENT";`
   - Use context in logs: `ESP_LOGI(TAG, "Relay %d -> %s", ch, on?"ON":"OFF");`

### Component Design Principles

1. **Separation of Concerns**:
   - Each component has single responsibility
   - Clear interfaces via header files
   - Minimal dependencies

2. **Callback-Based Integration**:
   - Drivers use callbacks for state changes
   - Router uses callbacks for execution
   - Enables loose coupling

3. **Configuration-Driven**:
   - Hardware mapping via config (not hardcoded)
   - Runtime reconfiguration supported
   - Persistent storage in NVS

### MQTT Message Format

**Command** (`/Cmd/Set`):
```json
{
  "target_dev": "ESP32_A",
  "io_kind": "RELAY",
  "io_id": 0,
  "action": "ON",
  "duration_ms": 5000,
  "corr_id": "optional-uuid"
}
```

**State** (`/State`):
```json
{
  "corr_id": "matching-uuid",
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
  "corr_id": "matching-uuid",
  "dev": "ESP32_A",
  "status": "ERROR",
  "code": "OUT_OF_RANGE",
  "path": "io_id",
  "detail": "io_id=99 exceeds relay_count=4"
}
```

### Field Aliases (Parser)

The parser supports multiple JSON field names for flexibility:

| Canonical Field | Accepted Aliases |
|-----------------|------------------|
| `target_dev` | `target`, `device`, `dev` |
| `io_kind` | `io`, `type`, `kind` |
| `io_id` | `gpio`, `pin`, `channel`, `ch`, `id` |
| `action` | `command`, `cmd`, `act` |
| `duration_ms` | `seconds`, `minutes` (auto-converted) |
| `brightness_pct` | `brightness`, `duty`, `level`, `pct` |
| `ramp_ms` | `ramp`, `fade`, `transition` |

## Application Flow (main.c)

### Startup Sequence

```c
void app_main(void) {
    // 1. Initialize NVS and load config
    config_init();
    const cfg_t *cfg = config_get_cached();

    // 2. Initialize drivers with config
    relay_ctrl_init(cfg->relay_gpio, cfg->relay_count, ...);
    pwm_ctrl_init(cfg->pwm_gpio, cfg->pwm_count, ...);
    input_ctrl_init(cfg->input_gpio, cfg->input_count, ...);

    // 3. Set driver parameters from config
    for (int i=0; i<cfg->relay_count; ++i)
        relay_ctrl_set_autoff_seconds(i, cfg->relay_autoff_sec[i]);
    // ... similar for PWM, input

    // 4. Start WiFi (triggers on_ip callback on success)
    wifi_link_init(&w, &cb);
    wifi_link_start();
}
```

### Runtime Flow (after WiFi connects)

```c
on_ip() {
    // 1. Initialize MQTT client
    mqtt_link_init(&m, &cbs);

    // 2. Initialize router
    hook_router_init(MQTT_CLIENT_ID);
}

on_cmd_set(json, topic) {
    // 1. Parse JSON → canonical msg
    parser_result_t r = parser_parse(json, &meta);

    // 2. Handle with router
    router_handle(&r.msg);
        ↓
    // 3. Router calls exec_relay/exec_pwm/exec_input
    exec_relay(msg) → relay_ctrl_on/off/toggle()
        ↓
    // 4. Router publishes State response via MQTT
}
```

## Important Notes for AI Assistants

### When Making Changes

1. **Always check config schema**: Hardware mapping is config-driven, not hardcoded.
2. **Preserve error handling**: ESP32 development requires robust error checks.
3. **Maintain logging**: Serial logs are primary debug mechanism.
4. **Test with actual hardware**: Embedded systems can't be fully simulated.
5. **Watch memory usage**: ESP32 has limited RAM (~520KB total, less available).
6. **Avoid blocking operations**: FreeRTOS tasks should yield regularly.

### Security Considerations

1. **secrets.ini is gitignored**: Never commit credentials.
2. **MQTT authentication**: Currently supported but optional.
3. **No TLS**: MQTT connection is plaintext (consider adding mbedTLS).
4. **No authorization**: Any valid MQTT message is executed.
5. **Command injection**: JSON parser validates types, but no input sanitization for downstream systems.

### Performance Considerations

1. **Flash is limited (2MB)**: No OTA partition to save space.
2. **WiFi power save disabled**: For better MQTT latency.
3. **MQTT QoS 1**: Ensures message delivery (at-least-once).
4. **No retain on State**: Reduces broker memory usage.

### Known Limitations

1. **No OTA updates**: Requires physical access for firmware updates.
2. **No mesh implementation in main.c**: Mesh components exist but not fully integrated.
3. **No unit tests**: All testing is manual/integration.
4. **No web interface**: Configuration only via MQTT or NVS.
5. **Single MQTT broker**: No failover or multi-broker support.

## Debugging Tips

### Serial Monitor

```bash
pio device monitor -b 115200
```

**Key logs to watch**:
- `[WIFI]`: Connection status
- `[MQTT]`: Broker connection, pub/sub activity
- `[MQ_RX]`: Incoming MQTT messages
- `[PARSER]`: JSON parsing errors
- `[EXEC]`: Driver execution
- `[CFG]`: Configuration changes

### Common Issues

1. **WiFi won't connect**: Check `WIFI_SSID` and `WIFI_PASS` in secrets.ini
2. **MQTT not connecting**: Verify `MQTT_HOST`, port, credentials
3. **Commands ignored**: Check `target_dev` matches `MQTT_CLIENT_ID`
4. **Relay not switching**: Verify GPIO mapping, active_low_mask, wiring
5. **PWM not visible**: Check frequency, duty cycle, hardware connection
6. **Input always reads same**: Check pull resistors, inverted_mask, debounce

### Using Test Code

The extensive test code in main.c (commented out) is valuable:

1. **To test parser**:
   - Comment out production `app_main()` (line 209-238)
   - Uncomment parser test (line 579-601)
   - Build & monitor serial output

2. **To test drivers**:
   - Uncomment relevant test (relay: 836, PWM: 812, input: 761)
   - Adjust GPIO pins to match your hardware
   - Build & observe behavior

## Git Workflow

### Branching Strategy

- **Main branch**: Stable releases
- **Feature branches**: `claude/claude-md-*` (AI-assisted development)

### Current Branch

```
claude/claude-md-mhxw6n9spbm3z1om-01SR2RwNciiMtq3CbCwdm25P
```

All development should occur on this branch until ready for PR.

### Committing

1. **Stage changes**: Typically via PlatformIO/VS Code UI
2. **Descriptive messages**: Explain "why", not just "what"
3. **Test before commit**: At minimum, ensure build succeeds
4. **Push regularly**: `git push -u origin <branch-name>`

### .gitignore Rules

- `secrets.ini`: Contains credentials
- `.pio/`: PlatformIO build artifacts
- Build outputs: `.elf`, `.bin`, `.map`, etc.
- VSCode internals: `.vscode/ipch/`, `.vscode/c_cpp_properties.json`

## Integration with Home Assistant

### Discovery

Devices don't auto-discover. Manual MQTT sensor/switch configuration required.

### Example HA Configuration

```yaml
# configuration.yaml
switch:
  - platform: mqtt
    name: "Garage Relay 0"
    state_topic: "Devices/ESP32_ROOT/State"
    command_topic: "Devices/ESP32_ROOT/Cmd/Set"
    payload_on: '{"target_dev":"ESP32_ROOT","io_kind":"RELAY","io_id":0,"action":"ON"}'
    payload_off: '{"target_dev":"ESP32_ROOT","io_kind":"RELAY","io_id":0,"action":"OFF"}'
    value_template: '{{ value_json.state if value_json.io_id == 0 else states("switch.garage_relay_0") }}'
    optimistic: false
```

## Future Enhancement Ideas

1. **OTA Support**: Larger flash or external storage
2. **Web UI**: ESP32 web server for local config
3. **Mesh Integration**: Fully implement child node forwarding
4. **TLS/Encryption**: Secure MQTT with mbedTLS
5. **Authorization**: Role-based command filtering
6. **Metrics**: Uptime, memory usage, command stats
7. **Event Streaming**: Continuous input monitoring
8. **Home Assistant Discovery**: Auto-configure entities
9. **Backup/Restore**: Export/import full configuration
10. **Unit Tests**: Automated testing framework

## Quick Reference

### Build Commands

```bash
# Full build
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

### File Locations Reference

| Purpose | Location |
|---------|----------|
| Application entry | `src/main.c` |
| Parser logic | `components/parser/parser.c` |
| Router logic | `components/router/router.c` |
| Config storage | `components/config_store/config_store.c` |
| Relay driver | `components/relay_ctrl/relay_ctrl.c` |
| PWM driver | `components/pwm_ctrl/pwm_ctrl.c` |
| Input driver | `components/input_ctrl/input_ctrl.c` |
| WiFi management | `components/wifi_link/wifi_link.c` |
| MQTT client | `components/mqtt_link/mqtt_link.c` |
| Credentials | `secrets.ini` (gitignored) |
| Partition table | `partitions/esp32-2mb-noota.csv` |

---

**Last Updated**: 2025-11-13
**Project Version**: Initial development
**Maintained By**: TommyTurboo
**AI Assistant**: Optimized for Claude Code understanding
