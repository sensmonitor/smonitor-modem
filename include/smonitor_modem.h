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

typedef struct {
    const char *apn;
    const char *username;
    const char *password;
    smonitor_modem_network_t network;
} smonitor_modem_config_t;

typedef struct {
    int rssi;
    int ber;
    bool registered;
} smonitor_modem_signal_t;

esp_err_t smonitor_modem_init(const smonitor_modem_config_t *config);
esp_err_t smonitor_modem_connect(uint32_t timeout_ms);
esp_err_t smonitor_modem_disconnect(void);
smonitor_modem_state_t smonitor_modem_get_state(void);
esp_err_t smonitor_modem_get_signal(smonitor_modem_signal_t *signal);

#ifdef __cplusplus
}
#endif
