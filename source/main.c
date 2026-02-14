#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <switch.h>

#include "config.h"

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
    printf(" Switch MQTT Telemetry v0.1\n");
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
    printf("Press + to exit\n");

    consoleUpdate(NULL);

    // Main loop
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus)
            break;

        consoleUpdate(NULL);
    }

    socketExit();
    consoleExit(NULL);
    return 0;
}
