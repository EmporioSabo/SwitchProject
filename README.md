# Switch MQTT Telemetry

Nintendo Switch homebrew app that publishes telemetry data (battery, temperatures, WiFi) via MQTT.

Built with [devkitPro](https://devkitpro.org/) / libnx, cross-compiled from a container.

## Prerequisites

- [Podman](https://podman.io/) (or Docker — edit `build.sh` accordingly)
- A hacked Nintendo Switch running Atmosphère + Homebrew Menu

The devkitPro toolchain runs inside a container, no local installation needed.

## Build

```bash
# First time: pulls the devkitpro/devkita64 image (~1 GB)
./build.sh

# Clean
./build.sh clean
```

Produces `switch-mqtt-telemetry.nro`.

## Deploy

Copy the `.nro` to your Switch's SD card:

```
sd:/switch/switch-mqtt-telemetry/switch-mqtt-telemetry.nro
```

Launch from the Homebrew Menu.

## Configuration

Edit `source/config.h` before building:

```c
#define MQTT_BROKER_IP      "192.168.1.100"  // your PC's local IP
#define MQTT_BROKER_PORT    1883
#define MQTT_CLIENT_ID      "switch-01"
#define TELEMETRY_INTERVAL_MS 5000
#define MQTT_TOPIC_PREFIX   "switch"
```

## Current status

- [x] Step 0 — devkitPro toolchain via container
- [x] Step 1 — Hello world: network init, IP display, clean exit
- [ ] Step 2 — Raw TCP connection to Mosquitto
- [ ] Step 3 — Paho MQTT Embedded C integration
- [ ] Step 4 — Sensor HAL (battery, temperatures, WiFi)
- [ ] Step 5 — Full telemetry loop (collect → JSON → publish)
- [ ] Step 6 — Grafana dashboard (Mosquitto + InfluxDB)
- [ ] Step 7 — Bidirectional MQTT, reconnection, QoS 1

## Project structure

```
SwitchProject/
├── Makefile              # devkitPro switch/application template
├── build.sh              # Podman wrapper for cross-compilation
├── Dockerfile.dev        # Container image reference
├── source/
│   ├── main.c            # Entry point: network init, console UI
│   └── config.h          # Centralized configuration
└── .gitignore
```
