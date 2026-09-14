/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "p1_reader.h"
#include "dsmr_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sdkconfig.h>
#include <esp_log.h>
#include <esp_check.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "p1_reader";

#define P1_UART_NUM       ((uart_port_t)CONFIG_P1_UART_NUM)
#define P1_RX_GPIO        (CONFIG_P1_UART_RX_GPIO)
#define P1_REQUEST_GPIO   ((gpio_num_t)CONFIG_P1_REQUEST_GPIO)
#define P1_BAUD           (CONFIG_P1_UART_BAUD_RATE)

#if CONFIG_P1_PASSTHROUGH
#define P1_TX_GPIO        (CONFIG_P1_TX_GPIO)
#if CONFIG_P1_PASSTHROUGH_HONOR_REQUEST
#define P1_TXREQ_GPIO     ((gpio_num_t)CONFIG_P1_TXREQ_GPIO)
#endif
#endif

#define P1_UART_RX_BUF    2048
#define P1_TELEGRAM_MAX   3072

/* Temporary raw-RX diagnostic. Logs bytes-per-2s + a hex sample so we can tell
 * "0 bytes = request line/wiring/meter" from "garbage = inversion/baud" from
 * "valid '/' ASCII = framing/parser". Set to 0 once the P1 link works. */
#define P1_RX_DEBUG       1

static p1_telegram_cb_t s_cb = NULL;

/* Validate (optionally) and parse one framed telegram.
 * `telegram` spans '/' .. trailing newline; `bang_pos` is the index of '!'. */
static void p1_process(char *telegram, size_t total_len, int bang_pos)
{
#if CONFIG_P1_VERIFY_CRC
    /* CRC is computed over '/' .. '!' inclusive = [0, bang_pos], and the four
     * hex characters that follow '!' hold the expected value. */
    if (bang_pos + 4 < (int)total_len) {
        char crc_str[5] = {0};
        memcpy(crc_str, telegram + bang_pos + 1, 4);
        unsigned int expected = (unsigned int)strtoul(crc_str, NULL, 16);
        uint16_t actual = dsmr_crc16((const unsigned char *)telegram, (size_t)(bang_pos + 1));
        if ((uint16_t)expected != actual) {
            ESP_LOGW(TAG, "CRC mismatch (telegram %04X, calculated %04X) - dropping", expected, actual);
            return;
        }
    } else {
        ESP_LOGW(TAG, "Telegram has no CRC field - dropping");
        return;
    }
#endif

#if CONFIG_P1_PASSTHROUGH
    /* Relay the exact telegram (including CRC + CRLF) out the second P1 port so
     * a downstream device reads the same data the meter sent. */
#if CONFIG_P1_PASSTHROUGH_HONOR_REQUEST
    if (gpio_get_level(P1_TXREQ_GPIO))
#endif
    {
        uart_write_bytes(P1_UART_NUM, telegram, total_len);
    }
#endif

    dsmr_data_t data;
    if (!dsmr_parse(telegram, total_len, &data)) {
        ESP_LOGW(TAG, "No known OBIS values in telegram");
        return;
    }

    ESP_LOGI(TAG,
             "P1: Pimp=%.3fkW Pexp=%.3fkW Eimp=%.3fkWh Eexp=%.3fkWh U1=%.1fV I1=%.1fA phases=%d",
             data.power_import_kw, data.power_export_kw,
             data.energy_import_kwh, data.energy_export_kwh,
             data.has_voltage[0] ? data.voltage_v[0] : 0.0,
             data.has_current[0] ? data.current_a[0] : 0.0,
             data.phase_count);

    if (s_cb) {
        s_cb(&data);
    }
}

static void p1_reader_task(void *arg)
{
    uint8_t *rx = (uint8_t *)malloc(P1_UART_RX_BUF);
    char    *tg = (char *)malloc(P1_TELEGRAM_MAX);
    if (!rx || !tg) {
        ESP_LOGE(TAG, "Failed to allocate reader buffers");
        free(rx);
        free(tg);
        vTaskDelete(NULL);
        return;
    }

    size_t tg_len = 0;
    bool   capturing = false;
    int    bang_pos = -1;

#if P1_RX_DEBUG
    uint32_t dbg_bytes = 0;
    int      dbg_reads = 0;
    uint8_t  dbg_sample[64];
    int      dbg_sample_len = 0;
    int      dbg_max_read = 0;
#endif

    while (true) {
        int n = uart_read_bytes(P1_UART_NUM, rx, P1_UART_RX_BUF, pdMS_TO_TICKS(200));

#if P1_RX_DEBUG
        if (n > 0) {
            dbg_bytes += n;
            if (n > dbg_max_read) dbg_max_read = n;
            if (dbg_sample_len == 0) {          /* capture the start of the burst */
                dbg_sample_len = n < 64 ? n : 64;
                memcpy(dbg_sample, rx, dbg_sample_len);
            }
        }
        if (++dbg_reads >= 10) {          /* ~every 2 s (200 ms timeout x 10) */
            if (dbg_bytes == 0) {
                ESP_LOGW(TAG, "RAW RX: 0 bytes/2s on GPIO%d", P1_RX_GPIO);
            } else {
                char hex[64 * 3 + 1] = {0};
                for (int j = 0; j < dbg_sample_len; j++) {
                    snprintf(hex + j * 3, 4, "%02X ", dbg_sample[j]);
                }
                ESP_LOGW(TAG, "RAW RX: %u bytes/2s (max single read %d); start: %s",
                         (unsigned)dbg_bytes, dbg_max_read, hex);
            }
            dbg_bytes = 0;
            dbg_reads = 0;
            dbg_sample_len = 0;
            dbg_max_read = 0;
        }
#endif

        for (int i = 0; i < n; i++) {
            char c = (char)rx[i];

            if (c == '/') {            /* start-of-telegram marker */
                capturing = true;
                tg_len = 0;
                bang_pos = -1;
            }
            if (!capturing) {
                continue;
            }
            if (tg_len >= P1_TELEGRAM_MAX - 1) {   /* runaway frame, drop it */
                capturing = false;
                continue;
            }
            tg[tg_len++] = c;

            if (c == '!' && bang_pos < 0) {
                bang_pos = (int)tg_len - 1;        /* CRC + CRLF still to come */
            } else if (bang_pos >= 0 && c == '\n') {
                tg[tg_len] = '\0';
                p1_process(tg, tg_len, bang_pos);
                capturing = false;
            }
        }
    }
}

esp_err_t p1_reader_start(p1_telegram_cb_t cb)
{
    s_cb = cb;

    /* Drive the data-request line so a DSMR 4/5 meter streams a telegram each
     * second. Through the dongle's opto-isolator a high level = "send data". */
    gpio_config_t req_io = {
        .pin_bit_mask = 1ULL << P1_REQUEST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&req_io), TAG, "request gpio config failed");
#if CONFIG_P1_REQUEST_ACTIVE
    gpio_set_level(P1_REQUEST_GPIO, 1);
#else
    gpio_set_level(P1_REQUEST_GPIO, 0);
#endif

#if CONFIG_P1_PASSTHROUGH && CONFIG_P1_PASSTHROUGH_HONOR_REQUEST
    /* Sense the downstream device's data-request line */
    gpio_config_t txreq_io = {
        .pin_bit_mask = 1ULL << P1_TXREQ_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&txreq_io), TAG, "txreq gpio config failed");
#endif

    uart_config_t uart_config = {
        .baud_rate = P1_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
#if CONFIG_P1_PASSTHROUGH
    const int tx_buf_size = P1_UART_RX_BUF;   /* buffered (non-blocking) TX for relay */
    const int tx_pin = P1_TX_GPIO;
#else
    const int tx_buf_size = 0;
    const int tx_pin = UART_PIN_NO_CHANGE;
#endif
    ESP_RETURN_ON_ERROR(uart_driver_install(P1_UART_NUM, P1_UART_RX_BUF * 2, tx_buf_size, 0, NULL, 0),
                        TAG, "uart_driver_install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(P1_UART_NUM, &uart_config), TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(P1_UART_NUM, tx_pin, P1_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin failed");

#if CONFIG_P1_INVERT_RX
    /* No on-board inverting transistor: flip the raw open-collector P1 signal
     * to normal UART polarity in software. */
    ESP_RETURN_ON_ERROR(uart_set_line_inverse(P1_UART_NUM, UART_SIGNAL_RXD_INV),
                        TAG, "uart_set_line_inverse failed");
#endif

    if (xTaskCreate(p1_reader_task, "p1_reader", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create reader task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "P1 reader started: UART%d RX=GPIO%d REQ=GPIO%d @ %d baud",
             (int)P1_UART_NUM, P1_RX_GPIO, (int)P1_REQUEST_GPIO, P1_BAUD);
#if CONFIG_P1_INVERT_RX
    ESP_LOGI(TAG, "P1 RX software inversion ENABLED (UART_SIGNAL_RXD_INV)");
#endif
#if CONFIG_P1_PASSTHROUGH
    ESP_LOGI(TAG, "P1 passthrough enabled: relaying telegrams out TX=GPIO%d", P1_TX_GPIO);
#endif
    return ESP_OK;
}
