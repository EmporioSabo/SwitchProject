#ifndef CONFIG_H
#define CONFIG_H

// MQTT Broker configuration
#define MQTT_BROKER_IP      "192.168.1.229"  // TODO: set to your PC's local IP
#define MQTT_BROKER_PORT    1883
#define MQTT_CLIENT_ID      "switch-01"

// Telemetry publishing interval (consumer thread)
#define TELEMETRY_INTERVAL_MS 5000

// MQTT topic prefix
#define MQTT_TOPIC_PREFIX   "switch"

// MQTT topics
#define MQTT_TELEMETRY_TOPIC  "switch/telemetry"
#define MQTT_CMD_TOPIC        "switch/cmd"
#define MQTT_RESPONSE_TOPIC   "switch/response"

// Per-sensor polling intervals (producer thread)
#define SENSOR_POLL_BATTERY_MS   30000   // Battery changes slowly
#define SENSOR_POLL_TEMP_MS      10000   // Moderate — catch thermal spikes
#define SENSOR_POLL_WIFI_MS       5000   // Fast — detect disconnects quickly

// MQTT reconnection (exponential backoff)
#define MQTT_RECONNECT_DELAY_MS   1000   // Initial retry delay
#define MQTT_RECONNECT_MAX_MS    30000   // Cap at 30 seconds

// MQTTYield timeout per main loop iteration (ms)
#define MQTT_YIELD_MS             10

#endif // CONFIG_H
