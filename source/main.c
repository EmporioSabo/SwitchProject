#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <switch.h>

#include "config.h"
#include "MQTTClient.h"

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

    printf("=================================\n");
    printf(" Switch MQTT Telemetry v0.3\n");
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

    // Paho MQTT client objects — all static, no malloc
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

    printf("Press + to disconnect and exit\n");
    consoleUpdate(NULL);

    // Main loop
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus)
            break;

        // Let Paho handle keepalive (PINGREQ/PINGRESP)
        if (mqtt_connected)
            MQTTYield(&client, 100);

        consoleUpdate(NULL);
    }

    // Cleanup
    if (mqtt_connected)
        MQTTDisconnect(&client);
    NetworkDisconnect(&network);

    socketExit();
    consoleExit(NULL);
    return 0;
}
