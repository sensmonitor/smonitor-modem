# smonitor-modem

ESP-IDF component that owns modem transport, board power control, PPP
connectivity and modem status for SensMonitor firmware.

## Initial support

- ESP-IDF 5.5.x
- LilyGO T-SIM7000G
- SIM7000 over UART
- TX 27, RX 26, RTS 25, CTS 23, PWRKEY 4

The initial scaffold defines the public API and board profile. Migration of
the working PPP implementation from `modem_console` is the next step.

The board profile and the operator profile are intentionally separate. APN
credentials belong to the consuming application and are never given working
defaults by this component.
