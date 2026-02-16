/*
 * hal_temperature.h - Temperature sensor HAL module
 *
 * Reads temperatures via the TS (Temperature Sensor) service.
 * The Switch has a TMP451 sensor with two channels:
 *
 *   - Internal (TsLocation_Internal): PCB / board temperature
 *   - External (TsLocation_External): SoC die (CPU/GPU)
 *
 * The "external" channel measures the SoC because the TMP451's
 * remote diode input is connected to a thermal diode on the
 * Tegra X1 die — standard practice in SoC thermal management.
 */

#ifndef HAL_TEMPERATURE_H
#define HAL_TEMPERATURE_H

#include <switch.h>

typedef struct {
    s32 soc_celsius;    /* CPU/GPU die temperature */
    s32 pcb_celsius;    /* board / PCB temperature */
} hal_temperature_reading_t;

Result hal_temperature_init(void);
Result hal_temperature_read(hal_temperature_reading_t *out);
void   hal_temperature_exit(void);

#endif /* HAL_TEMPERATURE_H */
