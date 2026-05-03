# CLI Reference

The nRF firmware exposes a USB CDC CLI in `firmware/src/cli.c`.

## Commands

| Command | Description |
|---------|-------------|
| `help` | Show available commands |
| `get` | Get current configuration |
| `set profile <usb\|pfsk_dongle\|pfsk_headset>` | Set the boot profile |
| `status` | Show device status |
| `status on [ms]` | Enable periodic status output with optional interval |
| `status off` | Disable periodic status output |
| `scan` | Run a BLE/prop scan |
| `linktest on\|off\|status` | Control link test mode |
| `i2s tone on\|off\|status` | Control I2S tone generator |
| `reset` | Reset the device |