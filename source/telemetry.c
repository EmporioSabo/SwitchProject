/*
 * telemetry.c - Producer thread and JSON payload builder
 *
 * The producer thread polls sensors at different rates and writes
 * the latest readings into a shared buffer. The main thread reads
 * this buffer to build JSON payloads and publish them over MQTT.
 *
 * Threading primitives (libnx / Horizon OS):
 *   Mutex          — mutual exclusion (prevents data races)
 *   svcSleepThread — suspends thread without busy-waiting
 *
 * The mutex is held only long enough to copy a struct — never during
 * sensor reads or network I/O. This is critical: holding a lock during
 * a slow operation (like a network write that times out) would block
 * the other thread, defeating the purpose of threading.
 */

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <switch.h>

#include "config.h"
#include "telemetry.h"
#include "cJSON.h"

/* ──────────────────────────────────────────────────────────────────────
 * Global state (declared extern in telemetry.h)
 * ──────────────────────────────────────────────────────────────────── */

telemetry_shared_t g_shared;
bool g_running = true;

/* ──────────────────────────────────────────────────────────────────────
 * Timer helpers — convert milliseconds to ARM system ticks
 *
 * armGetSystemTick() reads the CPU cycle counter (CNTPCT_EL0).
 * armGetSystemTickFreq() returns ticks-per-second (19.2 MHz on Tegra).
 * Together they give sub-microsecond timing without OS overhead.
 * ──────────────────────────────────────────────────────────────────── */

static u64 ms_to_ticks(u64 ms)
{
    return ms * armGetSystemTickFreq() / 1000;
}

static bool tick_expired(u64 target)
{
    return armGetSystemTick() >= target;
}

/* ──────────────────────────────────────────────────────────────────────
 * Sleep helper — wraps svcSleepThread for millisecond granularity.
 *
 * svcSleepThread is a Horizon OS syscall that yields the CPU to
 * other threads for the specified duration in nanoseconds.
 * ──────────────────────────────────────────────────────────────────── */

static void sleep_ms(u32 ms)
{
    svcSleepThread((u64)ms * 1000000ULL);
}

/* ══════════════════════════════════════════════════════════════════════
 * PRODUCER THREAD
 *
 * Time-triggered architecture: wake up, check timers, poll whichever
 * sensor is due, go back to sleep. Each sensor has its own interval
 * because they change at different rates:
 *
 *   Battery (30s)     — percentage drifts slowly
 *   Temperature (10s) — can spike during gameplay
 *   WiFi (5s)         — signal fluctuates, detect drops quickly
 * ══════════════════════════════════════════════════════════════════════ */

void producer_thread_entry(void *arg)
{
    (void)arg;

    /* Delay to let the main thread enter its event loop */
    sleep_ms(3000);

    /* Schedule all sensors to fire immediately on first iteration */
    u64 next_battery = armGetSystemTick();
    u64 next_temp    = armGetSystemTick();
    u64 next_wifi    = armGetSystemTick();

    while (g_running) {
        /*
         * Read sensors OUTSIDE the lock. Sensor reads are IPC calls
         * to system services (psm, ts, nifm) — they can take milliseconds.
         * We don't want to hold the mutex during that time.
         */

        /* Battery */
        if (tick_expired(next_battery)) {
            hal_battery_reading_t reading;
            if (R_SUCCEEDED(hal_battery_read(&reading))) {
                mutexLock(&g_shared.mutex);
                g_shared.battery = reading;
                g_shared.battery_valid = true;
                mutexUnlock(&g_shared.mutex);
            }
            next_battery = armGetSystemTick() + ms_to_ticks(SENSOR_POLL_BATTERY_MS);
        }

        /* Temperature */
        if (tick_expired(next_temp)) {
            hal_temperature_reading_t reading;
            if (R_SUCCEEDED(hal_temperature_read(&reading))) {
                mutexLock(&g_shared.mutex);
                g_shared.temperature = reading;
                g_shared.temperature_valid = true;
                mutexUnlock(&g_shared.mutex);
            }
            next_temp = armGetSystemTick() + ms_to_ticks(SENSOR_POLL_TEMP_MS);
        }

        /* WiFi */
        if (tick_expired(next_wifi)) {
            hal_wifi_reading_t reading;
            if (R_SUCCEEDED(hal_wifi_read(&reading))) {
                mutexLock(&g_shared.mutex);
                g_shared.wifi = reading;
                g_shared.wifi_valid = true;
                mutexUnlock(&g_shared.mutex);
            }
            next_wifi = armGetSystemTick() + ms_to_ticks(SENSOR_POLL_WIFI_MS);
        }

        /* Sleep 100ms — no need for sub-second precision in polling */
        sleep_ms(100);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * JSON payload builder
 *
 * cJSON builds a tree of JSON nodes in memory, then serializes them
 * to a string. We use PrintUnformatted to minimize payload size.
 * The caller must free the returned string with cJSON_free().
 * ══════════════════════════════════════════════════════════════════════ */

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

static char *build_json_payload(const telemetry_shared_t *snap)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    /* Battery */
    if (snap->battery_valid) {
        cJSON *bat = cJSON_CreateObject();
        cJSON_AddNumberToObject(bat, "percentage",    snap->battery.percentage);
        cJSON_AddNumberToObject(bat, "voltage_mv",    snap->battery.voltage_mv);
        cJSON_AddNumberToObject(bat, "temperature_c", snap->battery.temperature_c);
        cJSON_AddBoolToObject(bat,   "charging",      snap->battery.charging);
        cJSON_AddStringToObject(bat, "charger_type",  charger_type_str(snap->battery.charger_type));
        cJSON_AddItemToObject(root, "battery", bat);
    }

    /* Temperature */
    if (snap->temperature_valid) {
        cJSON *temp = cJSON_CreateObject();
        cJSON_AddNumberToObject(temp, "soc_celsius", snap->temperature.soc_celsius);
        cJSON_AddNumberToObject(temp, "pcb_celsius", snap->temperature.pcb_celsius);
        cJSON_AddItemToObject(root, "temperature", temp);
    }

    /* WiFi */
    if (snap->wifi_valid) {
        cJSON *wifi = cJSON_CreateObject();
        cJSON_AddBoolToObject(wifi, "connected", snap->wifi.connected);
        cJSON_AddNumberToObject(wifi, "signal_bars", snap->wifi.signal_bars);
        if (snap->wifi.rssi_dbm != 0)
            cJSON_AddNumberToObject(wifi, "rssi_dbm", snap->wifi.rssi_dbm);
        if (snap->wifi.connected) {
            struct in_addr addr;
            addr.s_addr = snap->wifi.ip_addr;
            cJSON_AddStringToObject(wifi, "ip", inet_ntoa(addr));
        }
        cJSON_AddItemToObject(root, "wifi", wifi);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;  /* caller frees with cJSON_free() */
}

/*
 * telemetry_build_json — public API for the main thread.
 *
 * Locks the shared mutex, takes a snapshot, unlocks, then builds
 * the JSON string outside the critical section.
 */
char *telemetry_build_json(void)
{
    telemetry_shared_t snap;
    mutexLock(&g_shared.mutex);
    memcpy(&snap, &g_shared, sizeof(snap));
    mutexUnlock(&g_shared.mutex);
    return build_json_payload(&snap);
}
