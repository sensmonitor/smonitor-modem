#include "smonitor_modem.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "smonitor_modem_internal.h"

static const char *TAG = "smonitor_modem";
static const EventBits_t CONNECTED_BIT = BIT0;
static const EventBits_t DISCONNECTED_BIT = BIT1;
static const EventBits_t FAILED_BIT = BIT2;

static smonitor_modem_state_t state = SMONITOR_MODEM_STATE_STOPPED;
static smonitor_modem_config_t modem_config;
static esp_modem_dce_t *dce;
static esp_netif_t *ppp_netif;
static EventGroupHandle_t events;
static int32_t last_ppp_event;

static const char *ppp_event_name(int32_t event_id)
{
    switch (event_id) {
    case NETIF_PPP_ERRORNONE:
        return "no error";
    case NETIF_PPP_ERRORPARAM:
        return "invalid parameter";
    case NETIF_PPP_ERROROPEN:
        return "unable to open session";
    case NETIF_PPP_ERRORDEVICE:
        return "invalid I/O device";
    case NETIF_PPP_ERRORALLOC:
        return "allocation failure";
    case NETIF_PPP_ERRORUSER:
        return "user interrupt";
    case NETIF_PPP_ERRORCONNECT:
        return "connection lost";
    case NETIF_PPP_ERRORAUTHFAIL:
        return "authentication failed";
    case NETIF_PPP_ERRORPROTOCOL:
        return "protocol negotiation failed";
    case NETIF_PPP_ERRORPEERDEAD:
        return "peer timeout";
    case NETIF_PPP_ERRORIDLETIMEOUT:
        return "idle timeout";
    case NETIF_PPP_ERRORCONNECTTIME:
        return "connect timeout";
    case NETIF_PPP_ERRORLOOPBACK:
        return "loopback detected";
    case NETIF_PPP_PHASE_DEAD:
        return "phase dead";
    case NETIF_PPP_PHASE_INITIALIZE:
        return "phase initialize";
    case NETIF_PPP_PHASE_SERIALCONN:
        return "phase serial connected";
    case NETIF_PPP_PHASE_ESTABLISH:
        return "phase establish";
    case NETIF_PPP_PHASE_AUTHENTICATE:
        return "phase authenticate";
    case NETIF_PPP_PHASE_NETWORK:
        return "phase network";
    case NETIF_PPP_PHASE_RUNNING:
        return "phase running";
    case NETIF_PPP_PHASE_TERMINATE:
        return "phase terminate";
    case NETIF_PPP_PHASE_DISCONNECT:
        return "phase disconnect";
    case NETIF_PPP_CONNECT_FAILED:
        return "modem PPP connect failed";
    default:
        return "unknown";
    }
}

static void on_ppp_changed(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;
    last_ppp_event = event_id;
    if (event_id > NETIF_PPP_ERRORNONE &&
        event_id < NETIF_PP_PHASE_OFFSET) {
        ESP_LOGE(TAG, "PPP error %" PRIi32 ": %s", event_id,
                 ppp_event_name(event_id));
        xEventGroupSetBits(events, FAILED_BIT);
        return;
    }
    if (event_id == NETIF_PPP_CONNECT_FAILED) {
        ESP_LOGE(TAG, "PPP error %" PRIi32 ": %s", event_id,
                 ppp_event_name(event_id));
        xEventGroupSetBits(events, FAILED_BIT);
        return;
    }
    ESP_LOGI(TAG, "PPP event %" PRIi32 ": %s", event_id,
             ppp_event_name(event_id));
}

static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id == IP_EVENT_PPP_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        esp_netif_dns_info_t dns = {};

        ESP_LOGI(TAG, "Modem connected to PPP server");
        ESP_LOGI(TAG, "IP          : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Netmask     : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway     : " IPSTR, IP2STR(&event->ip_info.gw));
        if (esp_netif_get_dns_info(event->esp_netif, ESP_NETIF_DNS_MAIN,
                                   &dns) == ESP_OK) {
            ESP_LOGI(TAG, "Name Server: " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
        state = SMONITOR_MODEM_STATE_ONLINE;
        xEventGroupClearBits(events, DISCONNECTED_BIT);
        xEventGroupSetBits(events, CONNECTED_BIT);
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "Modem disconnected from PPP server");
        state = SMONITOR_MODEM_STATE_CONNECTING;
        xEventGroupClearBits(events, CONNECTED_BIT);
        xEventGroupSetBits(events, DISCONNECTED_BIT);
    }
}

static esp_err_t run_at_retry(const char *command, int attempts)
{
    char response[128] = {};
    esp_err_t result = ESP_FAIL;

    for (int attempt = 1; attempt <= attempts; ++attempt) {
        result = esp_modem_at(dce, command, response, 3000);
        if (result == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "AT command failed (%d/%d): %s", attempt, attempts,
                 command);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return result;
}

static esp_err_t configure_radio(void)
{
    char command[48] = {};
    ESP_RETURN_ON_ERROR(run_at_retry("AT+CEREG=0\r", 3), TAG,
                        "Failed to reset network registration reporting");
    ESP_RETURN_ON_ERROR(run_at_retry("AT+CFUN=1\r", 3), TAG,
                        "Failed to enable full modem functionality");

    snprintf(command, sizeof(command), "AT+IPR=%d\r",
             CONFIG_SMONITOR_MODEM_UART_BAUD_RATE);
    ESP_RETURN_ON_ERROR(run_at_retry(command, 3), TAG,
                        "Failed to configure modem UART rate");
    ESP_RETURN_ON_ERROR(run_at_retry("AT+COPS=0\r", 3), TAG,
                        "Failed to enable automatic operator selection");

    /*
     * Keep both band profiles aligned with the known-good modem_console
     * sequence. SIM7000 stores CAT-M and NB-IoT masks independently.
     */
    snprintf(command, sizeof(command), "AT+CBANDCFG=\"CAT-M\",%d\r",
             CONFIG_SMONITOR_MODEM_LTE_BAND);
    ESP_RETURN_ON_ERROR(run_at_retry(command, 3), TAG,
                        "Failed to configure CAT-M band");
    snprintf(command, sizeof(command), "AT+CBANDCFG=\"NB-IOT\",%d\r",
             CONFIG_SMONITOR_MODEM_LTE_BAND);
    ESP_RETURN_ON_ERROR(run_at_retry(command, 3), TAG,
                        "Failed to configure NB-IoT band");

    ESP_RETURN_ON_ERROR(run_at_retry("AT&D0\r", 3), TAG,
                        "Failed to configure DTR");
    ESP_RETURN_ON_ERROR(run_at_retry("AT+CPSMS=0\r", 3), TAG,
                        "Failed to disable PSM");
    ESP_RETURN_ON_ERROR(run_at_retry("AT+CSCLK=0\r", 3), TAG,
                        "Failed to disable sleep");
    ESP_RETURN_ON_ERROR(run_at_retry("AT+CEDRXS=0\r", 3), TAG,
                        "Failed to disable eDRX");

    if (modem_config.network == SMONITOR_MODEM_NETWORK_NB_IOT) {
        ESP_RETURN_ON_ERROR(run_at_retry("AT+CNMP=38\r", 3), TAG,
                            "Failed to select LTE mode");
        ESP_RETURN_ON_ERROR(run_at_retry("AT+CMNB=2\r", 3), TAG,
                            "Failed to select NB-IoT");
    } else if (modem_config.network == SMONITOR_MODEM_NETWORK_LTE_M) {
        ESP_RETURN_ON_ERROR(run_at_retry("AT+CNMP=38\r", 3), TAG,
                            "Failed to select LTE mode");
        ESP_RETURN_ON_ERROR(run_at_retry("AT+CMNB=1\r", 3), TAG,
                            "Failed to select LTE-M");
    } else if (modem_config.network == SMONITOR_MODEM_NETWORK_GPRS) {
        ESP_RETURN_ON_ERROR(run_at_retry("AT+CNMP=13\r", 3), TAG,
                            "Failed to select GSM mode");
    } else {
        ESP_RETURN_ON_ERROR(run_at_retry("AT+CNMP=2\r", 3), TAG,
                            "Failed to select automatic mode");
    }

    ESP_RETURN_ON_ERROR(run_at_retry("AT+CFUN=1\r", 3), TAG,
                        "Failed to re-enable full modem functionality");
    ESP_RETURN_ON_ERROR(run_at_retry("AT+CEREG=4\r", 3), TAG,
                        "Failed to enable network registration reporting");
    return ESP_OK;
}

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

    esp_err_t result = esp_netif_init();
    ESP_RETURN_ON_FALSE(result == ESP_OK || result == ESP_ERR_INVALID_STATE,
                        result, TAG, "Failed to initialize esp_netif");
    result = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(result == ESP_OK || result == ESP_ERR_INVALID_STATE,
                        result, TAG, "Failed to create event loop");

    events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(events != NULL, ESP_ERR_NO_MEM, TAG,
                        "Failed to create modem event group");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event,
                                   NULL),
        TAG, "Failed to register IP events");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                   on_ppp_changed, NULL),
        TAG, "Failed to register PPP events");

    const esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_PPP();
    ppp_netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(ppp_netif != NULL, ESP_ERR_NO_MEM, TAG,
                        "Failed to create PPP network interface");

    if (config->username != NULL && config->username[0] != '\0') {
        ESP_RETURN_ON_ERROR(
            esp_netif_ppp_set_auth(ppp_netif, NETIF_PPP_AUTHTYPE_PAP,
                                   config->username,
                                   config->password != NULL
                                       ? config->password
                                       : ""),
            TAG, "Failed to configure PPP PAP authentication");
        ESP_LOGI(TAG, "PPP authentication: PAP");
    } else {
        ESP_RETURN_ON_ERROR(
            esp_netif_ppp_set_auth(ppp_netif, NETIF_PPP_AUTHTYPE_NONE,
                                   "", ""),
            TAG, "Failed to disable PPP authentication");
        ESP_LOGI(TAG, "PPP authentication: none");
    }

    ESP_LOGI(TAG, "Configured SIM7000 on LilyGO T-SIM7000G, APN=%s",
             modem_config.apn);
    return ESP_OK;
}

esp_err_t smonitor_modem_connect(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(state == SMONITOR_MODEM_STATE_STARTING,
                        ESP_ERR_INVALID_STATE, TAG,
                        "Initialize modem before connecting");

    esp_modem_dce_config_t dce_config =
        ESP_MODEM_DCE_DEFAULT_CONFIG(modem_config.apn);
    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.tx_io_num = CONFIG_SMONITOR_MODEM_UART_TX_PIN;
    dte_config.uart_config.rx_io_num = CONFIG_SMONITOR_MODEM_UART_RX_PIN;
    dte_config.uart_config.rts_io_num = CONFIG_SMONITOR_MODEM_UART_RTS_PIN;
    dte_config.uart_config.cts_io_num = CONFIG_SMONITOR_MODEM_UART_CTS_PIN;
    dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
    dte_config.uart_config.baud_rate = CONFIG_SMONITOR_MODEM_UART_BAUD_RATE;
    dte_config.uart_config.rx_buffer_size =
        CONFIG_SMONITOR_MODEM_UART_RX_BUFFER_SIZE;
    dte_config.uart_config.tx_buffer_size =
        CONFIG_SMONITOR_MODEM_UART_TX_BUFFER_SIZE;
    dte_config.dte_buffer_size =
        CONFIG_SMONITOR_MODEM_UART_RX_BUFFER_SIZE / 2;

    ESP_LOGI(TAG, "Power on the modem");
    ESP_RETURN_ON_ERROR(smonitor_lilygo_t_sim7000g_power_on(), TAG,
                        "Failed to power on modem");

    ESP_LOGI(TAG, "Initializing esp_modem for SIM7000");
    dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7000, &dte_config, &dce_config,
                            ppp_netif);
    ESP_RETURN_ON_FALSE(dce != NULL, ESP_FAIL, TAG,
                        "Failed to create SIM7000 DCE");
    ESP_RETURN_ON_ERROR(esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND), TAG,
                        "Failed to enter command mode");

    state = SMONITOR_MODEM_STATE_REGISTERING;
    esp_err_t result = ESP_FAIL;
    for (int attempt = 1; attempt <= 30; ++attempt) {
        result = esp_modem_sync(dce);
        if (result == ESP_OK) {
            break;
        }
        ESP_LOGI(TAG, "Waiting for modem AT sync (%d/30)", attempt);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_RETURN_ON_ERROR(result, TAG, "Modem did not respond to AT commands");
    ESP_RETURN_ON_ERROR(configure_radio(), TAG,
                        "Failed to configure SIM7000 radio profile");

    int rssi = 0;
    int ber = 0;
    if (esp_modem_get_signal_quality(dce, &rssi, &ber) == ESP_OK) {
        ESP_LOGI(TAG, "Signal quality: rssi=%d, ber=%d", rssi, ber);
    }

    state = SMONITOR_MODEM_STATE_CONNECTING;
    last_ppp_event = NETIF_PPP_ERRORNONE;
    xEventGroupClearBits(events,
                         CONNECTED_BIT | DISCONNECTED_BIT | FAILED_BIT);
    ESP_RETURN_ON_ERROR(esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA), TAG,
                        "Failed to enter PPP data mode");
    ESP_LOGI(TAG, "Waiting for PPP IP address");

    const EventBits_t bits = xEventGroupWaitBits(
        events, CONNECTED_BIT | FAILED_BIT, pdFALSE, pdFALSE,
        timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
    if ((bits & CONNECTED_BIT) != 0) {
        return ESP_OK;
    }
    if ((bits & FAILED_BIT) != 0) {
        state = SMONITOR_MODEM_STATE_ERROR;
        ESP_LOGE(TAG, "PPP negotiation stopped: %s",
                 ppp_event_name(last_ppp_event));
        return ESP_FAIL;
    }
    state = SMONITOR_MODEM_STATE_ERROR;
    ESP_LOGE(TAG, "PPP IP timeout after %" PRIu32 " ms; last event: %s",
             timeout_ms, ppp_event_name(last_ppp_event));
    return ESP_ERR_TIMEOUT;
}

esp_err_t smonitor_modem_disconnect(void)
{
    if (dce != NULL) {
        esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
        esp_modem_destroy(dce);
        dce = NULL;
    }
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
    ESP_RETURN_ON_FALSE(dce != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "Modem is not initialized");
    memset(signal, 0, sizeof(*signal));
    esp_err_t result =
        esp_modem_get_signal_quality(dce, &signal->rssi, &signal->ber);
    signal->registered = state == SMONITOR_MODEM_STATE_ONLINE;
    return result;
}
