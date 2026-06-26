#include "smonitor_modem.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
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

#ifndef CONFIG_SMONITOR_MODEM_GPS_ENABLE_ANTENNA_POWER
#define CONFIG_SMONITOR_MODEM_GPS_ENABLE_ANTENNA_POWER 1
#endif

#ifndef CONFIG_SMONITOR_MODEM_GPS_INITIAL_DELAY_MS
#define CONFIG_SMONITOR_MODEM_GPS_INITIAL_DELAY_MS 15000
#endif

#ifndef CONFIG_SMONITOR_MODEM_GPS_READ_ATTEMPTS
#define CONFIG_SMONITOR_MODEM_GPS_READ_ATTEMPTS 15
#endif

#ifndef CONFIG_SMONITOR_MODEM_GPS_RETRY_DELAY_MS
#define CONFIG_SMONITOR_MODEM_GPS_RETRY_DELAY_MS 15000
#endif

static const char *TAG = "smonitor_modem";
static const EventBits_t CONNECTED_BIT = BIT0;
static const EventBits_t DISCONNECTED_BIT = BIT1;
static const EventBits_t FAILED_BIT = BIT2;

static smonitor_modem_state_t state = SMONITOR_MODEM_STATE_STOPPED;
static smonitor_modem_config_t modem_config;
static smonitor_modem_location_t cached_location;
static esp_modem_dce_t *dce;
static esp_netif_t *ppp_netif;
static EventGroupHandle_t events;
static int32_t last_ppp_event;

typedef struct {
    const char *name;
    esp_err_t (*configure_radio)(void);
    esp_err_t (*read_location)(smonitor_modem_location_t *location);
} smonitor_modem_model_ops_t;

typedef struct {
    int run_status;
    int fix_status;
    double latitude;
    double longitude;
    double hdop;
    double pdop;
    double vdop;
    int satellites_in_view;
    int gps_satellites_used;
    int glonass_satellites_used;
    int cn0_max;
    bool has_hdop;
    bool has_pdop;
    bool has_vdop;
    bool has_satellites_in_view;
    bool has_gps_satellites_used;
    bool has_glonass_satellites_used;
    bool has_cn0_max;
} sim7000_gnss_info_t;

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

static const char *next_csv_field(const char **cursor, char *field,
                                  size_t field_size)
{
    const char *start = *cursor;
    if (start == NULL || *start == '\0' || *start == '\r' || *start == '\n') {
        return NULL;
    }

    const char *end = start;
    while (*end != '\0' && *end != ',' && *end != '\r' && *end != '\n') {
        ++end;
    }

    const size_t length = end - start;
    const size_t copy_length =
        length < field_size - 1 ? length : field_size - 1;
    memcpy(field, start, copy_length);
    field[copy_length] = '\0';

    *cursor = *end == ',' ? end + 1 : end;
    return field;
}

static bool parse_int_field(const char *field, int *value)
{
    if (field == NULL || field[0] == '\0') {
        return false;
    }
    char *end = NULL;
    const long parsed = strtol(field, &end, 10);
    if (end == field) {
        return false;
    }
    *value = (int)parsed;
    return true;
}

static bool parse_double_field(const char *field, double *value)
{
    if (field == NULL || field[0] == '\0') {
        return false;
    }
    char *end = NULL;
    const double parsed = strtod(field, &end);
    if (end == field) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool parse_cgnsinf_response(const char *response,
                                   sim7000_gnss_info_t *info)
{
    const char *line = strstr(response, "+CGNSINF:");
    if (line == NULL) {
        return false;
    }

    line += strlen("+CGNSINF:");
    while (*line == ' ') {
        ++line;
    }

    memset(info, 0, sizeof(*info));

    const char *cursor = line;
    char field_value[32] = {};
    for (int field = 0;
         next_csv_field(&cursor, field_value, sizeof(field_value)) != NULL;
         ++field) {
        switch (field) {
        case 0:
            parse_int_field(field_value, &info->run_status);
            break;
        case 1:
            parse_int_field(field_value, &info->fix_status);
            break;
        case 3:
            parse_double_field(field_value, &info->latitude);
            break;
        case 4:
            parse_double_field(field_value, &info->longitude);
            break;
        case 10:
            info->has_hdop = parse_double_field(field_value, &info->hdop);
            break;
        case 11:
            info->has_pdop = parse_double_field(field_value, &info->pdop);
            break;
        case 12:
            info->has_vdop = parse_double_field(field_value, &info->vdop);
            break;
        case 14:
            info->has_satellites_in_view =
                parse_int_field(field_value, &info->satellites_in_view);
            break;
        case 15:
            info->has_gps_satellites_used =
                parse_int_field(field_value, &info->gps_satellites_used);
            break;
        case 16:
            info->has_glonass_satellites_used =
                parse_int_field(field_value, &info->glonass_satellites_used);
            break;
        case 18:
            info->has_cn0_max =
                parse_int_field(field_value, &info->cn0_max);
            break;
        default:
            break;
        }
    }

    return true;
}

static bool cgnsinf_has_fix(const sim7000_gnss_info_t *info,
                            smonitor_modem_location_t *location)
{
    if (info->run_status != 1 || info->fix_status != 1) {
        return false;
    }

    location->latitude = info->latitude;
    location->longitude = info->longitude;
    location->valid = true;
    return true;
}

static void log_gnss_diagnostics(int attempt, const sim7000_gnss_info_t *info)
{
    const int satellites_in_view =
        info->has_satellites_in_view ? info->satellites_in_view : -1;
    const int gps_used =
        info->has_gps_satellites_used ? info->gps_satellites_used : -1;
    const int glonass_used =
        info->has_glonass_satellites_used ? info->glonass_satellites_used : -1;
    const int cn0_max = info->has_cn0_max ? info->cn0_max : -1;
    const double hdop = info->has_hdop ? info->hdop : -1.0;
    const double pdop = info->has_pdop ? info->pdop : -1.0;
    const double vdop = info->has_vdop ? info->vdop : -1.0;

    ESP_LOGI(TAG,
             "GNSS status (%d/%d): run=%d fix=%d, sats_view=%d, gps_used=%d, "
             "glonass_used=%d, cn0_max=%d, hdop=%.1f, pdop=%.1f, vdop=%.1f",
             attempt, CONFIG_SMONITOR_MODEM_GPS_READ_ATTEMPTS,
             info->run_status, info->fix_status, satellites_in_view,
             gps_used, glonass_used, cn0_max, hdop, pdop, vdop);
}

static esp_err_t sim7000_read_location(smonitor_modem_location_t *location)
{
    ESP_RETURN_ON_FALSE(location != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Location output is required");
    memset(location, 0, sizeof(*location));

#if CONFIG_SMONITOR_MODEM_GPS_ENABLE_ANTENNA_POWER
    esp_err_t antenna_power_result =
        run_at_retry("AT+SGPIO=0,4,1,1\r", 3);
    if (antenna_power_result == ESP_OK) {
        ESP_LOGI(TAG, "SIM7000 GPS antenna power enabled");
    } else {
        ESP_LOGW(TAG, "Failed to enable SIM7000 GPS antenna power: %s",
                 esp_err_to_name(antenna_power_result));
    }
#endif

    ESP_RETURN_ON_ERROR(run_at_retry("AT+CGNSPWR=1\r", 3), TAG,
                        "Failed to enable SIM7000 GNSS");

    if (CONFIG_SMONITOR_MODEM_GPS_INITIAL_DELAY_MS > 0) {
        ESP_LOGI(TAG, "Waiting %" PRIu32 " ms for GNSS startup",
                 (uint32_t)CONFIG_SMONITOR_MODEM_GPS_INITIAL_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SMONITOR_MODEM_GPS_INITIAL_DELAY_MS));
    }

    esp_err_t result = ESP_FAIL;
    for (int attempt = 1;
         attempt <= CONFIG_SMONITOR_MODEM_GPS_READ_ATTEMPTS;
         ++attempt) {
        char response[256] = {};
        result = esp_modem_at(dce, "AT+CGNSINF\r", response, 3000);
        if (result == ESP_OK) {
            sim7000_gnss_info_t gnss_info = {};
            if (parse_cgnsinf_response(response, &gnss_info)) {
                log_gnss_diagnostics(attempt, &gnss_info);
                if (cgnsinf_has_fix(&gnss_info, location)) {
                    ESP_LOGI(TAG,
                             "GPS location: latitude=%.6f, longitude=%.6f",
                             location->latitude, location->longitude);
                    return ESP_OK;
                }
            } else {
                ESP_LOGW(TAG, "Failed to parse CGNSINF response: %s",
                         response);
            }
        } else {
            ESP_LOGW(TAG, "CGNSINF failed (%d/%d): %s", attempt,
                     CONFIG_SMONITOR_MODEM_GPS_READ_ATTEMPTS,
                     esp_err_to_name(result));
        }

        ESP_LOGI(TAG, "Waiting for GPS location... (%d/%d)", attempt,
                 CONFIG_SMONITOR_MODEM_GPS_READ_ATTEMPTS);
        if (attempt < CONFIG_SMONITOR_MODEM_GPS_READ_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_SMONITOR_MODEM_GPS_RETRY_DELAY_MS));
        }
    }

    memset(location, 0, sizeof(*location));
    location->valid = false;
    return result == ESP_OK ? ESP_ERR_TIMEOUT : result;
}

static esp_err_t sim7000_configure_radio(void)
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

static const smonitor_modem_model_ops_t sim7000_model = {
    .name = "SIM7000",
    .configure_radio = sim7000_configure_radio,
    .read_location = sim7000_read_location,
};

static const smonitor_modem_model_ops_t *model_ops(void)
{
#if CONFIG_SMONITOR_MODEM_MODEL_SIM7000
    return &sim7000_model;
#else
    return NULL;
#endif
}

static void cache_startup_location(const smonitor_modem_model_ops_t *ops)
{
    memset(&cached_location, 0, sizeof(cached_location));

    if (ops == NULL || ops->read_location == NULL) {
        ESP_LOGW(TAG, "GPS location is not supported by this modem model");
        return;
    }

    const esp_err_t result = ops->read_location(&cached_location);
    if (result != ESP_OK) {
        memset(&cached_location, 0, sizeof(cached_location));
        cached_location.valid = false;
        ESP_LOGW(TAG, "GPS location not available: %s; using 0,0",
                 esp_err_to_name(result));
    }
}

esp_err_t smonitor_modem_init(const smonitor_modem_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Modem config is required");
    ESP_RETURN_ON_FALSE(config->apn != NULL && config->apn[0] != '\0',
                        ESP_ERR_INVALID_ARG, TAG, "APN is required");

    state = SMONITOR_MODEM_STATE_STARTING;
    modem_config = *config;
    memset(&cached_location, 0, sizeof(cached_location));

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

    const smonitor_modem_model_ops_t *ops = model_ops();
    ESP_RETURN_ON_FALSE(ops != NULL, ESP_ERR_NOT_SUPPORTED, TAG,
                        "Unsupported modem model");

    ESP_LOGI(TAG, "Initializing esp_modem for %s", ops->name);
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
    ESP_RETURN_ON_ERROR(ops->configure_radio(), TAG,
                        "Failed to configure modem radio profile");
    cache_startup_location(ops);

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

esp_err_t smonitor_modem_get_location(smonitor_modem_location_t *location)
{
    ESP_RETURN_ON_FALSE(location != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Location output is required");
    *location = cached_location;
    return cached_location.valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}
