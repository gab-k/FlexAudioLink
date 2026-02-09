# FlexAudioLink

FlexAudioLink is a standalone, low-latency wireless audio bridge that supports one-to-one or one-to-many streaming.

The project uses custom hardware based on the NXP RW612 (Dual-band Wi-Fi 6 & BLE 5.4 MCU) running FreeRTOS firmware. The same hardware can be configured for different operational modes at runtime.


## Status Legend:
✅ Implemented
🚧 Work in Progress
🔮 Planned

## Key Features

- USB Audio Class 2.0 compliant, works as a standard audio device ✅
- Runtime mode switching via CLI (Virtual Com Port) ✅
- Low-latency wireless audio streaming over Wi-Fi 🚧
- Adaptive jitter buffer sizing based on network conditions 🔮
- Low-power wireless audio streaming over BLE 🔮
- Streaming via IP network 🔮
- Point-to-multipoint support, one dongle, multiple headsets 🔮

## Operational Modes (Runtime Configurable):

### 1. USB-to-I2S Bridge (USB Headset Mode) ✅
Acts as a standard USB Audio Class 2.0 device. Isochronous USB data is routed directly to/from the I2S interface via DMA.

**Use case**: Wired fallback for wireless headsets when battery is depleted.

```mermaid
graph LR;
    Host[Host PC] <-- "USB ISO" --> TUSB[TinyUSB Stack]
    TUSB <-- "DMA Ring Buffer" --> I2S[I2S Interface]
    I2S <--> CODEC[Audio Codec]
```
    
### 2. USB-to-WiFi Bridge (UDP Dongle Mode) 🚧

Full-duplex USB audio interface that wirelessly bridges to headset hardware.

- Playback path: Receives playback (speaker) audio from the USB host, packetizes samples into UDP frames, and transmits them over WiFi.

- Microphone path: Receives microphone audio from the headset via WiFi, decodes UDP packets, and forwards the data to the USB audio IN endpoint.

- Feedback path: Receives and forwards buffer fill level for audio feedback mechanism which accounts for sample rate mismatch. 

```mermaid
graph LR
    Host[Host PC] <-- "USB ISO" --> TUSB[TinyUSB Stack]
    TUSB <--> Net[LwIP Stack]
    Net <-- "UDP Packet" --> WLAN((WiFi Radio))
```

### 3. WiFi-to-I2S Bridge (UDP Headset Mode) 🚧

Wireless audio endpoint with direct audio hardware interfacing.

- Playback path: Receives UDP audio data from the dongle, performs jitter buffering and packet reordering, and outputs audio via I2S to a CODEC.

- Microphone path: Captures microphone audio via I2S CODEC, packetizes samples into UDP frames, and transmits them back to the dongle over WiFi.

- Feedback path: Transmits back buffer fill level for audio feedback mechanism which accounts for sample rate mismatch. 

```mermaid
graph LR
    WLAN((WiFi Radio)) <-- "UDP Packet" --> Net[LwIP Stack]
    Net <-- "Jitter Buffer" --> I2S[I2S Interface]
    I2S <--> CODEC[Headset Audio Codec]
```

### 4. BLE Headset Mode 🔮

Operates as a Bluetooth Low Energy audio headset, removing the need for WiFi in portable or low-power scenarios.
Targets lower power consumption and broader device compatibility, with higher latency than WiFi modes.


### 5. Network Audio Endpoint Mode 🔮

Connects to a local network as a standalone audio endpoint, enabling:

- Direct audio streaming from any device on the same network

- Optional internet streaming 

- Operation without a dedicated dongle
