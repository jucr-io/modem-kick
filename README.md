# modem-kick

Many modems take a very long time to recover from denied registration when the
reason for the denial is solved. They often require a power state change.

This tool automates that process by detecting a long period (about 10 minutes)
of denied or idle registration and moving the modem to low-power mode and back
to enabled. This is often enough to "kick" the modem back into successful
registration, as long as the reason registration was denied has been solved.

`modem-kick` runs as a systemd service which listens to ModemManager for
registration state changes and performs the necessary power operations.

## Building

`modem-kick` depends on glib and libmm-glib (ModemManager's client library)
development headers and libraries. Once you have those installed:

```
make
sudo make install
systemctl daemon-reload
systemctl enable modem-kick
systemctl start modem-kick
```
