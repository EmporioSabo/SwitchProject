#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <switch.h>

#include "config.h"
#include "mqtt_raw.h"

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
    printf(" Switch MQTT Telemetry v0.2\n");
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

    // MQTT connection sequence
    int sockfd = -1;
    int mqtt_connected = 0;

    if (R_SUCCEEDED(rc)) {
        printf("--- MQTT Connection Sequence ---\n\n");

        // Step 1: TCP connect
        printf("1. Connecting to %s:%d...\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);
        consoleUpdate(NULL);

        sockfd = mqtt_raw_connect(MQTT_BROKER_IP, MQTT_BROKER_PORT);
        if (sockfd < 0) {
            printf("   [FAILED] TCP connection\n");
            printf("   Make sure Mosquitto is running!\n\n");
        } else {
            printf("   [OK] TCP connected (fd=%d)\n\n", sockfd);
            consoleUpdate(NULL);

            // Step 2: MQTT CONNECT
            printf("2. Sending MQTT CONNECT...\n");
            consoleUpdate(NULL);

            if (mqtt_raw_send_connect(sockfd, MQTT_CLIENT_ID) < 0) {
                printf("   [FAILED] CONNECT\n\n");
            } else {
                printf("   [OK] CONNECT sent\n\n");
                consoleUpdate(NULL);

                // Step 3: Wait for CONNACK
                printf("3. Waiting for CONNACK...\n");
                consoleUpdate(NULL);

                if (mqtt_raw_recv_connack(sockfd) < 0) {
                    printf("   [FAILED] CONNACK\n\n");
                } else {
                    printf("   [OK] Connection accepted!\n\n");
                    consoleUpdate(NULL);
                    mqtt_connected = 1;

                    // Step 4: Publish test message
                    char topic[64];
                    snprintf(topic, sizeof(topic), "%s/status", MQTT_TOPIC_PREFIX);

                    printf("4. Publishing to %s...\n", topic);
                    consoleUpdate(NULL);

                    if (mqtt_raw_send_publish(sockfd, topic, "online") < 0) {
                        printf("   [FAILED] PUBLISH\n\n");
                    } else {
                        printf("   [OK] Published: \"online\"\n\n");
                    }
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

        consoleUpdate(NULL);
    }

    // Cleanup
    if (sockfd >= 0) {
        if (mqtt_connected)
            mqtt_raw_send_disconnect(sockfd);
        mqtt_raw_close(sockfd);
    }

    socketExit();
    consoleExit(NULL);
    return 0;
}
