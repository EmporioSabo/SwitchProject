/*
 * main.c - Switch MQTT Telemetry v0.6
 *
 * Two-thread producer/consumer architecture:
 *
 *   Producer thread — polls sensors at configurable intervals,
 *                     writes latest readings to shared buffer
 *   Main thread     — acts as the consumer: reads shared buffer,
 *                     builds JSON, publishes to MQTT, renders UI
 *
 * Step 7 additions:
 *   - Subscribes to switch/cmd for remote commands (QoS 1)
 *   - Publishes telemetry at QoS 1 (guaranteed delivery)
 *   - Responds to: set_interval, set_poll_rate, ping, identify, publish_now
 *   - MQTTYield runs every loop iteration for prompt command delivery
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

/* ──────────────────────────────────────────────────────────────────────
 * Command handler state — file-scope statics accessed only from main
 * thread (command_handler runs inside MQTTYield on the main thread).
 * ──────────────────────────────────────────────────────────────────── */

static bool g_publish_now;              /* trigger immediate telemetry */
static u64  g_identify_until;           /* tick when identify banner expires */
static u64  g_start_tick;               /* app start time for uptime calc */
static char g_response_buf[256];        /* pending response JSON */
static bool g_has_response;             /* response ready to publish */

/* Forward declaration — defined after helpers */
static void command_handler(MessageData *data);

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

/* Clamp a u32 value to [lo, hi] */
static u32 clamp_u32(u32 val, u32 lo, u32 hi)
{
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
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

/* ──────────────────────────────────────────────────────────────────────
 * Subscribe to command topic — must be called after every (re)connect.
 *
 * With cleansession=1, the broker discards subscriptions on disconnect.
 * MQTTClientInit also clears handler slots, so we re-subscribe every time.
 * ──────────────────────────────────────────────────────────────────── */

static int mqtt_subscribe_commands(MQTTClient *client)
{
    return MQTTSubscribe(client, MQTT_CMD_TOPIC, QOS1, command_handler);
}

/* ──────────────────────────────────────────────────────────────────────
 * Command handler — called inside MQTTYield() on the main thread.
 *
 * Parses incoming JSON commands from switch/cmd and either updates
 * shared state directly or sets flags for the main loop to act on.
 *
 * Supported commands:
 *   {"cmd":"set_interval","value":N}     — change publish interval (ms)
 *   {"cmd":"set_poll_rate","sensor":"battery|temp|wifi","value":N}
 *   {"cmd":"ping"}                       — reply with pong + uptime
 *   {"cmd":"identify"}                   — flash UI banner
 *   {"cmd":"publish_now"}                — trigger immediate publish
 * ──────────────────────────────────────────────────────────────────── */

static void command_handler(MessageData *data)
{
    /* Null-terminate the payload for cJSON (it may not be terminated) */
    size_t len = data->message->payloadlen;
    if (len >= 512) return;  /* ignore oversized payloads */

    char buf[512];
    memcpy(buf, data->message->payload, len);
    buf[len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return;

    cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd_item)) {
        cJSON_Delete(root);
        return;
    }
    const char *cmd = cmd_item->valuestring;

    /* Update command stats */
    mutexLock(&g_shared.mutex);
    g_shared.cmd_count++;
    strncpy(g_shared.last_cmd, cmd, sizeof(g_shared.last_cmd) - 1);
    g_shared.last_cmd[sizeof(g_shared.last_cmd) - 1] = '\0';
    mutexUnlock(&g_shared.mutex);

    if (strcmp(cmd, "set_interval") == 0) {
        cJSON *val = cJSON_GetObjectItem(root, "value");
        if (cJSON_IsNumber(val)) {
            u32 ms = clamp_u32((u32)val->valuedouble, 1000, 60000);
            mutexLock(&g_shared.mutex);
            g_shared.telemetry_interval_ms = ms;
            mutexUnlock(&g_shared.mutex);

            snprintf(g_response_buf, sizeof(g_response_buf),
                     "{\"cmd\":\"ack\",\"original\":\"set_interval\",\"value\":%u}", ms);
            g_has_response = true;
        }
    } else if (strcmp(cmd, "set_poll_rate") == 0) {
        cJSON *sensor = cJSON_GetObjectItem(root, "sensor");
        cJSON *val = cJSON_GetObjectItem(root, "value");
        if (cJSON_IsString(sensor) && cJSON_IsNumber(val)) {
            u32 ms = clamp_u32((u32)val->valuedouble, 1000, 300000);
            const char *s = sensor->valuestring;
            mutexLock(&g_shared.mutex);
            if (strcmp(s, "battery") == 0)
                g_shared.poll_battery_ms = ms;
            else if (strcmp(s, "temp") == 0)
                g_shared.poll_temp_ms = ms;
            else if (strcmp(s, "wifi") == 0)
                g_shared.poll_wifi_ms = ms;
            mutexUnlock(&g_shared.mutex);

            snprintf(g_response_buf, sizeof(g_response_buf),
                     "{\"cmd\":\"ack\",\"original\":\"set_poll_rate\","
                     "\"sensor\":\"%s\",\"value\":%u}", s, ms);
            g_has_response = true;
        }
    } else if (strcmp(cmd, "ping") == 0) {
        u64 uptime_s = (armGetSystemTick() - g_start_tick) / armGetSystemTickFreq();
        snprintf(g_response_buf, sizeof(g_response_buf),
                 "{\"cmd\":\"pong\",\"uptime_s\":%llu}",
                 (unsigned long long)uptime_s);
        g_has_response = true;
    } else if (strcmp(cmd, "identify") == 0) {
        g_identify_until = armGetSystemTick() + 3 * armGetSystemTickFreq();
    } else if (strcmp(cmd, "publish_now") == 0) {
        g_publish_now = true;
    }

    cJSON_Delete(root);
}

/* ──────────────────────────────────────────────────────────────────────
 * Disconnect helper — centralize disconnect + state transition
 * ──────────────────────────────────────────────────────────────────── */

static void mqtt_force_disconnect(Network *net, u64 now, u64 freq,
                                  u64 *next_reconnect, u32 *reconnect_delay_ms)
{
    NetworkDisconnect(net);
    g_shared.mqtt_state = MQTT_STATE_DISCONNECTED;
    *next_reconnect = now + (u64)MQTT_RECONNECT_DELAY_MS * freq / 1000;
    *reconnect_delay_ms = MQTT_RECONNECT_DELAY_MS;
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    consoleInit(NULL);
    g_start_tick = armGetSystemTick();

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
     * Set runtime-configurable intervals to compile-time defaults.
     */
    memset(&g_shared, 0, sizeof(g_shared));
    g_shared.mqtt_state = MQTT_STATE_DISCONNECTED;
    g_shared.telemetry_interval_ms = TELEMETRY_INTERVAL_MS;
    g_shared.poll_battery_ms       = SENSOR_POLL_BATTERY_MS;
    g_shared.poll_temp_ms          = SENSOR_POLL_TEMP_MS;
    g_shared.poll_wifi_ms          = SENSOR_POLL_WIFI_MS;

    /* Banner */
    printf("=================================\n");
    printf(" Switch MQTT Telemetry v0.6\n");
    printf("=================================\n\n");

    if (R_SUCCEEDED(rc)) {
        struct in_addr addr;
        addr.s_addr = gethostid();
        printf("Switch IP : %s\n", inet_ntoa(addr));
    } else {
        printf("Switch IP : unavailable\n");
    }

    printf("Broker    : %s:%d\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);
    printf("Publish   : %s (QoS 1)\n", MQTT_TELEMETRY_TOPIC);
    printf("Subscribe : %s (QoS 1)\n", MQTT_CMD_TOPIC);
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

    if (mqtt_try_connect(&network, &mqtt_client,
                         sendbuf, sizeof(sendbuf),
                         readbuf, sizeof(readbuf)) == 0) {
        mqtt_subscribe_commands(&mqtt_client);
    }

    /*
     * Main loop — UI refresh, MQTT publishing, command processing.
     *
     * Timers run at different rates:
     *   MQTTYield:    every iteration (~50ms) — process incoming commands
     *   UI refresh:   every 500ms (2 Hz)
     *   MQTT publish: every telemetry_interval_ms (default 5s, configurable)
     *   Button poll:  every 50ms (20 Hz)
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
                /* Success — reset backoff, re-subscribe to commands */
                reconnect_delay_ms = MQTT_RECONNECT_DELAY_MS;
                mqtt_subscribe_commands(&mqtt_client);
            } else {
                /* Failed — schedule next attempt with backoff */
                next_reconnect = now + (u64)reconnect_delay_ms * freq / 1000;
                reconnect_delay_ms *= 2;
                if (reconnect_delay_ms > MQTT_RECONNECT_MAX_MS)
                    reconnect_delay_ms = MQTT_RECONNECT_MAX_MS;
            }
        }

        /* ── Process incoming MQTT (commands, PINGRESP, keepalive) ── */
        if (mqtt_client.isconnected) {
            MQTTYield(&mqtt_client, MQTT_YIELD_MS);

            /* Publish pending response from command handler */
            if (g_has_response) {
                MQTTMessage resp;
                memset(&resp, 0, sizeof(resp));
                resp.qos = QOS1;
                resp.payload = g_response_buf;
                resp.payloadlen = strlen(g_response_buf);
                MQTTPublish(&mqtt_client, MQTT_RESPONSE_TOPIC, &resp);
                g_has_response = false;
            }
        }

        /*
         * Detect silent disconnect — Paho's keepalive may have
         * internally set isconnected=0, but our state still says
         * CONNECTED. Sync them so the reconnection logic triggers.
         */
        if (!mqtt_client.isconnected && g_shared.mqtt_state == MQTT_STATE_CONNECTED) {
            mqtt_force_disconnect(&network, now, freq,
                                  &next_reconnect, &reconnect_delay_ms);
        }

        /* ── MQTT publish (runtime-configurable interval) ── */
        u32 interval_ms = g_shared.telemetry_interval_ms;
        bool do_publish = (now - last_publish >= (u64)interval_ms * freq / 1000);

        /* publish_now flag from command handler */
        if (g_publish_now) {
            do_publish = true;
            g_publish_now = false;
        }

        if (do_publish && mqtt_client.isconnected) {
            last_publish = now;

            char *json = telemetry_build_json();
            if (json) {
                MQTTMessage msg;
                memset(&msg, 0, sizeof(msg));
                msg.qos = QOS1;
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
                    mqtt_force_disconnect(&network, now, freq,
                                          &next_reconnect, &reconnect_delay_ms);
                }
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

            /* Identify banner (flashes for 3 seconds) */
            if (g_identify_until > 0 && now < g_identify_until) {
                printf(">>> IDENTIFY <<<                             \n");
            } else {
                printf("                                             \n");
                g_identify_until = 0;
            }
            ui_lines++;

            /* MQTT status */
            printf("=== MQTT Status ===                          \n");
            ui_lines++;

            printf("State     : %-20s\n", mqtt_state_str(snap.mqtt_state));
            ui_lines++;

            printf("Published : %u msgs (QoS 1) | interval %us    \n",
                   snap.publish_count, snap.telemetry_interval_ms / 1000);
            ui_lines++;

            if (snap.last_publish_tick > 0) {
                u64 ago = (now - snap.last_publish_tick) / freq;
                printf("Last pub  : %llu seconds ago            \n",
                       (unsigned long long)ago);
            } else {
                printf("Last pub  : never                       \n");
            }
            ui_lines++;

            printf("Commands  : %u", snap.cmd_count);
            if (snap.cmd_count > 0)
                printf(" (last: %s)", snap.last_cmd);
            printf("                        \n");
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
