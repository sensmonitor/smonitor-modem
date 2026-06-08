#include "smonitor_modem_internal.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

esp_err_t smonitor_lilygo_t_sim7000g_power_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << CONFIG_SMONITOR_MODEM_PWRKEY_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&config);
}

esp_err_t smonitor_lilygo_t_sim7000g_power_on(void)
{
    ESP_RETURN_ON_ERROR(
        gpio_set_level(CONFIG_SMONITOR_MODEM_PWRKEY_PIN, 1),
        "smonitor_modem",
        "Failed to assert PWRKEY");
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_RETURN_ON_ERROR(
        gpio_set_level(CONFIG_SMONITOR_MODEM_PWRKEY_PIN, 0),
        "smonitor_modem",
        "Failed to release PWRKEY");
    vTaskDelay(pdMS_TO_TICKS(2000));
    return ESP_OK;
}
