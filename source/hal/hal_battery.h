/*
 * hal_battery.h - Battery sensor HAL module
 *
 * Reads battery state via the PSM (Power State Management) service.
 * PSM talks to the battery controller IC on the Switch's mainboard,
 * which monitors the Li-ion cell's voltage, current, and temperature.
 *
 * This is the same kind of "fuel gauge" IC you'd find in any phone
 * or laptop — it tracks coulombs in/out to estimate remaining charge.
 */

#ifndef HAL_BATTERY_H
#define HAL_BATTERY_H

#include <switch.h>

typedef struct {
    u32  percentage;        /* 0-100 */
    u32  voltage_mv;        /* millivolts (e.g. 3890 = 3.89V) */
    s32  temperature_c;     /* battery cell temperature in Celsius */
    bool charging;          /* currently charging? */
    PsmChargerType charger_type;  /* Unconnected, EnoughPower, LowPower, NotSupported */
} hal_battery_reading_t;

Result hal_battery_init(void);
Result hal_battery_read(hal_battery_reading_t *out);
void   hal_battery_exit(void);

#endif /* HAL_BATTERY_H */
