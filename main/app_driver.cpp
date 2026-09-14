/*
   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_log.h>

#include <esp_matter.h>
#include <app_priv.h>

#include <iot_button.h>
#include <button_gpio.h>

static const char *TAG = "app_driver";

/* Board's dedicated "Matter" button. */
#define MATTER_RESET_BUTTON_GPIO   23

/* Logged on every press-down so the button/GPIO can be verified in the console. */
static void button_down_cb(void *arg, void *data)
{
    ESP_LOGW(TAG, "GPIO%d button pressed (down)", MATTER_RESET_BUTTON_GPIO);
}

/* Single click -> erase the Matter pairing (factory reset) and reboot. */
static void button_click_cb(void *arg, void *data)
{
    ESP_LOGW(TAG, "GPIO%d single click -> factory reset: erasing Matter pairing and rebooting",
             MATTER_RESET_BUTTON_GPIO);
    esp_matter::factory_reset();
}

app_driver_handle_t app_driver_button_init()
{
    button_handle_t handle = NULL;
    const button_config_t btn_cfg = {0};
    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num = MATTER_RESET_BUTTON_GPIO,
        .active_level = 0,          /* button wired to GND: pressed = low, internal pull-up */
    };

    if (iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create button on GPIO%d", MATTER_RESET_BUTTON_GPIO);
        return NULL;
    }

    iot_button_register_cb(handle, BUTTON_PRESS_DOWN,   NULL, button_down_cb,  NULL);
    iot_button_register_cb(handle, BUTTON_SINGLE_CLICK, NULL, button_click_cb, NULL);

    ESP_LOGI(TAG, "Reset button ready on GPIO%d (single click -> factory reset; presses are logged)",
             MATTER_RESET_BUTTON_GPIO);
    return (app_driver_handle_t)handle;
}
