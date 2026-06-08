#include "smonitor_modem.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "smonitor_modem_internal.h"

static const char *TAG = "smonitor_modem";
static smonitor_modem_state_t state = SMONITOR_MODEM_STATE_STOPPED;
static smonitor_modem_config_t modem_config;

esp_err_t smonitor_modem_init(const smonitor_modem_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Modem config is required");
    ESP_RETURN_ON_FALSE(config->apn != NULL && config->apn[0] != '\0',
                        ESP_ERR_INVALID_ARG, TAG, "APN is required");

    state = SMONITOR_MODEM_STATE_STARTING;
    modem_config = *config;

    ESP_RETURN_ON_ERROR(smonitor_lilygo_t_sim7000g_power_init(), TAG,
                        "Failed to initialize board power control");

    ESP_LOGI(TAG, "Configured SIM7000 on LilyGO T-SIM7000G, APN=%s",
             modem_config.apn);
    return ESP_OK;
}

esp_err_t smonitor_modem_connect(uint32_t timeout_ms)
{
    (void)timeout_ms;
    ESP_RETURN_ON_FALSE(state == SMONITOR_MODEM_STATE_STARTING,
                        ESP_ERR_INVALID_STATE, TAG,
                        "Initialize modem before connecting");

    /*
     * PPP and esp_modem migration is intentionally the next implementation
     * step. Keeping this explicit prevents the scaffold from reporting a
     * connection that it has not established.
     */
    state = SMONITOR_MODEM_STATE_ERROR;
    ESP_LOGW(TAG, "PPP transport is not implemented in the initial scaffold");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t smonitor_modem_disconnect(void)
{
    state = SMONITOR_MODEM_STATE_STOPPED;
    return ESP_OK;
}

smonitor_modem_state_t smonitor_modem_get_state(void)
{
    return state;
}

esp_err_t smonitor_modem_get_signal(smonitor_modem_signal_t *signal)
{
    ESP_RETURN_ON_FALSE(signal != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Signal output is required");
    memset(signal, 0, sizeof(*signal));
    return ESP_ERR_NOT_SUPPORTED;
}
