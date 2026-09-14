/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include "dsmr_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Invoked for every successfully decoded telegram. Runs in the reader task
 * context, NOT the Matter task -- do not call CHIP APIs directly from here. */
typedef void (*p1_telegram_cb_t)(const dsmr_data_t *data);

/* Configure the P1 UART + data-request line and start the reader task.
 * Pins, baud rate and CRC behaviour come from Kconfig. */
esp_err_t p1_reader_start(p1_telegram_cb_t cb);

#ifdef __cplusplus
}
#endif
