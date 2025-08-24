# Excellence – ESP32 WiFi Mesh + MQTT

ESP32‑project met **ESP‑IDF + PlatformIO**. Root ↔ MQTT (Home Assistant), childs schakelen relais en sturen status terug.

## Quick start
```bash
# 1) Vereisten
#  - VS Code + PlatformIO extension
#  - Git

# 2) Clone
git clone https://github.com/TommyTurboo/Excellence.git
cd Excellence

# 3) Secrets invullen
copy secrets_template.ini secrets.ini   # (Windows)
# of: cp secrets_template.ini secrets.ini   # (macOS/Linux)
# Vul WiFi/MQTT in

# 4) Build & upload
pio run -e esp32dev -t upload
pio device monitor -b 115200
