# NAU88L21 Codec

`firmware/src/audio/nau88l21.*` owns NAU88L21 register programming and the
runtime FLL retuning API used by the I2S-backed audio paths.

## Purpose

Configure the codec as the I2S clock master, program the playback-related DAC
and headphone driver registers, and expose runtime sample-rate retuning through
the codec FLL.

## Source Files

| File | Role |
|---|---|
| `firmware/src/audio/nau88l21.c` | Register writes, FLL math, init sequence, runtime retune. |
| `firmware/src/audio/nau88l21.h` | Public init and retune API. |
| `firmware/src/audio/i2s.c` | Calls `nau88l21_init()` before I2S configuration. |
| `firmware/boards/nrf54lm20dk_nrf54lm20a_cpuapp.overlay` | I2C bus, TDM pins, 32 MHz MCLKI source. |

## Bus and Addressing

| Item | Value |
|---|---|
| I2C bus | `i2c23` |
| I2C address | `0x1B` |
| SDA / SCL | P3.11 / P3.12 |
| I2C speed | `I2C_BITRATE_STANDARD` |
| DMA region | `DMA_RAM` from the board overlay |

## Public API

| Function | Behavior |
|---|---|
| `nau88l21_init(i2c_dev)` | Uses the ready `i2c23` device to configure codec clocking and playback-related registers. |
| `nau88l21_set_fll_target_rate_hz(target_rate_hz)` | Validates a target near the firmware audio rate and updates the FLL fractional registers. |

Registers and values are private to `nau88l21.c`; the driver does not expose a
generic codec register access API.

## Ownership

The driver stores the initialized I2C device pointer for later retunes. The I2S
thread performs initialization; audio paths request retunes through
`path_common` helpers.

## Clocking

| Signal | Owner |
|---|---|
| 32 MHz MCLKI | nRF TDM MCK pin driven from PCLK on P1.10. |
| BCLK / LRCK | NAU88L21 codec master. |
| TDM SDOUT / SDIN | nRF TDM data pins. |

The nRF TDM driver is configured as bit-clock and frame-clock slave. The
NAU88L21 FLL is driven from the nRF-provided 32 MHz reference, then the codec
generates the I2S clocks used by the TDM peripheral. Exact divider values,
fractional FLL values, and derived clock rates live in `nau88l21.c`.

The driver has a build assertion for `NAU88L21_I2S_CODEC_CLOCK_MASTER`.

## Init Sequence

`nau88l21_init()`:

1. Stores the I2C device pointer for later retunes.
2. Writes reset register `0x00`.
3. Programs standard I2S for the firmware audio format.
4. Keeps SYSCLK on MCLK while programming the FLL.
5. Programs FLL ratio, integer, prescale, and fractional registers.
6. Writes FLL fractional registers before FLL5/FLL6 so the divider latches
   correctly.
7. Waits for the FLL path to settle, switches SYSCLK to the FLL path, and polls
   FLL lock.
8. Programs codec-master I2S clocks.
9. Runs static DAC/headphone output-path initialization.
10. Runs staged headphone output bring-up.

FLL lock failure is logged as a warning; init continues if I2C transactions
succeed.

## DAC / Headphone Bring-Up

The output path is intentionally staged:

| Stage | Action |
|---|---|
| Static init | Bias, left time slot, boost, Class G timer, analog control, DAC, charge pump, and HP ungrounding. |
| 0 | Enable Class G headphone amps. |
| 1 | Enable charge pump, wait for it to settle, then set playback charge-pump state. |
| 2 | Enable analog DACL/R. |
| 3 | Enable analog DAC clocks. |
| 4-6 | Enable output driver integrators, input stages, and main drivers. |
| 9 | Enable HP boost, Class G playback, and clear DAC test-bias gating. |
| Final | Enable DAC output stage and digital DACL/R, then wait for the output path to settle. |

## Runtime FLL Retuning

`nau88l21_set_fll_target_rate_hz()` retunes by writing FLL7/FLL8 only.

| Check | Behavior |
|---|---|
| Driver not initialized | Returns `-ENODEV`. |
| Target outside the supported retune window | Returns `-EINVAL`. |
| Computed FDCO outside the supported codec range | Returns `-ERANGE`. |
| Valid target | Computes the FLL fractional value and writes FLL7/FLL8. |

Audio paths call this through `path_common` helpers:

| Helper | Behavior |
|---|---|
| `fll_set_fixed(rate_hz)` | Applies a fixed target and pauses automatic path updates. |
| `fll_set_auto()` | Clears fixed mode; path PI control resumes. |
| `fll_get_fixed_rate()` | Returns fixed rate or zero for auto mode. |

## Constraints

- Only codec-master I2S clocking is supported.
- Register programming is fixed for the firmware audio format.
- Runtime retuning is limited to the narrow window enforced by `nau88l21.c`.
- The driver does not expose generic register read/write APIs.
