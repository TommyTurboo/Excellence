# Project Excellence - ESP32 WiFi Mesh Systeem

## Projectoverzicht
Uniform codebase voor ESP32 boards met ESP-IDF WiFi Mesh protocol, geconfigureerd via berichten, met MQTT/Home Assistant integratie.

## Kernvereisten (door gebruiker opgegeven)

### 1. Uniform Code Platform
- Alle ESP32 boards draaien identieke code
- Backward compatibility tussen versies
- Versie tracking en broadcasting

### 2. Netwerk Architectuur
- ESP-IDF WiFi Mesh protocol
- Eén root node verbonden met WiFi-router
- Zelfherstellend netwerk (auto-healing bij wijzigingen)
- Multi-layer topologie

### 3. Dynamische Configuratie
- IO pin configuratie via gestructureerde berichten
- Support voor sensoren en actuatoren
- Configuratie bepaalt: input/output/PWM/andere signalen

### 4. Onboarding Process
1. Code upload
2. Fysieke installatie + power-on
3. Auto-join mesh netwerk (standaard naam: ESP32_[MAC])
4. Tijd synchronisatie ontvangen
5. Device naam toewijzen

### 5. Status Monitoring
- Periodieke status broadcasts naar root
- Root publiceert naar MQTT
- Offline detectie via retry mechanisme

### 6. Status Velden
- Software versie
- Timestamp (werkelijke tijd)
- Parent device name
- Layer nummer
- Event-driven updates (network changes, parent switch, layer change)

### 7. Home Assistant Weergave
- Tabel overzicht gesorteerd op layer
- Root bovenaan
- Per layer alfabetisch op device name

## Technische Beslissingen

**Tijd sync:** Request bij boot + periodieke updates (hybride aanpak)
**Offline detectie:** Mesh events + MQTT timeout (beide)
**Versie format:** Semantic versioning (#define VERSION "1.2.3")
**HA representatie:** Nodes als devices + centrale netwerk status tabel
**Architectuur:** Hybride - alleen `time_sync` als nieuwe component, rest geïntegreerd in bestaande code

---

# Implementatieplan

## Overzicht
Dit plan voegt toe: **software versie tracking**, **MQTT tijd synchronisatie**, **periodieke status broadcasts** en **offline detectie** aan het Excellence ESP32 mesh netwerk.

**Strategie:** Minimale nieuwe componenten - alleen `time_sync` als nieuwe component. Status broadcast en versie worden geïntegreerd in bestaande `mqtt_link`, `router` en `main.c`.

## 1. Nieuwe Componenten (Hybride Aanpak)

### 1.1 Time Sync Component
**Locatie:** `components/time_sync/`
- Root ontvangt tijd van MQTT (`Mesh/Time`)
- Nodes vragen tijd op via mesh request
- API: `time_sync_init()`, `time_sync_get_unix_time()`, `time_sync_is_synced()`
- **Reden voor aparte component:** Complexe tijd management logica met offset tracking

### 1.2 Version Header (geen component)
**Locatie:** `src/version.h` (in src directory)
```c
#define FW_VERSION "1.2.3"
```
- **Reden:** Eenvoudig, geen component overhead nodig

## 2. Wijzigingen Bestaande Componenten

### 2.1 mesh_link (backend_espmesh.c)
**Toevoegen:**
- `mesh_get_parent_name()` - parent device_name
- `mesh_get_layer()` - layer nummer
- Event callbacks: `on_layer_changed()`, `on_parent_changed()`

### 2.2 mqtt_link
**Toevoegen:**
- Subscribe `Mesh/Time` (root)
- Subscribe `Devices/+/Status` voor watchdog
- Offline detectie: 90s timeout (3x gemist)
- `mqtt_link_publish_time(unix_time)`
- **Status broadcast timer** (30s, geïntegreerd in mqtt_link)
- Network status tabel management
- `mqtt_link_trigger_status_broadcast()` - force immediate broadcast

### 2.3 router
**Toevoegen:**
- `ML_KIND_STATUS` message type
- `ML_KIND_TIME_REQUEST` / `ML_KIND_TIME_RESPONSE`
- `router_handle_status_broadcast()` - verwerk inkomende status van nodes
- `router_collect_status_data()` - verzamel lokale status info
- Retry logica (3x) voor status broadcasts

### 2.4 src/main.c
**Toevoegen:**
- Init call: `time_sync_init()`
- Status broadcast wordt gestart via `mqtt_link` (geen aparte init)
- Topology change handlers → `mqtt_link_trigger_status_broadcast()`

## 3. MQTT Topic Structuur

### Status per Node
**Topic:** `Devices/<device_name>/Status`
```json
{
  "dev": "ESP32_A",
  "version": "1.2.3",
  "timestamp": 1732873200,
  "parent": "ESP32_ROOT",
  "layer": 1,
  "uptime_s": 3600
}
```
**Retained:** true, **Interval:** 30s

### Network Status Tabel
**Topic:** `Mesh/Network/Status`
```json
{
  "timestamp": 1732873200,
  "nodes": [
    {"dev": "ESP32_ROOT", "version": "1.2.3", "layer": 0, "parent": null, "status": "online"},
    {"dev": "ESP32_A", "version": "1.2.3", "layer": 1, "parent": "ESP32_ROOT", "status": "online"}
  ]
}
```
**Sortering:** Layer (0 eerst), daarna alfabetisch

### Time Sync
**Topic:** `Mesh/Time`
```json
{"unix_time": 1732873200}
```

## 4. Implementatie Flow

### Software Versie
1. Define in `version.h`
2. Include in status broadcasts
3. Root logt warning bij major version mismatch

### Tijd Synchronisatie
**Root:**
1. Subscribe `Mesh/Time` topic
2. Update lokale tijd
3. Beantwoord mesh time requests van nodes

**Node:**
1. Bij boot: mesh request naar root
2. Bij reconnect: opnieuw tijd vragen
3. Lokale offset bijhouden

### Status Broadcasts
**Node (elke 30s):**
1. Verzamel: versie, timestamp, parent, layer
2. Mesh event naar root (3x retry)

**Root:**
1. Ontvang status
2. Update watchdog timer per device
3. Publish `Devices/<dev>/Status`
4. Update `Mesh/Network/Status` tabel

### Offline Detectie
**Mesh events:** Directe detectie bij disconnect
**MQTT timeout:** >90s geen status → offline

### Event-Driven Updates
Triggers voor immediate status broadcast:
- Layer change
- Parent change
- Root change

## 5. Implementatie Volgorde (Hybride Aanpak)

### Fase 1: Basis Infrastructure
1. Voeg `src/version.h` toe met FW_VERSION
2. Maak `time_sync` component (alleen deze als nieuwe component)
3. Mesh API uitbreidingen: `mesh_get_parent_name()`, `mesh_get_layer()`
4. Test: tijd sync root → nodes

### Fase 2: Status System (geïntegreerd in bestaande componenten)
1. Router: handlers voor `ML_KIND_STATUS`, `router_collect_status_data()`
2. MQTT_link: timer (30s), `mqtt_link_trigger_status_broadcast()`
3. MQTT topics implementeren: `Devices/<dev>/Status`
4. Test: periodieke status broadcasts

### Fase 3: Monitoring
1. MQTT_link: watchdog voor offline detectie (90s)
2. Network status tabel in mqtt_link
3. Topology event handlers in main.c
4. Test: offline detectie, topology changes

### Fase 4: Home Assistant
1. MQTT discovery voor node devices
2. Network tabel sensor configuratie
3. Test: HA dashboard met sortering

## 6. Kritieke Bestanden

| Bestand | Wijzigingen |
|---------|-------------|
| `src/main.c` | Init nieuwe modules, topology handlers |
| `components/mesh_link/backends/backend_espmesh.c` | Parent/layer tracking, events |
| `components/router/router.c` | STATUS en TIME handlers |
| `components/mqtt_link/mqtt_link.c` | Time subscription, watchdog, tabel |
| `components/mesh_link/include/mesh_link.h` | Nieuwe enums en API |

## 7. Testing Checklist

- [ ] Versie in status berichten
- [ ] Tijd sync: root → nodes
- [ ] Status broadcast elke 30s
- [ ] Topology change → immediate update
- [ ] Offline detectie (<90s)
- [ ] Network tabel sortering (layer + alfabet)
- [ ] Root reboot → nodes reconnect
- [ ] HA dashboard weergave