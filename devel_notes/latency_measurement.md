# Latency Measurement

This test measures the device path from the dongle audio thread to the analog
codec output. It intentionally excludes host playback latency and any time the
sample spent in TinyUSB's EP OUT FIFO before `path_dongle.c` drained it.

## Firmware Marker

`path_dongle.c` scans each 192-byte USB speaker packet immediately after
`tu_fifo_read_n()` copies it into `packet.payload`. When it finds this signed
16-bit little-endian stereo pattern, it toggles P0.02:

```text
32767, +32767
-32768, -32768
+32767, +32767
-32768, -32768
```

The existing radio PHYEND debug pins use P0.00 and P0.01, so the marker probe
uses P0.02 by default.

## Generate Test WAV

From the repo root:

```sh
python3 python/generate_latency_marker_wav.py
```

This creates `latency_marker_48k_stereo.wav`: 48 kHz, stereo, signed 16-bit PCM,
with a marker every 10 seconds. The marker interval is aligned to the firmware's
1 ms / 192-byte speaker packet size.

Optional:

```sh
python3 python/generate_latency_marker_wav.py --duration-s 120 --interval-s 10
```

## Linux Playback

Use `aplay` directly to the ALSA `hw` device when you want the marker bytes to
arrive unchanged. Regular desktop players can apply volume, mixing, format
conversion, resampling, or effects before the samples reach USB audio; any of
those can prevent the firmware from matching the full-scale marker pattern.

The playback host is still not the timing reference. The timing reference is the
dongle GPIO edge.

For ALSA, first find the device:

```sh
aplay -l
```

Then play the file:

```sh
aplay -D hw:CARD,DEVICE latency_marker_48k_stereo.wav
```

Replace `CARD,DEVICE` with the dongle's ALSA card and device numbers.

Avoid `default`, PipeWire/PulseAudio-routed playback, and player volume controls
for this test. `hw:CARD,DEVICE` sends the WAV frames directly to the USB audio
PCM device, so the generated marker should remain full scale.

## Scope Setup

Probe:

```text
CH1: dongle P0.02 marker GPIO
CH2: codec analog output
```

Measured latency:

```text
dongle P0.02 edge -> analog pulse at codec output
```

Interpret this as:

```text
dongle post-USB-FIFO audio path
+ PROP enqueue/TX/RX
+ headset buffering
+ I2S scheduling/DMA
+ codec latency
```
