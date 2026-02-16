/*
 * telemetry.h - Shared state for producer/consumer telemetry
 *
 * The producer/consumer pattern decouples data generation from data
 * transmission. The producer thread polls sensors at configurable
 * intervals and writes the latest readings into a shared buffer.
 * The main thread acts as the consumer — it periodically reads the
 * buffer, builds a JSON payload, and publishes it over MQTT.
 *
 * Why not a dedicated consumer thread? libnx's BSD socket layer uses
 * a global IPC session to the bsd:u service. Blocking calls like
 * connect() hold that session lock, freezing any other thread that
 * touches the network stack. By keeping all socket I/O on the main
 * thread, we avoid this contention entirely.
 *
 * The shared buffer is a snapshot (not a queue) — we only care about
 * the most recent state, not a history of readings. This simplifies
 * synchronization: one mutex, no ring buffer bookkeeping.
 */

#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <switch.h>

#include "hal_battery.h"
#include "hal_temperature.h"
#include "hal_wifi.h"

/*
 * MQTT connection state — drives both the reconnection logic
 * and the status display in the main thread.
 */
typedef enum {
    MQTT_STATE_DISCONNECTED,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_RECONNECTING
} mqtt_state_t;

/*
 * Shared telemetry buffer — the rendezvous point between threads.
 *
 * All fields are protected by `mutex`. The pattern for accessing them:
 *
 *   mutexLock(&g_shared.mutex);
 *   // read or write fields
 *   mutexUnlock(&g_shared.mutex);
 *
 * Keep the critical section small — copy what you need into a local
 * variable, then unlock before doing slow work (JSON building, I/O).
 */
typedef struct {
    /* Latest sensor snapshots (written by producer) */
    hal_battery_reading_t     battery;
    hal_temperature_reading_t temperature;
    hal_wifi_reading_t        wifi;

    /* Set to true after first successful read of each sensor */
    bool battery_valid;
    bool temperature_valid;
    bool wifi_valid;

    /* MQTT status (written by main thread, read for UI) */
    mqtt_state_t mqtt_state;
    u32 publish_count;
    u64 last_publish_tick;

    /* Runtime-configurable intervals (initialized from config.h defaults) */
    u32 telemetry_interval_ms;
    u32 poll_battery_ms;
    u32 poll_temp_ms;
    u32 poll_wifi_ms;

    /* Command stats (written by main thread) */
    u32 cmd_count;
    char last_cmd[32];

    Mutex mutex;  /* libnx mutex — lightweight, no init needed beyond zero */
} telemetry_shared_t;

/* Global shared state — defined in telemetry.c */
extern telemetry_shared_t g_shared;

/* Shutdown flag — set to false by main thread, checked by producer */
extern bool g_running;

/*
 * Producer thread entry point — passed to threadCreate().
 * Polls sensors at configurable intervals, writes to g_shared.
 */
void producer_thread_entry(void *arg);

/*
 * Build a JSON payload from the current shared state.
 * Locks the mutex internally, safe to call from any context.
 * Returns a heap-allocated string — caller frees with cJSON_free().
 * Returns NULL if no sensor data is available yet.
 */
char *telemetry_build_json(void);

#endif /* TELEMETRY_H */
