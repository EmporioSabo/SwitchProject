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

## Monitoring Stack (Step 6)

A pre-configured Grafana dashboard visualizes telemetry in real time.

```
Switch ──MQTT──▶ Mosquitto :1883 ──▶ Telegraf ──▶ InfluxDB 2.x :8086 ──▶ Grafana :3000
```

### Quick start

```bash
cd monitoring
podman-compose up -d    # or docker-compose up -d
```

This spins up four containers:

| Service | Port | Purpose |
|---------|------|---------|
| Mosquitto | 1883 | MQTT broker (replaces standalone Mosquitto) |
| InfluxDB | 8086 | Time-series database |
| Telegraf | — | Bridges MQTT → InfluxDB |
| Grafana | 3000 | Dashboard UI |

### Access Grafana

Open [http://localhost:3000](http://localhost:3000) and log in with `admin` / `admin`.

The **Switch Telemetry** dashboard is pre-provisioned with 6 panels:
- Battery level gauge (with thresholds)
- Battery voltage over time
- SoC and PCB temperatures over time
- WiFi signal strength (dBm)
- Charging status
- WiFi connection info

### Tear down

```bash
cd monitoring
podman-compose down             # stop containers
podman-compose down -v          # stop + delete InfluxDB data
```

## Current status

- [x] Step 0 — devkitPro toolchain via container
- [x] Step 1 — Hello world: network init, IP display, clean exit
- [x] Step 2 — Raw TCP connection to Mosquitto
- [x] Step 3 — Paho MQTT Embedded C integration
- [x] Step 4 — Sensor HAL (battery, temperatures, WiFi)
- [x] Step 5 — Full telemetry loop (collect → JSON → publish)
- [x] Step 6 — Grafana dashboard (Mosquitto + InfluxDB)
- [ ] Step 7 — Bidirectional MQTT, reconnection, QoS 1

## Project structure

```
SwitchProject/
├── Makefile              # devkitPro switch/application template
├── build.sh              # Podman wrapper for cross-compilation
├── Dockerfile.dev        # Container image reference
├── source/
│   ├── main.c            # Entry point: threads, MQTT, UI
│   ├── telemetry.c       # Producer thread + JSON builder
│   ├── telemetry.h       # Shared buffer, MQTT state
│   ├── config.h          # Centralized configuration
│   ├── mqtt_switch.c     # Paho platform layer (Switch sockets)
│   ├── mqtt_switch.h     # Network/Timer types for Paho
│   └── hal/              # Sensor HAL modules
│       ├── hal_battery.c/h
│       ├── hal_temperature.c/h
│       └── hal_wifi.c/h
├── lib/
│   ├── paho.mqtt.embedded-c/  # Paho MQTT Embedded C
│   └── cJSON/                 # JSON serialization
├── monitoring/                # Grafana stack (Step 6)
│   ├── docker-compose.yml
│   ├── mosquitto/mosquitto.conf
│   ├── telegraf/telegraf.conf
│   └── grafana/               # Provisioned datasource + dashboard
└── .gitignore
```
