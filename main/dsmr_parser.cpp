/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "dsmr_parser.h"

#include <string.h>
#include <stdlib.h>

uint16_t dsmr_crc16(const unsigned char *data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/* True when `line` begins with the OBIS `code` and the value group '(' follows
 * immediately. The '(' check prevents e.g. "1-0:21.7.0" matching "1-0:2.7.0". */
static bool obis_is(const char *line, const char *code)
{
    size_t n = strlen(code);
    return strncmp(line, code, n) == 0 && line[n] == '(';
}

/* Extract the first numeric value out of "...(value*unit)". */
static bool obis_value(const char *line, double *out)
{
    const char *p = strchr(line, '(');
    if (!p) {
        return false;
    }
    p++;
    char tmp[32];
    size_t i = 0;
    while (*p && *p != '*' && *p != ')' && i < sizeof(tmp) - 1) {
        tmp[i++] = *p++;
    }
    tmp[i] = '\0';
    if (i == 0) {
        return false;
    }
    *out = atof(tmp);
    return true;
}

bool dsmr_parse(const char *telegram, size_t len, dsmr_data_t *out)
{
    memset(out, 0, sizeof(*out));
    bool found = false;

    const char *p = telegram;
    const char *end = telegram + len;

    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        while (line_len > 0 && p[line_len - 1] == '\r') {
            line_len--;
        }

        char line[80];
        if (line_len < sizeof(line)) {
            memcpy(line, p, line_len);
            line[line_len] = '\0';

            double v;
            if (obis_is(line, "1-0:1.8.1") || obis_is(line, "1-0:1.8.2")) {
                if (obis_value(line, &v)) { out->energy_import_kwh += v; out->has_energy_import = true; found = true; }
            } else if (obis_is(line, "1-0:2.8.1") || obis_is(line, "1-0:2.8.2")) {
                if (obis_value(line, &v)) { out->energy_export_kwh += v; out->has_energy_export = true; found = true; }
            } else if (obis_is(line, "1-0:1.7.0")) {
                if (obis_value(line, &v)) { out->power_import_kw = v; out->has_power_import = true; found = true; }
            } else if (obis_is(line, "1-0:2.7.0")) {
                if (obis_value(line, &v)) { out->power_export_kw = v; out->has_power_export = true; found = true; }
            } else if (obis_is(line, "1-0:32.7.0")) {
                if (obis_value(line, &v)) { out->voltage_v[0] = v; out->has_voltage[0] = true; found = true; }
            } else if (obis_is(line, "1-0:52.7.0")) {
                if (obis_value(line, &v)) { out->voltage_v[1] = v; out->has_voltage[1] = true; found = true; }
            } else if (obis_is(line, "1-0:72.7.0")) {
                if (obis_value(line, &v)) { out->voltage_v[2] = v; out->has_voltage[2] = true; found = true; }
            } else if (obis_is(line, "1-0:31.7.0")) {
                if (obis_value(line, &v)) { out->current_a[0] = v; out->has_current[0] = true; found = true; }
            } else if (obis_is(line, "1-0:51.7.0")) {
                if (obis_value(line, &v)) { out->current_a[1] = v; out->has_current[1] = true; found = true; }
            } else if (obis_is(line, "1-0:71.7.0")) {
                if (obis_value(line, &v)) { out->current_a[2] = v; out->has_current[2] = true; found = true; }
            }
        }

        if (!nl) {
            break;
        }
        p = nl + 1;
    }

    out->phase_count = (out->has_voltage[1] || out->has_voltage[2] ||
                        out->has_current[1] || out->has_current[2]) ? 3 : 1;
    return found;
}
