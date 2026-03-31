# FlexAudioLink Web UI

Web-based configuration GUI FlexAudioLink. Communicates with the Hardware over USB CDC ACM serial.

## Prerequisites

- [Node.js](https://nodejs.org/) >= 18
- [pnpm](https://pnpm.io/) (package manager)
- Python 3 with `pyserial` and `websockets` (only for the WebSocket bridge)

## Quick Start

```bash
cd webui/app
pnpm install
pnpm dev
```

Opens at `http://localhost:5173`. Click **Demo Mode** in the status bar to test without hardware.

## Building for Production

```bash
cd webui/app
pnpm build
```

Output goes to `webui/app/dist/`. This is a fully static site deployable to GitHub Pages or any static host.

Preview the production build locally:

```bash
pnpm preview
```

## Connecting to a Device

### Chrome / Edge (WebSerial)

Works out of the box. Click **Connect** in the status bar, then select the dongle serial port from the browser prompt.

### Firefox / Safari (WebSocket Bridge)

These browsers don't support WebSerial. Run the companion bridge script:

```bash
pip install pyserial websockets
python webui/websocket_serial_bridge.py
```

The bridge auto-detects the dongle by USB VID:PID (`1FC9:00A2`). The web app connects to `ws://localhost:8765` automatically when WebSerial is unavailable.

Bridge options:

```
python websocket_serial_bridge.py /dev/ttyACM0   # specify port
python websocket_serial_bridge.py --baud 921600   # custom baud
python websocket_serial_bridge.py --mock           # simulated device
python websocket_serial_bridge.py -v               # show serial traffic
python websocket_serial_bridge.py --list           # list serial ports
```

## Project Structure

```
webui/
  app/                          Svelte frontend
    src/
      main.js                   Entry point
      App.svelte                Shell, tab navigation
      app.css                   Global dark theme
      lib/
        stores/device.svelte.js Reactive device state, protocol parser, config validation
        transport/transport.js  WebSerial / WebSocket / Demo transport abstraction
        components/
          StatusBar.svelte      Connection status, toasts, connect/disconnect buttons
          LinkBudget.svelte     Bitrate budget, TDMA frame viz, latency estimate
        panels/
          ModePanel.svelte      Device role + operating mode selection
          StatusPanel.svelte    Live monitoring (RSSI, battery, packet loss chart)
          AudioPanel.svelte     Speaker/mic settings, EQ, codec selection
          RadioPanel.svelte     PHY rate, TX power, payload/jitter buffer config
          DevicePanel.svelte    Addressing, sleep, persistence (save/load/export/import)
          FirmwarePanel.svelte  DFU instructions with nrfutil commands
          AdvancedPanel.svelte  Raw serial console, live stats, developer shortcuts
    dist/                       Production build output (committed for GitHub Pages)
  websocket_serial_bridge.py    Python WebSocket-to-serial bridge
  spec.md                       Full design specification
```
