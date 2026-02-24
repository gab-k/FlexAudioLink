# 🦈 Wireshark Monitor Mode Cheatsheet
guide for wifi capture using monitor mode

---

## Prerequisites

```bash
# Core tools
sudo pacman -S wireshark-qt aircrack-ng iw wireless_tools

# Add your user to the wireshark group (avoid running wireshark as root)
sudo usermod -aG wireshark $USER
newgrp wireshark   # apply without logout
```

---

## Identify Interface

```
iwconfig                       # list wireless interfaces
ip link                        # list all interfaces
iw dev                         # list wireless interfaces + mode
iw phy                         # detailed phy info (bands, capabilities)
lspci | grep -i network        # PCI wifi cards
lsusb                          # USB wifi adapters
```

---

## rfkill — Unblock Wireless

```bash
rfkill list                    # show all blocked/unblocked devices
rfkill unblock wifi            # unblock wifi
rfkill unblock all             # unblock everything
rfkill block wifi              # block (re-enable if needed)
```

> If `soft blocked: yes` — run `rfkill unblock wifi`.
> If `hard blocked: yes` — check physical hardware switch on laptop.

---

## Kill Interfering Processes

Before enabling monitor mode, kill processes that fight for the interface:

```bash
sudo airmon-ng check           # list potentially interfering processes
sudo airmon-ng check kill      # kill them automatically
```

> **Restore after session:**
> ```bash
> sudo systemctl start NetworkManager
> ```

---

## Entering Monitor Mode via airmon-ng

```bash
# Start monitor mode (creates wlan0mon)
sudo airmon-ng start wlan0

# Start on a specific channel
sudo airmon-ng start wlan0 6

# Check status
sudo airmon-ng

# Stop monitor mode (restore managed mode)
sudo airmon-ng stop wlan0mon
```

> The new interface is typically named `wlan0mon`. Verify with `iw dev`.

---

## Monitor Mode via iw (Manual Method)

```bash
# Bring interface down
sudo ip link set wlan0 down

# Set to monitor mode
sudo iw dev wlan0 set type monitor

# Bring interface back up
sudo ip link set wlan0 up

# Verify mode
iw dev wlan0 info

# Set channel
sudo iw dev wlan0 set channel 6

# Set channel with HT20 mode
sudo iw dev wlan0 set channel 36 HT20

# Revert to managed mode
sudo ip link set wlan0 down
sudo iw dev wlan0 set type managed
sudo ip link set wlan0 up
```

### Add a dedicated monitor interface (non-destructive)

```bash
sudo iw phy phy0 interface add mon0 type monitor
sudo ip link set mon0 up
# ... capture ...
sudo ip link set mon0 down
sudo iw dev mon0 del
```

---

## ip — Interface Control

```bash
ip link show                           # show all interfaces
ip link set wlan0 up                   # bring up
ip link set wlan0 down                 # bring down
ip link set wlan0 promisc on           # enable promiscuous mode
ip link show wlan0                     # show wlan0 details
```

---

## Scan & Capture via airodump-ng 

```bash
# Scan all channels
sudo airodump-ng wlan0mon

# Scan specific band
sudo airodump-ng --band a wlan0mon     # 5GHz
sudo airodump-ng --band bg wlan0mon    # 2.4GHz

# Lock to specific channel
sudo airodump-ng -c 6 wlan0mon

# Capture to file (pcap compatible with Wireshark)
sudo airodump-ng -c 6 --bssid AA:BB:CC:DD:EE:FF -w capture wlan0mon

# Output files: capture-01.cap, capture-01.csv, capture-01.kismet.xml
```

> Open `.cap` files directly in Wireshark.

---

## Wireshark — Capture on Monitor Interface

```bash
# Launch GUI
wireshark

# Launch and start capture on interface directly
wireshark -i wlan0mon -k

# CLI capture with tshark
tshark -i wlan0mon

# tshark capture to file
tshark -i wlan0mon -w capture.pcap

# tshark on specific channel (set channel first with iw)
sudo iw dev wlan0mon set channel 6
tshark -i wlan0mon -w out.pcap
```

### Useful Wireshark Display Filters for WiFi

```
frame.time_delta > 0.0015             # time delta > 15ms from previous frame
frame.time >= "Feb 21, 2026 15:22:56" # limit time
wlan                                  # all 802.11 frames
wlan.fc.type == 0                     # management frames
wlan.fc.type == 1                     # control frames
wlan.fc.type == 2                     # data frames
wlan.fc.type_subtype == 0x08          # beacon frames
wlan.fc.type_subtype == 0x04          # probe requests
wlan.fc.type_subtype == 0x1d          # Acknowledgements
wlan.ta == c2:95:da:01:84:e9          # Transmitter MAC
wlan.ra == c2:95:da:01:84:e9          # Receiver MAC
wlan.ssid == "AP"                     # filter by SSID
eapol                                 # WPA handshake frames
```

---

## Useful One-Liners

```bash
# Check if card supports monitor mode
iw phy phy0 info | grep -A 10 "Supported interface modes"

# List channels your card supports
iw phy phy0 channels

# Scan for networks (managed mode, no monitor needed)
sudo iw dev wlan0 scan | grep -E "SSID|freq|signal"

# Watch interface stats live
watch -n1 iw dev wlan0 station dump
watch -n1 iw dev wlan0mon station dump
```

---

## Workflow Example

```bash
# 1. Unblock wifi
rfkill unblock wifi

# 2. Kill interference
sudo airmon-ng check kill

# 3. Start monitor mode
sudo airmon-ng start wlan0

# 4. Confirm interface
iw dev

# 5. Scan networks to find target channel
sudo airodump-ng wlan0mon

# 6. Capture on specific channel/BSSID
sudo airodump-ng -c 6 --bssid AA:BB:CC:DD:EE:FF -w mycapture wlan0mon

# --- OR open directly in Wireshark ---
wireshark -i wlan0mon -k

# 7. Restore when done
sudo airmon-ng stop wlan0mon
sudo systemctl start NetworkManager
```

---

## Qualcomm Atheros QCA6174 caveats

The **Qualcomm QCA6174** (ath10k driver) requires a firmware that has the raw-mode feature bit compiled in. Not all firmware versions support this, entering monitor mode typically still works but you wont be able to receive anything.

> This was confirmed by ath10k maintainer Kalle Valo on the ath10k mailing list: [source](https://www.mail-archive.com/search?l=ath10k@lists.infradead.org&q=subject:%22ath10k_pci+rawmode+%3D+1+requires+support+from+firmware%22&o=newest&f=1)


```bash
# Check current features - look for rawmode
sudo dmesg | grep ath10k | grep features 
```

### Which firmware to use

`RM` in the name indicates raw-mode enabled.

``` bash
# Confirmed working version:
firmware-6.bin_WLAN.RM.4.4.1.c3-00059
```


Other firmware versions for QCA6174 hw3.0 are available [here](https://git.codelinaro.org/clo/ath-firmware/ath10k-firmware/-/tree/main/QCA6174/hw3.0)


### Installing the firmware on Arch

The ath10k firmware lives in `/lib/firmware/ath10k/`. The kernel expects it **compressed with zstd** (Arch's initramfs uses zstd by default for firmware).

```bash
# 1. Find your device path (e.g. QCA6174/hw3.0)
ls /lib/firmware/ath10k/

# 2. Back up existing firmware
sudo cp /lib/firmware/ath10k/QCA6174/hw3.0/firmware-6.bin \
        /lib/firmware/ath10k/QCA6174/hw3.0/firmware-6.bin.bak

# 3. Copy new firmware into place
sudo cp firmware-6.bin_WLAN.RM.4.4.1.c3-00059 \
        /lib/firmware/ath10k/QCA6174/hw3.0/firmware-6.bin

# 4. Compress it with zstd (Arch kernel expects .zst)
sudo zstd /lib/firmware/ath10k/QCA6174/hw3.0/firmware-6.bin \
          -o /lib/firmware/ath10k/QCA6174/hw3.0/firmware-6.bin.zst

# 5. reboot and verify
reboot
sudo dmesg | grep ath10k | grep features 

```


---
