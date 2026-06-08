# smonitor-modem

ESP-IDF component for SensMonitor cellular modem connectivity.

This component owns modem board power control, SIM7000 AT initialization, PPP
network setup, PPP event diagnostics and modem status reporting. It is used by
`smonitor-iot`, but can also be consumed by another ESP-IDF project that needs
the same modem flow.

## Current Support

- ESP-IDF `>=5.5,<5.6`
- ESP32 target
- SIM7000 modem
- LilyGO T-SIM7000G board profile
- UART modem connection
- PPP over cellular data
- Network modes: Automatic, LTE-M, NB-IoT and GPRS

Default LilyGO T-SIM7000G pins:

| Signal | GPIO |
| --- | ---: |
| Modem TX | 27 |
| Modem RX | 26 |
| Modem RTS | 25 |
| Modem CTS | 23 |
| Modem PWRKEY | 4 |

The tested SensMonitor profile uses SIM7000 on NB-IoT band 20 with PPP
authentication disabled.

## What The Component Does

The component:

1. Initializes ESP-IDF networking and the default event loop.
2. Creates a PPP network interface.
3. Configures PPP authentication according to the application config.
4. Initializes the board PWRKEY GPIO.
5. Powers on the SIM7000 modem.
6. Creates an `esp_modem` SIM7000 DCE.
7. Runs the SIM7000 AT configuration sequence.
8. Switches the modem to PPP data mode.
9. Waits for `IP_EVENT_PPP_GOT_IP`.
10. Logs PPP phases, PPP failures, IP address and DNS.

The AT configuration sequence is based on the known-good `modem_console`
firmware used for LilyGO T-SIM7000G:

- reset/enable network registration reporting
- enable full modem functionality
- set UART baud rate
- enable automatic operator selection
- configure CAT-M and NB-IoT band masks
- ignore DTR status
- disable PSM, sleep and eDRX
- select preferred network mode
- re-enable full modem functionality
- enable registration reporting

The component intentionally does not write settings with `AT&W` on every boot.

## Repository Layout

```text
smonitor-modem/
  include/
    smonitor_modem.h
  src/
    smonitor_modem.c
    smonitor_modem_internal.h
    boards/
      lilygo_t_sim7000g.c
  CMakeLists.txt
  Kconfig
  idf_component.yml
```

## Configuration

Board and modem hardware options are defined by this component in:

```text
SensMonitor modem
```

Application-level mobile network options, such as APN and PPP authentication
choice, are supplied by the consuming project. In `smonitor-iot` they are under:

```text
SensMonitor IoT
```

### Hardware Options

| Option | Meaning | Default |
| --- | --- | --- |
| `CONFIG_SMONITOR_MODEM_MODEL_SIM7000` | Select SIM7000 modem. | enabled |
| `CONFIG_SMONITOR_MODEM_BOARD_LILYGO_T_SIM7000G` | Select LilyGO T-SIM7000G board profile. | enabled |
| `CONFIG_SMONITOR_MODEM_UART_TX_PIN` | ESP32 TX pin connected to modem RX. | `27` |
| `CONFIG_SMONITOR_MODEM_UART_RX_PIN` | ESP32 RX pin connected to modem TX. | `26` |
| `CONFIG_SMONITOR_MODEM_UART_RTS_PIN` | UART RTS pin. | `25` |
| `CONFIG_SMONITOR_MODEM_UART_CTS_PIN` | UART CTS pin. | `23` |
| `CONFIG_SMONITOR_MODEM_PWRKEY_PIN` | Modem PWRKEY GPIO. | `4` |
| `CONFIG_SMONITOR_MODEM_UART_BAUD_RATE` | Modem UART baud rate. | `9600` |
| `CONFIG_SMONITOR_MODEM_UART_RX_BUFFER_SIZE` | UART RX buffer size. | `4096` |
| `CONFIG_SMONITOR_MODEM_UART_TX_BUFFER_SIZE` | UART TX buffer size. | `2048` |
| `CONFIG_SMONITOR_MODEM_LTE_BAND` | Preferred LTE/NB-IoT band. | `20` |

### Runtime Config

The public API receives runtime modem configuration from the application:

```c
typedef struct {
    const char *apn;
    const char *username;
    const char *password;
    smonitor_modem_network_t network;
} smonitor_modem_config_t;
```

Rules:

- `apn` is required.
- If `username` is `NULL` or empty, PPP authentication is disabled.
- If `username` is non-empty, PAP is enabled and `password` is used.
- `network` selects Automatic, LTE-M, NB-IoT or GPRS mode.

For the tested LilyGO/SIM7000G profile:

```c
smonitor_modem_config_t config = {
    .apn = "vipmobile",
    .username = NULL,
    .password = NULL,
    .network = SMONITOR_MODEM_NETWORK_NB_IOT,
};
```

## Public API

```c
esp_err_t smonitor_modem_init(const smonitor_modem_config_t *config);
esp_err_t smonitor_modem_connect(uint32_t timeout_ms);
esp_err_t smonitor_modem_disconnect(void);
smonitor_modem_state_t smonitor_modem_get_state(void);
esp_err_t smonitor_modem_get_signal(smonitor_modem_signal_t *signal);
```

Typical use:

```c
smonitor_modem_config_t config = {
    .apn = CONFIG_SMONITOR_MODEM_APN,
    .username = NULL,
    .password = NULL,
    .network = SMONITOR_MODEM_NETWORK_NB_IOT,
};

ESP_ERROR_CHECK(smonitor_modem_init(&config));
ESP_ERROR_CHECK(smonitor_modem_connect(180000));
```

## Expected Logs

A successful connection should include:

```text
smonitor_modem: Configured SIM7000 on LilyGO T-SIM7000G, APN=...
smonitor_modem: PPP authentication: none
smonitor_modem: Power on the modem
smonitor_modem: Initializing esp_modem for SIM7000
smonitor_modem: Signal quality: rssi=..., ber=...
smonitor_modem: Waiting for PPP IP address
smonitor_modem: PPP event ...: phase establish
smonitor_modem: PPP event ...: phase network
smonitor_modem: PPP event ...: phase running
smonitor_modem: Modem connected to PPP server
smonitor_modem: IP          : ...
smonitor_modem: Netmask     : ...
smonitor_modem: Gateway     : ...
smonitor_modem: Name Server: ...
```

PPP failures are logged by name:

```text
smonitor_modem: PPP error 7: authentication failed
smonitor_modem: PPP error 9: peer timeout
smonitor_modem: PPP IP timeout after 180000 ms; last event: ...
```

## Troubleshooting

### Modem Does Not Respond To AT

Check:

- board power
- PWRKEY pin
- UART TX/RX wiring
- UART baud rate
- antenna and SIM module seating

Default baud is `9600`, matching the current known-good SIM7000 setup.

### PPP Times Out

Check:

- APN is correct
- SIM card has mobile data enabled
- antenna is connected
- selected network mode is available in the area
- selected band is correct for the operator
- PPP authentication setting matches the operator

For the tested profile:

```text
auth: none
network: NB-IoT
band: 20
```

### Authentication Failed

Log:

```text
PPP error 7: authentication failed
```

If the operator does not require PPP auth, pass `NULL` or an empty username.
If the operator requires auth, pass a username and password so PAP is enabled.

### Peer Timeout

Log:

```text
PPP error 9: peer timeout
```

This usually means the modem entered data mode but the carrier PPP negotiation
did not complete. Re-check APN, signal quality, network mode and band.

## Notes For Maintainers

- APN and operator credentials must stay in the consuming application, not in
  this component.
- This component currently supports one board profile. Add new boards under
  `src/boards/` and expose them through `Kconfig`.
- The SIM7000 AT sequence is intentionally conservative because it matches the
  working SensMonitor `modem_console` flow.
- Avoid writing modem settings to NVM on every boot unless there is a clear
  provisioning reason.
