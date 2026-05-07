# nRF Firmware Test Plan

1. CI build
   Requirement:
   Build `nrf54lm20dk_debug` on every push and PR.
   Purpose:
   Fast regression gate for compile, link, and config changes.

2. USB CDC CLI smoke test
   Interface:
   USB CDC
   Minimum coverage:
   `help`, `get`, `status`, `set mode`, `scan`, `linktest`, `reset`, `i2s tone`
   Pass criteria:
   Device enumerates, accepts commands, returns sane status, and rejects invalid modes.

3. Two-board proprietary radio smoke test
   Setup:
   2 boards, one forced `dongle`, one forced `headset`
   Interface:
   CLI plus `linktest`
   Pass criteria:
   PROP session starts, headset reaches running state, lock is acquired within timeout, RX counters increase, RSSI is plausible, and rebooting into a different mode recovers cleanly.

4. USB descriptor enumeration stability
   Coverage:
   UAC+CDC descriptor set after booting `usb` and `prop_dongle`; CDC-only descriptor set after booting `prop_headset`.
   Pass criteria:
   Device enumerates with the mode-appropriate descriptors and retains functional CDC CLI after rebooting into each mode.

5. Soak test
   Duration:
   Long-run manual or scripted board test
   Pass criteria:
   No stalls, deadlocks, watchdog resets, counter corruption, or stuck mode reboot transitions.

## Lower Priority

6. Unit tests for isolated logic
   Only add these where there is a clean seam and a real payoff.
   Likely targets:
   mode validation, status formatting, packet bookkeeping

7. Failure injection
   Only add after a real failure mode is observed.
   Likely targets:
   mode persistence errors, radio start errors, codec init failures, I2S trigger or write failures