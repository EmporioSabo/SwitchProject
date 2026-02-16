/*
 * hal_wifi.h - WiFi sensor HAL module
 *
 * Reads WiFi state via WLAN Infrastructure or NIFM services.
 *
 * On firmware <= 14.1.2, wlaninf gives precise RSSI in dBm.
 * On newer firmware, wlaninf is gone — we fall back to nifm
 * which only gives signal bars (0-3). The HAL hides this
 * difference from the caller.
 *
 * RSSI (Received Signal Strength Indicator) is measured in dBm:
 *   -30 dBm = excellent (right next to the router)
 *   -50 dBm = good
 *   -70 dBm = fair
 *   -90 dBm = barely connected
 */

#ifndef HAL_WIFI_H
#define HAL_WIFI_H

#include <switch.h>

typedef struct {
    bool connected;
    s32  rssi_dbm;      /* signal strength in dBm, or 0 if unavailable */
    u32  signal_bars;   /* 0-3 (from nifm, always available) */
    u32  ip_addr;       /* IPv4 in network byte order */
} hal_wifi_reading_t;

Result hal_wifi_init(void);
Result hal_wifi_read(hal_wifi_reading_t *out);
void   hal_wifi_exit(void);

#endif /* HAL_WIFI_H */
