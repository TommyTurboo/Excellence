# Project Excellence - Node-RED & Home Assistant Specificatie

## Overzicht
Dit document beschrijft de automation logica (Node-RED), gebruikersinterface (Home Assistant), en use cases voor het Excellence systeem.

**Verantwoordelijkheden:**
- **Home Assistant**: UI, configuratie, state visualisatie
- **Node-RED**: Automation logica, scheduling, multi-sensor fusion

**Data flow:**
```
User → HA → Node-RED → MQTT → ESP32 (command)
ESP32 → MQTT → HA (state, rechtstreeks)
```

## Ontwerpprincipes

### 1. State Management
- **Single source of truth**: MQTT retained messages
- **HA leest rechtstreeks van MQTT** (niet via Node-RED)
- **Node-RED is stateless** waar mogelijk (logica gebaseerd op events)

### 2. Separation of Concerns
- **HA**: Welke entities/devices bestaan, hoe ze eruitzien (UI)
- **Node-RED**: Wanneer en hoe entities geschakeld worden (logica)
- **ESP32**: Fysieke uitvoering + lokale safety

### 3. Configurability
- **Tijden, duur, limieten**: Instelbaar in HA (input_number, input_datetime)
- **Flows**: Modulair, makkelijk aan/uit te zetten
- **Prioriteiten**: Duidelijk gedocumenteerd in flows

## Use Cases

---

## Use Case 1: Ventielsturing

### Beschrijving
Besturing van een ventiel voor regenwaterput. **Kritisch**: moet safe state hebben (ventiel dicht) bij problemen.

### Manuele Bediening

**Home Assistant Entities:**
```yaml
# Ventiel schakelaar
switch.ventiel_tuin:
  name: "Ventiel Regenwaterput"
  platform: mqtt
  state_topic: "Devices/ESP32_TUIN/State"
  command_topic: "Devices/ESP32_TUIN/Cmd/Set"
  value_template: "{{ value_json.relay_0 }}"
  payload_on: '{"target_dev":"ESP32_TUIN","io_kind":"RELAY","io_id":0,"action":"ON","max_runtime_sec":3600,"safe_state":"OFF"}'
  payload_off: '{"target_dev":"ESP32_TUIN","io_kind":"RELAY","io_id":0,"action":"OFF"}'

# Tijdsduur slider
input_number.ventiel_duur:
  name: "Ventiel Duur (minuten)"
  min: 0
  max: 60
  step: 5
  initial: 30
  unit_of_measurement: "min"
```

**Node-RED Flow (Manueel):**
```
[HA: switch.ventiel_tuin ON] → [Function: Build Command] → [MQTT Out: Devices/ESP32_TUIN/Cmd/Set]
                                       ↓
                              max_runtime_sec = input_number.ventiel_duur * 60
                              safe_state = "OFF"
```

**Function node code:**
```javascript
// Input: HA switch toggle event
const duur_min = global.get("homeassistant.input_number.ventiel_duur.state");
const max_runtime_sec = duur_min * 60;

msg.payload = {
    target_dev: "ESP32_TUIN",
    io_kind: "RELAY",
    io_id: 0,
    action: msg.payload === "ON" ? "ON" : "OFF",
    max_runtime_sec: max_runtime_sec,
    safe_state: "OFF"
};

return msg;
```

### Automatische Bediening

**Configuratie Entities:**
```yaml
# Schema aan/uit
input_boolean.ventiel_schema_actief:
  name: "Ventiel Schema Actief"

# Tijdstippen (meerdere mogelijk)
input_datetime.ventiel_tijd_1:
  name: "Ventiel Tijd 1"
  has_time: true
  has_date: false

input_datetime.ventiel_tijd_2:
  name: "Ventiel Tijd 2"
  has_time: true
  has_date: false

# Duur per activatie
input_number.ventiel_schema_duur:
  name: "Ventiel Schema Duur"
  min: 5
  max: 60
  step: 5
  initial: 20
  unit_of_measurement: "min"
```

**Node-RED Flow (Automatisch):**
```
[Inject: Cron "0 * * * *"] → [Function: Check Schema] → [Switch: Schema Actief?] → [MQTT Out]
                                        ↓
                              Haal huidige tijd
                              Check of tijd == ventiel_tijd_1/2
                              Ja? → Build command
```

**Check Schema function:**
```javascript
const schema_actief = global.get("homeassistant.input_boolean.ventiel_schema_actief.state");
if (schema_actief !== "on") {
    return null; // Stop flow
}

const now = new Date();
const tijd_1 = global.get("homeassistant.input_datetime.ventiel_tijd_1.state");
const tijd_2 = global.get("homeassistant.input_datetime.ventiel_tijd_2.state");

// Parse tijd_1 (format: "HH:MM")
const [uur_1, min_1] = tijd_1.split(":").map(Number);

if (now.getHours() === uur_1 && now.getMinutes() === min_1) {
    const duur = global.get("homeassistant.input_number.ventiel_schema_duur.state");

    msg.payload = {
        target_dev: "ESP32_TUIN",
        io_kind: "RELAY",
        io_id: 0,
        action: "ON",
        max_runtime_sec: duur * 60,
        safe_state: "OFF"
    };
    return msg;
}

// Check tijd_2 op zelfde manier...
return null;
```

### State Weergave

**HA State Topic:**
```yaml
sensor.ventiel_tuin_state:
  name: "Ventiel Status"
  platform: mqtt
  state_topic: "Devices/ESP32_TUIN/State"
  value_template: "{{ value_json.relay_0 | default('unknown') }}"

# Extra: Watchdog status
sensor.ventiel_tuin_watchdog:
  name: "Ventiel Watchdog"
  platform: mqtt
  state_topic: "Devices/ESP32_TUIN/State"
  value_template: >
    {% if value_json.watchdog_active %}
      Actief (nog {{ value_json.remaining_sec }} sec)
    {% else %}
      Inactief
    {% endif %}
```

### Safety Features

**Watchdog:**
- ESP32 heeft `max_runtime_sec` (bijv. 3600s = 1 uur)
- Na timeout → auto OFF (safe state)
- Bij mesh disconnect → direct OFF

**Node-RED Alert:**
```
[MQTT In: Devices/ESP32_TUIN/State] → [Filter: reason=="watchdog_timeout"] → [HA Notification]
```

**Alert message:**
```javascript
if (msg.payload.reason === "watchdog_timeout") {
    msg.payload = {
        title: "Ventiel Safety Timeout",
        message: "Ventiel is automatisch gesloten na max runtime."
    };
    return msg;
}
```

---

## Use Case 2: Verlichting Binnen

### Beschrijving
LED verlichting met PWM dimming. Automatische sturing via PIR sensors en drukknoppen. **Drukknop heeft prioriteit** over PIR.

### Manuele Bediening

**Home Assistant Entities:**
```yaml
light.woonkamer:
  name: "Verlichting Woonkamer"
  platform: mqtt
  state_topic: "Devices/ESP32_BINNEN/State"
  command_topic: "Devices/ESP32_BINNEN/Cmd/Set"
  brightness_state_topic: "Devices/ESP32_BINNEN/State"
  brightness_command_topic: "Devices/ESP32_BINNEN/Cmd/Set"
  brightness_scale: 100
  on_command_type: "brightness"
  payload_template: >
    {
      "target_dev":"ESP32_BINNEN",
      "io_kind":"PWM",
      "io_id":0,
      "action":"SET",
      "brightness_pct":{{ brightness }}
    }
  payload_off: >
    {"target_dev":"ESP32_BINNEN","io_kind":"PWM","io_id":0,"action":"OFF"}
```

### Automatische Bediening - PIR Sensors

**Configuratie:**
```yaml
input_number.verlichting_binnen_duur_pir:
  name: "PIR Verlichting Duur"
  min: 1
  max: 30
  step: 1
  initial: 5
  unit_of_measurement: "min"

input_number.verlichting_binnen_helderheid_pir:
  name: "PIR Verlichting Helderheid"
  min: 10
  max: 100
  step: 10
  initial: 80
  unit_of_measurement: "%"

binary_sensor.pir_woonkamer:
  platform: mqtt
  state_topic: "Devices/ESP32_BINNEN/State"
  value_template: "{{ value_json.input_2 }}"
  device_class: motion
```

**Node-RED Flow (PIR):**
```
[MQTT In: PIR detectie] → [Filter: state==ON] → [Check: Drukknop override?] → [Delay: Anti-flicker] → [MQTT Out: Light ON]
                                                           ↓
                                                    Als drukknop actief: STOP
                                                    Anders: doorsturen
```

**PIR Function:**
```javascript
// Context: onthoud drukknop override
const override_actief = context.get("drukknop_override") || false;
const override_end = context.get("drukknop_override_end") || 0;

if (override_actief && Date.now() < override_end) {
    // Drukknop heeft prioriteit, negeer PIR
    return null;
}

const helderheid = global.get("homeassistant.input_number.verlichting_binnen_helderheid_pir.state");
const duur_min = global.get("homeassistant.input_number.verlichting_binnen_duur_pir.state");

msg.payload = {
    target_dev: "ESP32_BINNEN",
    io_kind: "PWM",
    io_id: 0,
    action: "SET",
    brightness_pct: helderheid,
    max_runtime_sec: duur_min * 60,
    safe_state: "OFF" // Optioneel voor verlichting
};

return msg;
```

### Automatische Bediening - Drukknoppen

**Configuratie:**
```yaml
input_number.verlichting_binnen_duur_drukknop:
  name: "Drukknop Verlichting Duur"
  min: 5
  max: 120
  step: 5
  initial: 30
  unit_of_measurement: "min"

input_number.verlichting_binnen_waarschuwing_voor:
  name: "Waarschuwing X min voor uit"
  min: 1
  max: 10
  step: 1
  initial: 5
  unit_of_measurement: "min"

binary_sensor.drukknop_woonkamer:
  platform: mqtt
  state_topic: "Devices/ESP32_BINNEN/State"
  value_template: "{{ value_json.input_0 }}"
  device_class: none
```

**Node-RED Flow (Drukknop):**
```
[MQTT In: Drukknop] → [Filter: RISING edge] → [Function: Light ON + Set Override] → [MQTT Out]
                                                          ↓
                                              Set context: drukknop_override = true
                                              Schedule: Warning na (duur - warning_tijd)
                                              Schedule: OFF na duur
```

**Drukknop Function:**
```javascript
const duur_min = global.get("homeassistant.input_number.verlichting_binnen_duur_drukknop.state");
const warning_min = global.get("homeassistant.input_number.verlichting_binnen_waarschuwing_voor.state");

// Zet override (PIR wordt genegeerd)
context.set("drukknop_override", true);
context.set("drukknop_override_end", Date.now() + duur_min * 60000);

// Schedule warning (3x dim up/down)
const warning_delay = (duur_min - warning_min) * 60000;
setTimeout(() => {
    // Stuur dim sequence
    send_dim_warning("ESP32_BINNEN", 0);
}, warning_delay);

// Schedule OFF
setTimeout(() => {
    // Graceful dim naar uit (over 10 seconden)
    send_dim_off("ESP32_BINNEN", 0, 10);
    context.set("drukknop_override", false);
}, duur_min * 60000);

// Direct: Light ON
msg.payload = {
    target_dev: "ESP32_BINNEN",
    io_kind: "PWM",
    io_id: 0,
    action: "SET",
    brightness_pct: 100
};

return msg;
```

**Warning Sequence (3x dimmen):**
```javascript
function send_dim_warning(device, io_id) {
    const sequence = [
        {brightness: 30, delay: 0},
        {brightness: 100, delay: 500},
        {brightness: 30, delay: 1000},
        {brightness: 100, delay: 1500},
        {brightness: 30, delay: 2000},
        {brightness: 100, delay: 2500}
    ];

    sequence.forEach(step => {
        setTimeout(() => {
            mqtt_publish({
                target_dev: device,
                io_kind: "PWM",
                io_id: io_id,
                action: "SET",
                brightness_pct: step.brightness
            });
        }, step.delay);
    });
}
```

**Graceful Dim Off:**
```javascript
function send_dim_off(device, io_id, duration_sec) {
    // Stuur fade command naar ESP32
    mqtt_publish({
        target_dev: device,
        io_kind: "PWM",
        io_id: io_id,
        action: "FADE",
        brightness_pct: 0,
        ramp_ms: duration_sec * 1000
    });
}
```

**Drukknop Override Extend:**
Als drukknop opnieuw ingedrukt tijdens actieve override:
```javascript
// In drukknop handler
const override_actief = context.get("drukknop_override");

if (override_actief) {
    // Extend met nog eens X minuten
    const extend_min = 30; // Of instelbaar
    const new_end = Date.now() + extend_min * 60000;
    context.set("drukknop_override_end", new_end);

    // Cancel oude timers, maak nieuwe
    // ...
}
```

---

## Use Case 3: Verlichting Buiten

### Beschrijving
Tuin verlichting met tijd-gebaseerde auto-off (bijv. 22:00). Warning sequence bij auto-off. Extend optie voor gebruiker.

### Manuele Bediening

**Entities:**
```yaml
light.tuin:
  name: "Verlichting Tuin"
  platform: mqtt
  # Zelfde als binnen...

input_datetime.verlichting_buiten_uit_tijd:
  name: "Verlichting Buiten Uit Tijd"
  has_time: true
  has_date: false
  initial: "22:00"

input_number.verlichting_buiten_extend_duur:
  name: "Extend Duur (uur)"
  min: 1
  max: 4
  step: 1
  initial: 2
  unit_of_measurement: "uur"
```

**Node-RED Flow (Manueel met Tijdslimiet):**
```
[HA: light.tuin ON] → [Function: Check Tijd] → [MQTT Out: Light ON]
                               ↓
                    Als NA uit_tijd → Schedule warning + auto-off
                    Anders → Gewoon ON, schedule check voor uit_tijd
```

**Check Tijd Function:**
```javascript
const uit_tijd = global.get("homeassistant.input_datetime.verlichting_buiten_uit_tijd.state");
const [uit_uur, uit_min] = uit_tijd.split(":").map(Number);

const now = new Date();
const uit_datetime = new Date();
uit_datetime.setHours(uit_uur, uit_min, 0);

// Light ON command
msg.payload = {
    target_dev: "ESP32_TUIN",
    io_kind: "PWM",
    io_id: 1,
    action: "SET",
    brightness_pct: global.get("requested_brightness") || 100
};

// Schedule warning + auto-off
let delay_ms;
if (now > uit_datetime) {
    // Al voorbij uit_tijd, direct warning (over 2 min)
    delay_ms = 2 * 60000;
} else {
    // Wacht tot uit_tijd
    delay_ms = uit_datetime - now;
}

// Save timer ID voor cancel (bij extend)
const timer_id = setTimeout(() => {
    send_warning_sequence("ESP32_TUIN", 1); // 3x aan/uit knipperen

    // Na warning: grace period (2 min)
    setTimeout(() => {
        // Check of extend is gevraagd
        if (!context.get("extend_requested")) {
            send_off_command("ESP32_TUIN", 1);
        } else {
            // Reset extend flag, schedule nieuwe auto-off
            context.set("extend_requested", false);
            const extend_uur = global.get("homeassistant.input_number.verlichting_buiten_extend_duur.state");
            schedule_auto_off(extend_uur * 3600000);
        }
    }, 2 * 60000);
}, delay_ms);

context.set("auto_off_timer", timer_id);

return msg;
```

**Warning Sequence (3x Knipperen):**
```javascript
function send_warning_sequence(device, io_id) {
    const sequence = [
        {state: "OFF", delay: 0},
        {state: "ON", delay: 500},
        {state: "OFF", delay: 1000},
        {state: "ON", delay: 1500},
        {state: "OFF", delay: 2000},
        {state: "ON", delay: 2500}
    ];

    sequence.forEach(step => {
        setTimeout(() => {
            mqtt_publish({
                target_dev: device,
                io_kind: "PWM",
                io_id: io_id,
                action: step.state === "ON" ? "SET" : "OFF",
                brightness_pct: 100
            });
        }, step.delay);
    });
}
```

**Extend Handling:**
```yaml
# HA: Button entity voor extend
input_button.verlichting_buiten_extend:
  name: "Verleng Verlichting Buiten"
```

**Node-RED Flow (Extend):**
```
[HA: input_button.extend pressed] → [Function: Set Extend Flag] → [HA Notification: "Verlengd met X uur"]

# Function:
context.set("extend_requested", true);
```

### Automatische Bediening - Thuiskomst

**Configuratie:**
```yaml
input_boolean.verlichting_buiten_thuiskomst:
  name: "Verlichting Bij Thuiskomst"
  initial: true

input_number.verlichting_buiten_thuiskomst_duur:
  name: "Thuiskomst Verlichting Duur"
  min: 5
  max: 60
  step: 5
  initial: 15
  unit_of_measurement: "min"
```

**Thuiskomst Detectie (meerdere opties):**

**Optie A: Smartphone Proximity (Home Assistant)**
```yaml
# HA Device Tracker
device_tracker.iphone_tom:
  platform: icloud
  # Of via companion app

# Automation trigger in HA → Node-RED webhook
automation:
  trigger:
    - platform: state
      entity_id: device_tracker.iphone_tom
      to: "home"
  action:
    - service: rest_command.nodered_thuiskomst
```

**Optie B: Poort Sensor**
```yaml
binary_sensor.poort_open:
  platform: mqtt
  state_topic: "Devices/ESP32_POORT/State"
  value_template: "{{ value_json.input_1 }}"
```

**Optie C: Wagen Detectie (ultrasone/radar sensor)**
```yaml
binary_sensor.wagen_geparkeerd:
  platform: mqtt
  state_topic: "Devices/ESP32_GARAGE/State"
  value_template: "{{ value_json.wagen_detected }}"
```

**Node-RED Flow (Thuiskomst):**
```
[Trigger: Poort OPEN / Device Home / Wagen Detected]
    ↓
[Filter: Is het na zonsondergang?]
    ↓
[Check: Verlichting buiten al aan?]
    ↓
[MQTT Out: Light ON voor X min]
```

**Thuiskomst Function:**
```javascript
// Input: trigger event
const feature_enabled = global.get("homeassistant.input_boolean.verlichting_buiten_thuiskomst.state");
if (feature_enabled !== "on") return null;

// Check zonsondergang
const sun_elevation = global.get("homeassistant.sun.sun.attributes.elevation");
if (sun_elevation > 0) {
    // Nog licht, geen verlichting nodig
    return null;
}

// Check of licht al aan is
const current_state = global.get("mqtt.Devices.ESP32_TUIN.State.pwm_1");
if (current_state === "ON") {
    // Al aan, niks doen
    return null;
}

const duur_min = global.get("homeassistant.input_number.verlichting_buiten_thuiskomst_duur.state");

msg.payload = {
    target_dev: "ESP32_TUIN",
    io_kind: "PWM",
    io_id: 1,
    action: "SET",
    brightness_pct: 80,
    max_runtime_sec: duur_min * 60
};

return msg;
```

---

## Monitoring & Diagnostics

### Network Status Dashboard

**HA Sensor:**
```yaml
sensor.mesh_network_status:
  name: "Mesh Netwerk Status"
  platform: mqtt
  state_topic: "Mesh/Network/Status"
  value_template: "{{ value_json.nodes | length }} nodes"
  json_attributes_topic: "Mesh/Network/Status"
  json_attributes_template: "{{ value_json.nodes | tojson }}"
```

**Lovelace Card (Custom):**
```yaml
type: custom:auto-entities
card:
  type: entities
  title: "Mesh Netwerk"
filter:
  template: |
    {% for node in state_attr('sensor.mesh_network_status', 'nodes') | sort(attribute='layer') %}
      {{
        {
          'entity': 'sensor.mesh_network_status',
          'name': node.dev,
          'secondary_info': 'Layer ' + node.layer|string + ' | Parent: ' + (node.parent or 'None') + ' | v' + node.version,
          'icon': 'mdi:router-wireless' if node.layer == 0 else 'mdi:access-point'
        }
      }},
    {% endfor %}
```

### Alerts & Notifications

**Node Offline Alert:**
```
[MQTT In: Devices/+/Status] → [Filter: status==offline] → [HA Notification]
```

**Watchdog Trigger Alert:**
```
[MQTT In: Devices/+/State] → [Filter: reason==watchdog_timeout] → [HA Notification]
```

**Version Mismatch Warning:**
```
[MQTT In: Mesh/Network/Status] → [Function: Check Versions] → [HA Persistent Notification]
```

---

## Implementatie Volgorde

### Fase 1: Basis Setup (Week 1)
- [ ] MQTT Broker configuratie
- [ ] Home Assistant MQTT integratie
- [ ] Node-RED installatie + HA nodes plugin
- [ ] Test: MQTT pub/sub tussen HA ↔ Node-RED ↔ ESP32

### Fase 2: Ventiel Use Case (Week 2)
- [ ] HA entities (switch, sliders)
- [ ] Node-RED: Manuele flow
- [ ] Node-RED: Automatisch schema flow
- [ ] Test: Manueel + automatisch, watchdog trigger

### Fase 3: Verlichting Binnen (Week 3)
- [ ] HA entities (light, config)
- [ ] Node-RED: PIR flow
- [ ] Node-RED: Drukknop flow (prioriteit + override)
- [ ] Node-RED: Warning sequence
- [ ] Test: PIR → light, drukknop override, warning

### Fase 4: Verlichting Buiten (Week 4)
- [ ] HA entities (light, tijd config, extend button)
- [ ] Node-RED: Tijdslimiet flow
- [ ] Node-RED: Warning + extend logica
- [ ] Node-RED: Thuiskomst detectie (kies methode)
- [ ] Test: Tijd-based auto-off, extend, thuiskomst

### Fase 5: Monitoring & Polish (Week 5)
- [ ] Network status dashboard
- [ ] Alerts & notifications
- [ ] Lovelace UI polish
- [ ] Documentatie (gebruikershandleiding)

## Testing Checklist

**Ventiel:**
- [ ] Manueel ON/OFF werkt
- [ ] Slider wijzigen → nieuwe max_runtime
- [ ] Schema trigger op juiste tijd
- [ ] Watchdog timeout → ventiel sluit
- [ ] Mesh disconnect → ventiel sluit (safe state)

**Verlichting Binnen:**
- [ ] Manueel dimmen werkt (0-100%)
- [ ] PIR detectie → light ON
- [ ] Drukknop → light ON + PIR override
- [ ] Drukknop extend werkt
- [ ] Warning sequence (3x dim) werkt
- [ ] Graceful fade naar uit

**Verlichting Buiten:**
- [ ] Manueel ON werkt
- [ ] Auto-off bij 22:00 (of ingestelde tijd)
- [ ] Warning (3x knipperen) werkt
- [ ] Extend button → +2 uur
- [ ] Thuiskomst → light ON (na zonsondergang)

**Monitoring:**
- [ ] Network status tabel toont alle nodes
- [ ] Sortering: layer, dan alfabetisch
- [ ] Node offline → HA notification
- [ ] Watchdog trigger → HA notification

## Toekomstige Uitbreidingen

1. **Voice Control**: Alexa/Google Home integratie via HA
2. **Scenes**: Voorgedefinieerde licht scenes (avond, film, etc.)
3. **Presence Simulation**: Willekeurige verlichting tijdens vakantie
4. **Energy Monitoring**: Power consumption tracking + grafieken
5. **Advanced Scheduling**: Astro-based (zonsopgang/ondergang offsets)
6. **Multi-zone**: Verschillende regels per kamer/zone
