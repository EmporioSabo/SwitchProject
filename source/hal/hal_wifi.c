/*
 * hal_wifi.c - WiFi sensor implementation
 *
 * Two APIs exist for WiFi info on the Switch:
 *
 *   1. wlaninf (firmware 1.0.0-14.1.2): gives RSSI in dBm
 *   2. nifm (all firmware): gives signal bars (0-3)
 *
 * We try wlaninf first. If it's unavailable (newer firmware),
 * we fall back to nifm. This is a common embedded pattern:
 * graceful degradation when hardware/firmware features change.
 *
 * nifm needs its own explicit initialization — socketInitializeDefault()
 * sets up the BSD socket layer, not the nifm query API.
 */

#include <unistd.h>
#include "hal_wifi.h"

static bool wlaninf_available = false;
static bool nifm_available = false;

Result hal_wifi_init(void)
{
    /* Try wlaninf first (precise RSSI, older firmware only) */
    Result rc = wlaninfInitialize();
    wlaninf_available = R_SUCCEEDED(rc);

    /* Initialize nifm for connection status and signal bars */
    rc = nifmInitialize(NifmServiceType_User);
    nifm_available = R_SUCCEEDED(rc);

    return 0;  /* Always succeed — we handle missing services gracefully */
}

Result hal_wifi_read(hal_wifi_reading_t *out)
{
    out->connected   = false;
    out->rssi_dbm    = 0;
    out->signal_bars = 0;
    out->ip_addr     = 0;

    if (!nifm_available)
        return 1;  /* No network query service available */

    NifmInternetConnectionType conn_type;
    u32 wifi_strength;
    NifmInternetConnectionStatus conn_status;

    Result rc = nifmGetInternetConnectionStatus(&conn_type, &wifi_strength, &conn_status);
    if (R_FAILED(rc))
        return rc;

    out->connected   = (conn_status == NifmInternetConnectionStatus_Connected);
    out->signal_bars = wifi_strength;

    if (out->connected) {
        out->ip_addr = gethostid();

        /* Try wlaninf for precise RSSI if available */
        if (wlaninf_available) {
            rc = wlaninfGetRSSI(&out->rssi_dbm);
            if (R_FAILED(rc))
                out->rssi_dbm = 0;
        }
    }

    return 0;
}

void hal_wifi_exit(void)
{
    if (wlaninf_available)
        wlaninfExit();
    if (nifm_available)
        nifmExit();
}
