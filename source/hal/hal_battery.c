/*
 * hal_battery.c - Battery sensor implementation via PSM service
 *
 * PSM provides two levels of battery info:
 *   1. Simple: psmGetBatteryChargePercentage() — just the percentage
 *   2. Detailed: psmGetBatteryChargeInfoFields() — voltage, current,
 *      temperature, charger type, and more in a single struct
 *
 * We use the detailed API to get everything in one call, plus
 * the simple percentage API (which is more reliable for display).
 */

#include "hal_battery.h"

Result hal_battery_init(void)
{
    return psmInitialize();
}

Result hal_battery_read(hal_battery_reading_t *out)
{
    Result rc;

    /* Get battery percentage (0-100) */
    rc = psmGetBatteryChargePercentage(&out->percentage);
    if (R_FAILED(rc))
        return rc;

    /* Get charger type */
    rc = psmGetChargerType(&out->charger_type);
    if (R_FAILED(rc))
        return rc;

    /*
     * Get detailed battery info — voltage, temperature, charging state.
     * This is a single IPC call that returns everything the battery
     * controller knows. On STM32 you'd read these from individual
     * ADC channels or I2C registers; here the OS aggregates them.
     */
    PsmBatteryChargeInfoFields info;
    rc = psmGetBatteryChargeInfoFields(&info);
    if (R_FAILED(rc))
        return rc;

    out->voltage_mv    = info.battery_charge_milli_voltage;
    out->charging      = info.battery_charging;

    /*
     * Despite its name, temperature_celcius is actually in
     * milliCelsius (33000 = 33.0°C). The field was likely named
     * before the unit was finalized — a common issue in vendor
     * SDKs. Always verify units empirically, not just by name.
     */
    out->temperature_c = (s32)(info.temperature_celcius / 1000);

    return 0;
}

void hal_battery_exit(void)
{
    psmExit();
}
