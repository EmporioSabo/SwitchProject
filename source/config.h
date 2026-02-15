#ifndef CONFIG_H
#define CONFIG_H

// MQTT Broker configuration
#define MQTT_BROKER_IP      "192.168.1.229"  // TODO: set to your PC's local IP
#define MQTT_BROKER_PORT    1883
#define MQTT_CLIENT_ID      "switch-01"

// Telemetry settings
#define TELEMETRY_INTERVAL_MS 5000

// MQTT topic prefix
#define MQTT_TOPIC_PREFIX   "switch"

#endif // CONFIG_H
