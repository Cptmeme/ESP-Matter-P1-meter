/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parsed values extracted from a single DSMR/P1 telegram.
 * Each value has an accompanying `has_*` flag because a given meter only
 * emits a subset of the OBIS codes (e.g. single-phase meters omit L2/L3). */
typedef struct {
    bool   has_energy_import;   /* 1-0:1.8.1 + 1-0:1.8.2 (sum of both tariffs) */
    double energy_import_kwh;   /* total energy drawn from the grid [kWh]      */
    bool   has_energy_export;   /* 1-0:2.8.1 + 1-0:2.8.2 (sum of both tariffs) */
    double energy_export_kwh;   /* total energy delivered back to grid [kWh]   */

    bool   has_power_import;    /* 1-0:1.7.0  current import power [kW]         */
    double power_import_kw;
    bool   has_power_export;    /* 1-0:2.7.0  current export power [kW]         */
    double power_export_kw;

    bool   has_voltage[3];      /* 1-0:32.7.0 / 52.7.0 / 72.7.0  (L1/L2/L3) [V] */
    double voltage_v[3];
    bool   has_current[3];      /* 1-0:31.7.0 / 51.7.0 / 71.7.0  (L1/L2/L3) [A] */
    double current_a[3];

    int    phase_count;         /* 1 or 3, auto-detected from present codes     */
} dsmr_data_t;

/* Parse a complete telegram (the text from '/' up to and including the data,
 * CRC bytes may be present or not). Returns true when at least one known OBIS
 * value was found. `out` is fully zeroed first. */
bool dsmr_parse(const char *telegram, size_t len, dsmr_data_t *out);

/* DSMR CRC16 (reflected, polynomial 0xA001, init 0x0000), computed over
 * [data, data+len). For a telegram this is the range '/' .. '!' inclusive. */
uint16_t dsmr_crc16(const unsigned char *data, size_t len);

#ifdef __cplusplus
}
#endif
