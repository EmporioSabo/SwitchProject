/*
 * main.c - Switch MQTT Telemetry v0.5
 *
 * Two-thread producer/consumer architecture:
 *
 *   Producer thread — polls sensors at configurable intervals,
 *                     writes latest readings to shared buffer
 *   Main thread     — acts as the consumer: reads shared buffer,
 *                     builds JSON, publishes to MQTT, renders UI
 *
 * Why MQTT runs on the main thread instead of a dedicated consumer:
 * libnx's BSD socket layer routes all socket calls through a single
 * IPC session (bsd:u). A blocking connect() in a worker thread holds
 * that session lock, freezing any other thread that touches the
 * network — including the main loop. Keeping all socket I/O on one
 * thread avoids this contention entirely.
 *
 * Threading uses libnx native primitives (Thread, Mutex, svcSleepThread)
 * because the devkitPro newlib C11 <threads.h> compiles but crashes
 * at runtime — a classic embedded pitfall: headers exist != runtime works.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <switch.h>

#include "config.h"
#include "telemetry.h"
#include "mqtt_switch.h"
#include "MQTTClient.h"
#include "cJSON.h"

/*
 * Stack size for the producer thread. 0x10000 (64 KB) is generous —
 * the biggest stack consumer is the HAL read functions which do IPC.
 * libnx's default main thread stack is 128 KB for comparison.
 */
#define THREAD_STACK_SIZE  0x10000

/* Charger type as human-readable string (for UI display) */
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

/* MQTT state as human-readable string */
static const char *mqtt_state_str(mqtt_state_t state)
{
    switch (state) {
    case MQTT_STATE_DISCONNECTED:  return "Disconnected";
    case MQTT_STATE_CONNECTING:    return "Connecting...";
    case MQTT_STATE_CONNECTED:     return "Connected";
    case MQTT_STATE_RECONNECTING:  return "Reconnecting...";
    default:                       return "Unknown";
    }
}

/* ──────────────────────────────────────────────────────────────────────
 * MQTT connection helper
 *
 * Attempt a full MQTT connection: TCP socket + MQTT CONNECT.
 * Returns 0 on success, -1 on failure.
 * ──────────────────────────────────────────────────────────────────── */

static int mqtt_try_connect(Network *net, MQTTClient *client,
                            unsigned char *sendbuf, int sendbuf_sz,
                            unsigned char *readbuf, int readbuf_sz)
{
    g_shared.mqtt_state = MQTT_STATE_CONNECTING;

    NetworkInit(net);
    if (NetworkConnect(net, MQTT_BROKER_IP, MQTT_BROKER_PORT) < 0) {
        g_shared.mqtt_state = MQTT_STATE_DISCONNECTED;
        return -1;
    }

    MQTTClientInit(client, net, 5000,
                   sendbuf, sendbuf_sz, readbuf, readbuf_sz);

    MQTTPacket_connectData opts = MQTTPacket_connectData_initializer;
    opts.MQTTVersion = 4;
    opts.clientID.cstring = MQTT_CLIENT_ID;
    opts.keepAliveInterval = 60;
    opts.cleansession = 1;

    if (MQTTConnect(client, &opts) != SUCCESS) {
        NetworkDisconnect(net);
        g_shared.mqtt_state = MQTT_STATE_DISCONNECTED;
        return -1;
    }

    g_shared.mqtt_state = MQTT_STATE_CONNECTED;
    return 0;
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    consoleInit(NULL);

    /* Configure input */
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    /* Initialize network stack */
    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        printf("socketInitializeDefault() failed: 0x%x\n", rc);
    }

    /* Initialize sensor HAL modules */
    hal_battery_init();
    hal_temperature_init();
    hal_wifi_init();

    /*
     * Initialize shared telemetry buffer.
     * libnx Mutex is zero-initialized (no init function needed),
     * but memset ensures all fields start clean.
     */
    memset(&g_shared, 0, sizeof(g_shared));
    g_shared.mqtt_state = MQTT_STATE_DISCONNECTED;

    /* Banner */
    printf("=================================\n");
    printf(" Switch MQTT Telemetry v0.5\n");
    printf("=================================\n\n");

    if (R_SUCCEEDED(rc)) {
        struct in_addr addr;
        addr.s_addr = gethostid();
        printf("Switch IP : %s\n", inet_ntoa(addr));
    } else {
        printf("Switch IP : unavailable\n");
    }

    printf("Broker    : %s:%d\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);
    printf("Topic     : %s\n", MQTT_TELEMETRY_TOPIC);
    printf("Press + to stop and exit\n\n");
    consoleUpdate(NULL);

    /*
     * Launch the producer thread for sensor polling.
     *
     * threadCreate parameters:
     *   - Thread *t        : thread handle (for join/close later)
     *   - ThreadFunc entry : void func(void *arg)
     *   - void *arg        : argument passed to entry (NULL for us)
     *   - void *stack      : NULL = auto-allocate stack
     *   - size_t stack_sz  : stack size in bytes
     *   - int priority     : 0x3B = same as main thread (default user priority)
     *   - int cpuid        : -2 = let the OS pick any available core
     */
    Thread producer;
    g_running = true;

    rc = threadCreate(&producer, producer_thread_entry, NULL,
                      NULL, THREAD_STACK_SIZE, 0x3B, -2);
    if (R_FAILED(rc)) {
        printf("Failed to create producer thread: 0x%x\n", rc);
        consoleUpdate(NULL);
        goto cleanup;
    }
    rc = threadStart(&producer);
    if (R_FAILED(rc)) {
        printf("Failed to start producer thread: 0x%x\n", rc);
        threadClose(&producer);
        consoleUpdate(NULL);
        goto cleanup;
    }

    /*
     * MQTT connection — runs on the main thread to avoid libnx's
     * BSD socket layer contention (single IPC session).
     *
     * This is a blocking call. If the broker is reachable, connect()
     * completes in ~100ms. If not, it blocks for the TCP timeout.
     */
    Network network;
    MQTTClient mqtt_client;
    unsigned char sendbuf[1024];
    unsigned char readbuf[256];

    printf("Connecting to MQTT broker...\n");
    consoleUpdate(NULL);

    mqtt_try_connect(&network, &mqtt_client,
                     sendbuf, sizeof(sendbuf),
                     readbuf, sizeof(readbuf));

    /*
     * Main loop — UI refresh, MQTT publishing, button polling.
     *
     * Three timers run at different rates:
     *   UI refresh:   every 500ms (2 Hz — smooth enough for status display)
     *   MQTT publish:  every 5s   (TELEMETRY_INTERVAL_MS)
     *   Button poll:  every 50ms  (20 Hz — responsive to user input)
     */
    int ui_lines = 0;
    u64 last_ui_update = 0;
    u64 last_publish = 0;
    u64 next_reconnect = 0;
    u32 reconnect_delay_ms = MQTT_RECONNECT_DELAY_MS;

    telemetry_shared_t snap;
    memset(&snap, 0, sizeof(snap));

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus)
            break;

        u64 now = armGetSystemTick();
        u64 freq = armGetSystemTickFreq();

        /* ── MQTT reconnection (non-blocking, exponential backoff) ── */
        if (g_shared.mqtt_state == MQTT_STATE_DISCONNECTED && now >= next_reconnect) {
            g_shared.mqtt_state = MQTT_STATE_RECONNECTING;
            if (mqtt_try_connect(&network, &mqtt_client,
                                 sendbuf, sizeof(sendbuf),
                                 readbuf, sizeof(readbuf)) == 0) {
                /* Success — reset backoff */
                reconnect_delay_ms = MQTT_RECONNECT_DELAY_MS;
            } else {
                /* Failed — schedule next attempt with backoff */
                next_reconnect = now + (u64)reconnect_delay_ms * freq / 1000;
                reconnect_delay_ms *= 2;
                if (reconnect_delay_ms > MQTT_RECONNECT_MAX_MS)
                    reconnect_delay_ms = MQTT_RECONNECT_MAX_MS;
            }
        }

        /* ── MQTT publish (every TELEMETRY_INTERVAL_MS) ── */
        if (now - last_publish >= (u64)TELEMETRY_INTERVAL_MS * freq / 1000) {
            last_publish = now;

            if (mqtt_client.isconnected) {
                /*
                 * MQTTYield BEFORE publish — let Paho process incoming
                 * PINGRESP and handle keepalive. 100ms timeout is fine
                 * since this block only runs every 5 seconds.
                 */
                MQTTYield(&mqtt_client, 100);

                char *json = telemetry_build_json();
                if (json) {
                    MQTTMessage msg;
                    memset(&msg, 0, sizeof(msg));
                    msg.qos = QOS0;
                    msg.payload = json;
                    msg.payloadlen = strlen(json);

                    int pub_rc = MQTTPublish(&mqtt_client, MQTT_TELEMETRY_TOPIC, &msg);
                    cJSON_free(json);

                    if (pub_rc == SUCCESS) {
                        mutexLock(&g_shared.mutex);
                        g_shared.publish_count++;
                        g_shared.last_publish_tick = now;
                        mutexUnlock(&g_shared.mutex);
                    } else {
                        /* Publish failed — broker went away */
                        NetworkDisconnect(&network);
                        g_shared.mqtt_state = MQTT_STATE_DISCONNECTED;
                        next_reconnect = now + (u64)MQTT_RECONNECT_DELAY_MS * freq / 1000;
                        reconnect_delay_ms = MQTT_RECONNECT_DELAY_MS;
                    }
                }
            }

            /*
             * Detect silent disconnect — Paho's keepalive may have
             * internally set isconnected=0, but our state still says
             * CONNECTED. Sync them so the reconnection logic triggers.
             */
            if (!mqtt_client.isconnected && g_shared.mqtt_state == MQTT_STATE_CONNECTED) {
                NetworkDisconnect(&network);
                g_shared.mqtt_state = MQTT_STATE_DISCONNECTED;
                next_reconnect = now + (u64)MQTT_RECONNECT_DELAY_MS * freq / 1000;
                reconnect_delay_ms = MQTT_RECONNECT_DELAY_MS;
            }
        }

        /* ── UI refresh (every 500ms) ── */
        if (now - last_ui_update >= freq / 2) {
            last_ui_update = now;

            /* Move cursor back to overwrite previous output */
            if (ui_lines > 0)
                printf("\x1b[%dA", ui_lines);
            ui_lines = 0;

            /* Snapshot shared state (fast copy under lock) */
            if (mutexTryLock(&g_shared.mutex)) {
                memcpy(&snap, &g_shared, sizeof(snap));
                mutexUnlock(&g_shared.mutex);
            }

            /* MQTT status */
            printf("=== MQTT Status ===                          \n");
            ui_lines++;

            printf("State     : %-20s\n", mqtt_state_str(snap.mqtt_state));
            ui_lines++;

            printf("Published : %u messages                \n", snap.publish_count);
            ui_lines++;

            if (snap.last_publish_tick > 0) {
                u64 ago = (now - snap.last_publish_tick) / freq;
                printf("Last pub  : %llu seconds ago            \n",
                       (unsigned long long)ago);
            } else {
                printf("Last pub  : never                       \n");
            }
            ui_lines++;

            /* Sensor readings */
            printf("\n=== Sensor Readings ===                    \n");
            ui_lines += 2;

            /* Battery */
            if (snap.battery_valid) {
                printf("Battery : %u%% | %u mV | %dC | %s       \n",
                       snap.battery.percentage, snap.battery.voltage_mv,
                       snap.battery.temperature_c,
                       charger_type_str(snap.battery.charger_type));
            } else {
                printf("Battery : waiting...                        \n");
            }
            ui_lines++;

            /* Temperature */
            if (snap.temperature_valid) {
                printf("Temp    : SoC %dC | PCB %dC                \n",
                       snap.temperature.soc_celsius, snap.temperature.pcb_celsius);
            } else {
                printf("Temp    : waiting...                        \n");
            }
            ui_lines++;

            /* WiFi */
            if (snap.wifi_valid) {
                if (snap.wifi.connected) {
                    struct in_addr addr;
                    addr.s_addr = snap.wifi.ip_addr;
                    if (snap.wifi.rssi_dbm != 0)
                        printf("WiFi    : %d dBm | %s                \n",
                               snap.wifi.rssi_dbm, inet_ntoa(addr));
                    else
                        printf("WiFi    : %u/3 bars | %s              \n",
                               snap.wifi.signal_bars, inet_ntoa(addr));
                } else {
                    printf("WiFi    : disconnected                      \n");
                }
            } else {
                printf("WiFi    : waiting...                        \n");
            }
            ui_lines++;

            consoleUpdate(NULL);
        }

        /*
         * Sleep 50ms to reduce CPU usage. The main thread doesn't need
         * high frequency — UI refreshes at 2 Hz, button polling is fine
         * at 20 Hz. Sleeping lets the OS schedule the producer thread.
         */
        svcSleepThread(50000000ULL);  /* 50ms in nanoseconds */
    }

    /*
     * Shutdown sequence:
     *   1. Disconnect MQTT cleanly
     *   2. Signal producer thread to stop
     *   3. Wait for it to finish (threadWaitForExit blocks)
     *   4. Release thread resources (threadClose)
     *   5. Clean up HAL and network in reverse init order
     */
    printf("\nShutting down...\n");
    consoleUpdate(NULL);

    if (mqtt_client.isconnected)
        MQTTDisconnect(&mqtt_client);
    NetworkDisconnect(&network);

    g_running = false;
    threadWaitForExit(&producer);
    threadClose(&producer);

cleanup:
    hal_wifi_exit();
    hal_temperature_exit();
    hal_battery_exit();

    socketExit();
    consoleExit(NULL);
    return 0;
}
