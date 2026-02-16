#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <switch.h>

#include "config.h"
#include "MQTTClient.h"
#include "hal_battery.h"
#include "hal_temperature.h"
#include "hal_wifi.h"

/* Charger type as human-readable string */
static const char *charger_type_str(PsmChargerType type)
{
    switch (type) {
    case PsmChargerType_Unconnected:  return "Unplugged";
    case PsmChargerType_EnoughPower:  return "Charging";
    case PsmChargerType_LowPower:     return "Low Power";
    case PsmChargerType_NotSupported: return "Unsupported";
    default:                          return "Unknown";
    }
}

int main(int argc, char* argv[])
{
    consoleInit(NULL);

    // Configure input
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    // Initialize network stack
    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        printf("socketInitializeDefault() failed: 0x%x\n", rc);
    }

    // Initialize sensor HAL modules
    hal_battery_init();
    hal_temperature_init();
    hal_wifi_init();

    printf("=================================\n");
    printf(" Switch MQTT Telemetry v0.4\n");
    printf("=================================\n\n");

    if (R_SUCCEEDED(rc)) {
        struct in_addr addr;
        addr.s_addr = gethostid();
        printf("Switch IP : %s\n", inet_ntoa(addr));
    } else {
        printf("Switch IP : unavailable (network error)\n");
    }

    printf("Broker    : %s:%d\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);
    printf("Client ID : %s\n\n", MQTT_CLIENT_ID);
    consoleUpdate(NULL);

    // Paho MQTT client objects
    Network network;
    MQTTClient client;
    unsigned char sendbuf[256];
    unsigned char readbuf[256];
    int mqtt_connected = 0;

    if (R_SUCCEEDED(rc)) {
        printf("--- MQTT Connection Sequence ---\n\n");

        // Step 1: TCP connect
        printf("1. Connecting to %s:%d...\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);
        consoleUpdate(NULL);

        NetworkInit(&network);
        int ret = NetworkConnect(&network, MQTT_BROKER_IP, MQTT_BROKER_PORT);
        if (ret < 0) {
            printf("   [FAILED] TCP connection\n");
            printf("   Make sure Mosquitto is running!\n\n");
        } else {
            printf("   [OK] TCP connected\n\n");
            consoleUpdate(NULL);

            // Step 2: MQTT CONNECT via Paho
            printf("2. Sending MQTT CONNECT...\n");
            consoleUpdate(NULL);

            MQTTClientInit(&client, &network, 5000,
                           sendbuf, sizeof(sendbuf),
                           readbuf, sizeof(readbuf));

            MQTTPacket_connectData opts = MQTTPacket_connectData_initializer;
            opts.MQTTVersion = 4;
            opts.clientID.cstring = MQTT_CLIENT_ID;
            opts.keepAliveInterval = 60;
            opts.cleansession = 1;

            ret = MQTTConnect(&client, &opts);
            if (ret != SUCCESS) {
                printf("   [FAILED] MQTT CONNECT (rc=%d)\n\n", ret);
            } else {
                printf("   [OK] Connection accepted!\n\n");
                consoleUpdate(NULL);
                mqtt_connected = 1;

                // Step 3: Publish test message
                char topic[64];
                snprintf(topic, sizeof(topic), "%s/status", MQTT_TOPIC_PREFIX);

                printf("3. Publishing to %s...\n", topic);
                consoleUpdate(NULL);

                MQTTMessage msg;
                msg.qos = QOS0;
                msg.retained = 0;
                msg.dup = 0;
                msg.id = 0;
                msg.payload = "online";
                msg.payloadlen = 6;

                ret = MQTTPublish(&client, topic, &msg);
                if (ret != SUCCESS) {
                    printf("   [FAILED] PUBLISH (rc=%d)\n\n", ret);
                } else {
                    printf("   [OK] Published: \"online\"\n\n");
                }
            }
        }

        consoleUpdate(NULL);
    }

    printf("Press + to disconnect and exit\n\n");
    consoleUpdate(NULL);

    /*
     * Remember the cursor position so we can overwrite the sensor
     * readings each loop iteration. The console is 80 columns wide
     * and we print a fixed number of lines.
     */
    int sensor_line = 0;

    // Main loop — update sensor display every iteration (~60 fps)
    u64 last_update = 0;

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus)
            break;

        // Let Paho handle keepalive
        if (mqtt_connected)
            MQTTYield(&client, 10);

        // Update sensor display roughly once per second
        u64 now = armGetSystemTick();
        u64 freq = armGetSystemTickFreq();
        if (now - last_update >= freq) {
            last_update = now;

            // Move cursor back to sensor display area
            if (sensor_line > 0)
                printf("\x1b[%dA", sensor_line);

            sensor_line = 0;

            printf("=== Sensor Readings ===\n");
            sensor_line++;

            // Battery
            hal_battery_reading_t bat;
            if (R_SUCCEEDED(hal_battery_read(&bat))) {
                printf("Battery : %u%% | %u mV | %dC | %s   \n",
                       bat.percentage, bat.voltage_mv,
                       bat.temperature_c, charger_type_str(bat.charger_type));
            } else {
                printf("Battery : read error                        \n");
            }
            sensor_line++;

            // Temperature
            hal_temperature_reading_t temp;
            if (R_SUCCEEDED(hal_temperature_read(&temp))) {
                printf("Temp    : SoC %dC | PCB %dC               \n",
                       temp.soc_celsius, temp.pcb_celsius);
            } else {
                printf("Temp    : read error                        \n");
            }
            sensor_line++;

            // WiFi
            hal_wifi_reading_t wifi;
            if (R_SUCCEEDED(hal_wifi_read(&wifi))) {
                if (wifi.connected) {
                    struct in_addr addr;
                    addr.s_addr = wifi.ip_addr;
                    if (wifi.rssi_dbm != 0)
                        printf("WiFi    : %d dBm | %s              \n",
                               wifi.rssi_dbm, inet_ntoa(addr));
                    else
                        printf("WiFi    : %u/3 bars | %s            \n",
                               wifi.signal_bars, inet_ntoa(addr));
                } else {
                    printf("WiFi    : disconnected                      \n");
                }
            } else {
                printf("WiFi    : read error                        \n");
            }
            sensor_line++;

            consoleUpdate(NULL);
        }
    }

    // Cleanup
    if (mqtt_connected)
        MQTTDisconnect(&client);
    NetworkDisconnect(&network);

    hal_wifi_exit();
    hal_temperature_exit();
    hal_battery_exit();

    socketExit();
    consoleExit(NULL);
    return 0;
}
