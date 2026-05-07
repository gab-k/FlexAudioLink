# TODO

Documented here for later follow-up from review feedback on the recent PFSK/CLI refactor.

## High Priority

### 1. Web UI mode command parsing not updated

- File: `src/cli.c`
- Problem: the firmware CLI now uses `set mode <usb|pfsk_dongle|pfsk_headset>` and returns `OK mode=...`.
- Impact: the Web UI may still send separate `role` / `mode` commands and track acknowledgements by those old parameter names.
- Fix direction: update the Web UI protocol/parser to use the single mode command.

## Medium Priority

### 2. `#S` status lines no longer expose `conn=`

- File: `src/cli.c`
- Problem: periodic `#S` status output replaced `conn=` with `lock=` / `state=`.
- Impact: the current Web UI parser only updates connection state from `conn=`, so the dashboard connection indicator no longer reflects the real device link state.
- Fix direction: either preserve `conn=` in firmware status pushes, or update the Web UI parser to understand the new fields.