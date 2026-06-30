#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SMONITOR_MODEM_STATE_STOPPED = 0,
    SMONITOR_MODEM_STATE_STARTING,
    SMONITOR_MODEM_STATE_REGISTERING,
    SMONITOR_MODEM_STATE_CONNECTING,
    SMONITOR_MODEM_STATE_ONLINE,
    SMONITOR_MODEM_STATE_ERROR,
} smonitor_modem_state_t;

typedef enum {
    SMONITOR_MODEM_NETWORK_AUTO = 0,
    SMONITOR_MODEM_NETWORK_LTE_M,
    SMONITOR_MODEM_NETWORK_NB_IOT,
    SMONITOR_MODEM_NETWORK_GPRS,
} smonitor_modem_network_t;

typedef enum {
    SMONITOR_MODEM_MODEL_SIM7000 = 0,
    SMONITOR_MODEM_MODEL_SIM7080,
    SMONITOR_MODEM_MODEL_SIM7600,
} smonitor_modem_model_t;

typedef esp_err_t (*smonitor_modem_power_callback_t)(void *context);

typedef struct {
    int tx_pin;
    int rx_pin;
    int rts_pin;
    int cts_pin;
    int baud_rate;
    int rx_buffer_size;
    int tx_buffer_size;
} smonitor_modem_uart_config_t;

typedef struct {
    const char *apn;
    const char *username;
    const char *password;
    smonitor_modem_network_t network;
    smonitor_modem_model_t model;
    smonitor_modem_uart_config_t uart;
    smonitor_modem_power_callback_t power_init;
    smonitor_modem_power_callback_t power_on;
    void *power_context;
} smonitor_modem_config_t;

typedef struct {
    smonitor_modem_model_t model;
    const char *name;
    bool supports_gprs;
    bool supports_lte_m;
    bool supports_nb_iot;
    bool supports_psm;
    bool supports_edrx;
    bool supports_gnss;
    int pwrkey_pulse_ms;
    int startup_delay_ms;
    int pwrkey_active_level;
} smonitor_modem_profile_t;

typedef struct {
    int rssi;
    int ber;
    bool registered;
} smonitor_modem_signal_t;

typedef struct {
    double latitude;
    double longitude;
    bool valid;
} smonitor_modem_location_t;

esp_err_t smonitor_modem_init(const smonitor_modem_config_t *config);
smonitor_modem_model_t smonitor_modem_configured_model(void);
const smonitor_modem_profile_t *smonitor_modem_get_profile(
    smonitor_modem_model_t model);
const smonitor_modem_profile_t *smonitor_modem_configured_profile(void);
esp_err_t smonitor_modem_connect(uint32_t timeout_ms);
esp_err_t smonitor_modem_disconnect(void);
smonitor_modem_state_t smonitor_modem_get_state(void);
esp_err_t smonitor_modem_get_signal(smonitor_modem_signal_t *signal);
esp_err_t smonitor_modem_get_location(smonitor_modem_location_t *location);

#ifdef __cplusplus
}
#endif
