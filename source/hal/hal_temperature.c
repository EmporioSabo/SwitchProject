/*
 * hal_temperature.c - Temperature sensor implementation via TS service
 *
 * The TMP451 is a common temperature sensor IC found in many
 * embedded systems. It has two measurement channels:
 *
 *   - Local (Internal): measures the IC's own temperature,
 *     which tracks the PCB temperature nearby
 *   - Remote (External): measures a thermal diode on the SoC,
 *     giving the actual CPU/GPU die temperature
 *
 * Two APIs exist:
 *   - tsGetTemperature(): direct call, works on most firmware
 *   - tsOpenSession() + tsSessionGetTemperature(): session-based,
 *     available on firmware 8.0.0+
 *
 * We try the direct API first and fall back to sessions.
 */

#include "hal_temperature.h"

static bool use_session_api = false;
static TsSession session_internal;
static TsSession session_external;

Result hal_temperature_init(void)
{
    Result rc = tsInitialize();
    if (R_FAILED(rc))
        return rc;

    /*
     * Try a test read with the direct API. If it fails,
     * switch to the session-based API for all future reads.
     */
    s32 test;
    rc = tsGetTemperature(TsLocation_Internal, &test);
    if (R_FAILED(rc)) {
        /* Direct API not available — open sessions instead */
        Result rc1 = tsOpenSession(&session_internal, TsDeviceCode_LocationInternal);
        Result rc2 = tsOpenSession(&session_external, TsDeviceCode_LocationExternal);

        if (R_FAILED(rc1) || R_FAILED(rc2)) {
            if (R_SUCCEEDED(rc1)) tsSessionClose(&session_internal);
            if (R_SUCCEEDED(rc2)) tsSessionClose(&session_external);
            tsExit();
            return rc1 ? rc1 : rc2;
        }

        use_session_api = true;
    }

    return 0;
}

Result hal_temperature_read(hal_temperature_reading_t *out)
{
    Result rc;

    if (use_session_api) {
        float pcb_f, soc_f;

        rc = tsSessionGetTemperature(&session_internal, &pcb_f);
        if (R_FAILED(rc))
            return rc;

        rc = tsSessionGetTemperature(&session_external, &soc_f);
        if (R_FAILED(rc))
            return rc;

        out->pcb_celsius = (s32)pcb_f;
        out->soc_celsius = (s32)soc_f;
    } else {
        rc = tsGetTemperature(TsLocation_Internal, &out->pcb_celsius);
        if (R_FAILED(rc))
            return rc;

        rc = tsGetTemperature(TsLocation_External, &out->soc_celsius);
        if (R_FAILED(rc))
            return rc;
    }

    return 0;
}

void hal_temperature_exit(void)
{
    if (use_session_api) {
        tsSessionClose(&session_internal);
        tsSessionClose(&session_external);
    }
    tsExit();
}
