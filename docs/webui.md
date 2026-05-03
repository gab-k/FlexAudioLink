# Web UI

The `webui/` directory contains:

- a **Svelte application** in `webui/app/`
- a **Python WebSocket-to-serial bridge** in `webui/websocket_serial_bridge.py`

## Communication

The UI communicates with the device over USB CDC ACM using:

- **WebSerial** in Chromium-based browsers
- a local **WebSocket bridge** for browsers without WebSerial support

!!! warning
    `webui/spec.md` is broader than the currently implemented firmware surface and should be read as intent rather than as a statement of completed firmware behavior.