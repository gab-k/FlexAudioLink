# HW Spec — Wireless Headset Retrofit

## ICs
- **SoC** nRF54LM20A-PAAA (CSP47)
- **Codec** NAU88L21 (QFN32)
- **PMIC** nPM1300-QEAA
- **Battery protection** DW01A + FS8205

---

## Power Rails
| Rail | Source | Voltage | Load |
|---|---|---|---|
| Buck 1 (VOUT1) | VBAT/VBUS | 1.8V | nRF VDD, codec VDDB |
| Buck 2 (VOUT2) | VBAT/VBUS | 3.3V | codec VDDMIC, LDO1 in |
| LDO 1 | Buck 2 | 1.8V | codec VDDA |
| LDO 2 | Buck 2 | TBD | touch IC / reserved |

---

## nRF54LM20A — Schematic Reminders
- DC-DC: DCC node → **L1 4.7µH 0603**
- VDD bulk: **C4 10µF 0402** + **C5 100nF 0201**
- VDD decoupling: **C1, C2, C10 100nF 0201**
- DECA: **FB1 0201** + **FB2 0201** (two ferrites in DK) + **C6 4.7µF** to VSS
- DECD: **C14 2.2µF 0201** to VSS
- DECRF: **L5 1.0nH** + **C8 1.0pF** + **C7 2.2µF** (DK fits C7, ref design marks N.C.)
- DECUSB: **C10 2.2µF + C11 4.7µF**
- USB VBUS: **R1 2R2 0603** in series; ESD protection PRTR5V0U2F + CMC DLM0NSM900HY2D on D+/D−
- TRXTUNE: **R2 200R 0603** to VSS
- 32MHz XO: XTAL_2016 (X1)
- 32.768kHz XO: XTAL_2012 (X2) — optional
- RF match: L2 3.6nH, L3 4.7nH, L4 3.3nH, C3 1.2pF, C12 1.5pF, C13 N.C.

## nRF54LM20A — Layout Reminders
- Follow Nordic reference: **13.11 × 12.00mm**, 8-layer, 1.57mm total
- RF matching as close as possible to ANT pin
- DECRF caps closest to IC, then L5
- DECA/DECD/DCC passives same side as IC, short traces
- P2 is high-speed port — follow Nordic HDI rules

---

## NAU88L21 — Schematic Reminders
- VDDA: 1.8V from LDO1 (max 1.98V — do not exceed)
- VDDB: 1.8V from Buck 1
- VDDMIC: 3.3V from Buck 2
- VREF: **4.7µF** low-leakage cap to VSSA
- Charge pump caps: **3× 2µF ceramic** (CPCA–CPCB, CPOUTP, CPOUTN)
- Mic inputs: AC-coupled differential
- I2S to nRF: BCLK, FS, DACIN, ADCOUT
- I2C to nRF: SCLK, SDIO (codec is slave)
- GPIO1/CSB: pull low → I2C addr 0x1B, pull high → 0x54

---

## nPM1300 — Schematic Reminders
- Buck inductors: **L6, L7 = 2.2µH** (one per buck)
- VSET1: **47k** → 1.8V startup
- VSET2: **4.7k** → 1.8V startup (change for 3.3V per resistor table)
- VSYS bulk: **4× 10µF** caps (PVSS1 and PVSS2, via to GND plane on each)
- VOUT1 decoupling: **10µF + 100nF**
- VOUT2 decoupling: **10µF + 100nF**
- VBUS input: **4.7µF + 100nF**; ferrite bead FB on VBUS line
- CC1/CC2: wire directly to USB-C connector + **1.0nF cap to GND** on each
- NTC pin: connect battery thermistor (10k NTC recommended)
- VBAT: via DW01A + FS8205 from cell
- VDDIO: connect to Buck 1 output (1.8V)
- nPM1300 communicates with nRF via I2C (SCL/SDA)

---

## Battery Protection (DW01A + FS8205)
- Cell+ → DW01A → FS8205 → VBAT (nPM1300)
- Skip if using cell with integrated protection PCB

---

## USB-C Connector
- VBUS → ferrite bead (120R/3A) → nPM1300 VBUS
- VBUS → R1 2R2 → nRF VBUS pin
- CC1/CC2 → nPM1300 CC1/CC2 (+ 1.0nF to GND each)
- D+/D− → ESD + CMC → nRF P2 (high-speed port)
- R45/R46 5k1 on CC lines only needed if doing USB-C orientation detection at nRF — not required for basic charging

---

## Codec Audio Path
- Headphone out: HPL/HPR ground-referenced, cap-free
- Mic in: MICL+/− or MICR+/− AC-coupled differential
- Digital mic alt: MICR−/DMCLK + MICL−/DMDATA
