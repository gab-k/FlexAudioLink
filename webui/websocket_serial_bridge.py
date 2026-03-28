#!/usr/bin/env python3
"""
WebSocket-to-Serial bridge for FlexAudioLink configuration GUI.
Allows Firefox/Safari users to communicate with the dongle via WebSocket
when WebSerial is not available.

Usage:
    pip install pyserial websockets
    python websocket_serial_bridge.py                  # auto-detect or interactive select
    python websocket_serial_bridge.py /dev/ttyACM0     # specify port directly
    python websocket_serial_bridge.py --baud 921600    # custom baud rate
    python websocket_serial_bridge.py --mock            # run with simulated device (no hardware)
"""

import argparse
import asyncio
import json
import random
import sys
from datetime import datetime

import serial
import serial.tools.list_ports
import websockets

# ANSI colors for terminal traffic display
C_TX = "\033[36m"   # cyan  — GUI → Device
C_RX = "\033[33m"   # yellow — Device → GUI
C_RST = "\033[0m"

# USB VID/PID for the FlexAudioLink dongle
DONGLE_VID = 0x1FC9
DONGLE_PID = 0x00A2
WS_HOST = "localhost"
WS_PORT = 8765
DEFAULT_BAUD = 115200


def list_serial_ports():
    """List all available serial ports with details."""
    ports = serial.tools.list_ports.comports()
    return sorted(ports, key=lambda p: p.device)


def find_dongle():
    """Auto-detect dongle by USB VID/PID."""
    for port in serial.tools.list_ports.comports():
        if port.vid == DONGLE_VID and port.pid == DONGLE_PID:
            return port.device
    return None


def select_port_interactive():
    """Let the user pick a serial port from a numbered list."""
    ports = list_serial_ports()
    if not ports:
        print("No serial ports found.")
        return None

    print("\nAvailable serial ports:")
    for i, port in enumerate(ports):
        vid_pid = ""
        if port.vid is not None:
            vid_pid = f"  [{port.vid:04X}:{port.pid:04X}]"
        desc = port.description or ""
        marker = " <-- FlexAudioLink dongle" if (port.vid == DONGLE_VID and port.pid == DONGLE_PID) else ""
        print(f"  {i + 1}) {port.device}  {desc}{vid_pid}{marker}")

    print()
    while True:
        try:
            choice = input(f"Select port [1-{len(ports)}]: ").strip()
            idx = int(choice) - 1
            if 0 <= idx < len(ports):
                return ports[idx].device
        except (ValueError, EOFError):
            pass
        print("Invalid selection, try again.")


def resolve_port(port_arg):
    """Resolve which serial port to use: argument > auto-detect > interactive."""
    if port_arg:
        return port_arg

    auto = find_dongle()
    if auto:
        print(f"Auto-detected FlexAudioLink dongle: {auto}")
        return auto

    print("FlexAudioLink dongle not auto-detected (VID:PID {0:04X}:{1:04X}).".format(DONGLE_VID, DONGLE_PID))
    return select_port_interactive()


class MockDevice:
    """Simulated device for GUI testing without real hardware."""

    DEFAULT_STATE = {
        # Audio — Speaker
        'sample_rate_spk': '48000', 'bit_width_spk': '16',
        'channels_spk': 'stereo', 'codec_spk': 'pcm',
        'volume': '80', 'sidetone': '0',
        # Audio — Mic
        'sample_rate_mic': '48000', 'bit_width_mic': '16',
        'channels_mic': 'mono', 'codec_mic': 'pcm',
        'mic_gain': '12', 'mic_mute': 'off',
        # Radio
        'phy_rate': '4', 'tx_power': '0', 'fhss_exclusion': 'none',
        'ble_phy': '2M',
        'payload_ms_dl': '1', 'payload_ms_ul': '1', 'jitter_buffer_ms': '10',
        # Mode
        'role': 'dongle', 'mode': 'proprietary',
        # Device
        'audio_io': 'usb', 'device_addr': 'D0D0D0D0', 'peer_addr': 'A1A1A1A1',
        'auto_sleep': '10', 'low_battery_threshold': '10',
        # EQ
        'eq0': '100,0', 'eq1': '400,0', 'eq2': '1000,0',
        'eq3': '4000,0', 'eq4': '10000,0',
    }

    # Group config keys for structured 'get all' output
    GROUPS = [
        ('[audio]', ['sample_rate_spk', 'bit_width_spk', 'channels_spk', 'codec_spk',
                     'volume', 'sidetone', 'sample_rate_mic', 'bit_width_mic',
                     'channels_mic', 'codec_mic', 'mic_gain', 'mic_mute']),
        ('[radio]', ['phy_rate', 'tx_power', 'fhss_exclusion', 'ble_phy',
                     'payload_ms_dl', 'payload_ms_ul', 'jitter_buffer_ms']),
        ('[mode]', ['role', 'mode']),
        ('[device]', ['audio_io', 'device_addr', 'peer_addr', 'auto_sleep',
                      'low_battery_threshold']),
        ('[eq]', ['eq0', 'eq1', 'eq2', 'eq3', 'eq4']),
    ]

    def __init__(self):
        self.state = dict(self.DEFAULT_STATE)
        self.echo = True
        self.status_interval_ms = 0  # 0 = off
        # Counters for status push
        self._tx = 0
        self._rx = 0
        self._lost = 0

    def handle_line(self, line):
        """Process a command line and return a list of response lines."""
        line = line.strip()
        if not line:
            return []

        parts = line.split()
        cmd = parts[0].lower()

        if cmd == 'echo':
            if len(parts) > 1 and parts[1].lower() == 'off':
                self.echo = False
            else:
                self.echo = True
            return ['OK echo']

        if cmd == 'status':
            if len(parts) >= 3 and parts[1].lower() == 'on':
                try:
                    self.status_interval_ms = int(parts[2])
                except ValueError:
                    return [f'ERR status invalid_interval']
                return ['OK status']
            elif len(parts) >= 2 and parts[1].lower() == 'off':
                self.status_interval_ms = 0
                return ['OK status']
            return ['ERR status bad_args']

        if cmd == 'get':
            if len(parts) >= 2 and parts[1].lower() == 'all':
                return self._get_all()
            if len(parts) >= 2:
                key = parts[1]
                if key in self.state:
                    return [f'{key}={self.state[key]}']
                return [f'ERR {key} unknown_param']
            return ['ERR get missing_param']

        if cmd == 'set':
            if len(parts) >= 3:
                key = parts[1]
                value = ' '.join(parts[2:])
                if key in ('save_config', 'load_config'):
                    return [f'OK {key}']
                if key in self.state:
                    self.state[key] = value
                    return [f'OK {key}={value}']
                # EQ: 'set eq <band> <freq> <gain>'
                if key == 'eq' and len(parts) >= 5:
                    band = parts[2]
                    eq_key = f'eq{band}'
                    if eq_key in self.state:
                        self.state[eq_key] = f'{parts[3]},{parts[4]}'
                        return [f'OK eq={self.state[eq_key]}']
                return [f'ERR {key} unknown_param']
            return ['ERR set missing_args']

        if cmd == 'reset':
            return ['OK reset']

        if cmd == 'save_config':
            return ['OK save_config']

        if cmd == 'load_config':
            return self._get_all()

        return [f'ERR {cmd} unknown_command']

    def _get_all(self):
        """Return all config as grouped key=value lines."""
        lines = []
        for header, keys in self.GROUPS:
            lines.append(header)
            for key in keys:
                lines.append(f'{key}={self.state[key]}')
        return lines

    def status_line(self):
        """Generate a periodic status push line with slight jitter."""
        self._tx += random.randint(8, 12)
        self._rx += random.randint(8, 12)
        self._lost += (1 if random.random() < 0.05 else 0)
        rssi = -45 + random.randint(-5, 5)
        bat = max(0, min(100, 85 + random.randint(-1, 1)))
        loss = round(random.uniform(0, 0.5), 1)
        return (f'#S rssi={rssi} bat={bat} loss={loss} conn=yes '
                f'tx={self._tx} rx={self._rx} lost={self._lost} '
                f'urun=0 orun=0 cerr=0 fw=0.1.0')


class Bridge:
    def __init__(self, port, baud, ws_port=WS_PORT, verbose=False, mock=False):
        self.port = port
        self.baud = baud
        self.ws_port = ws_port
        self.verbose = verbose
        self.mock = mock
        self.mock_device = MockDevice() if mock else None
        self.ser = None
        self.ws_client = None
        self._read_task = None

    def _log_traffic(self, direction, line):
        """Print traffic to terminal. direction: 'TX' (GUI→Device) or 'RX' (Device→GUI)."""
        if not self.verbose:
            return
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        color = C_TX if direction == "TX" else C_RX
        print(f"{color}{ts} {direction} {line}{C_RST}")

    def connect_serial(self):
        try:
            self.ser = serial.Serial(self.port, self.baud, timeout=0.1)
            print(f"Serial connected: {self.port} @ {self.baud} baud")
            return True
        except serial.SerialException as e:
            print(f"Serial error: {e}")
            return False

    def disconnect_serial(self):
        if self.ser and self.ser.is_open:
            self.ser.close()
            self.ser = None
            print("Serial disconnected")

    async def serial_read_loop(self):
        """Read lines from serial and forward to WebSocket client."""
        loop = asyncio.get_event_loop()
        buffer = ""
        while self.ser and self.ser.is_open:
            try:
                data = await loop.run_in_executor(None, self.ser.readline)
                if data:
                    buffer += data.decode("utf-8", errors="replace")
                    while "\n" in buffer:
                        line, buffer = buffer.split("\n", 1)
                        line = line.strip()
                        if not line:
                            continue
                        self._log_traffic("RX", line)
                        if self.ws_client:
                            try:
                                await self.ws_client.send(line)
                            except websockets.ConnectionClosed:
                                break
                else:
                    await asyncio.sleep(0.01)
            except (serial.SerialException, OSError):
                print("Serial read error, attempting reconnect...")
                self.disconnect_serial()
                while not self.connect_serial():
                    await asyncio.sleep(2)

    async def mock_status_loop(self):
        """Periodically send status push lines from the mock device."""
        while True:
            if self.mock_device.status_interval_ms > 0 and self.ws_client:
                line = self.mock_device.status_line()
                self._log_traffic("RX", line)
                try:
                    await self.ws_client.send(line)
                except websockets.ConnectionClosed:
                    break
                await asyncio.sleep(self.mock_device.status_interval_ms / 1000)
            else:
                await asyncio.sleep(0.1)

    async def handle_ws(self, websocket):
        """Handle a single WebSocket client connection."""
        if self.ws_client:
            print("Rejecting second client — only one connection allowed")
            await websocket.close(1008, "Only one client allowed")
            return

        self.ws_client = websocket
        print(f"WebSocket client connected: {websocket.remote_address}")

        if self.mock:
            # Mock mode — no serial needed
            self._read_task = asyncio.create_task(self.mock_status_loop())
        else:
            # Ensure serial is connected
            if not self.ser or not self.ser.is_open:
                if not self.connect_serial():
                    await websocket.send(
                        json.dumps({"cmd": "error", "message": f"Cannot open {self.port}"})
                    )

            # Start serial read loop
            self._read_task = asyncio.create_task(self.serial_read_loop())

        try:
            async for message in websocket:
                line = message.strip()
                self._log_traffic("TX", line)

                if self.mock:
                    # Route through mock device
                    responses = self.mock_device.handle_line(line)
                    for resp in responses:
                        self._log_traffic("RX", resp)
                        await websocket.send(resp)
                else:
                    # Forward to serial
                    if self.ser and self.ser.is_open:
                        try:
                            self.ser.write((line + "\n").encode("utf-8"))
                        except serial.SerialException:
                            print("Serial write error")
                            await websocket.send(
                                json.dumps(
                                    {"cmd": "error", "message": "Serial write failed"}
                                )
                            )
        except websockets.ConnectionClosed:
            pass
        finally:
            print("WebSocket client disconnected")
            self.ws_client = None
            if self._read_task:
                self._read_task.cancel()
                try:
                    await self._read_task
                except asyncio.CancelledError:
                    pass

    async def run(self):
        print(f"\nFlexAudioLink Bridge — ws://{WS_HOST}:{self.ws_port}")
        if self.mock:
            print("Mode: MOCK DEVICE (no serial hardware)")
        else:
            print(f"Serial: {self.port} @ {self.baud} baud")
        if self.verbose:
            print(f"Traffic logging: {C_TX}TX (GUI→Device){C_RST}  {C_RX}RX (Device→GUI){C_RST}")
        print("Waiting for WebSocket client...\n")
        async with websockets.serve(self.handle_ws, WS_HOST, self.ws_port):
            await asyncio.Future()  # Run forever


def main():
    parser = argparse.ArgumentParser(
        description="FlexAudioLink WebSocket-to-Serial bridge"
    )
    parser.add_argument(
        "port", nargs="?", default=None,
        help="Serial port (e.g. /dev/ttyACM0, COM3). Omit to auto-detect or choose interactively."
    )
    parser.add_argument(
        "--baud", type=int, default=DEFAULT_BAUD,
        help=f"Baud rate (default: {DEFAULT_BAUD})"
    )
    parser.add_argument(
        "--ws-port", type=int, default=WS_PORT,
        help=f"WebSocket port (default: {WS_PORT})"
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true",
        help="Show serial traffic in terminal (TX=GUI→Device, RX=Device→GUI)"
    )
    parser.add_argument(
        "-m", "--mock", action="store_true",
        help="Run with simulated device (no serial hardware needed)"
    )
    parser.add_argument(
        "--list", action="store_true",
        help="List available serial ports and exit"
    )
    args = parser.parse_args()

    if args.list:
        ports = list_serial_ports()
        if not ports:
            print("No serial ports found.")
        else:
            for port in ports:
                vid_pid = ""
                if port.vid is not None:
                    vid_pid = f"  [{port.vid:04X}:{port.pid:04X}]"
                print(f"  {port.device}  {port.description or ''}{vid_pid}")
        sys.exit(0)

    if args.mock:
        port = None
    else:
        port = resolve_port(args.port)
        if not port:
            print("No port selected. Exiting.")
            sys.exit(1)

    ws_port = args.ws_port

    bridge = Bridge(port, args.baud, ws_port, verbose=args.verbose, mock=args.mock)
    try:
        asyncio.run(bridge.run())
    except KeyboardInterrupt:
        print("\nBridge stopped")
        bridge.disconnect_serial()
        sys.exit(0)


if __name__ == "__main__":
    main()
