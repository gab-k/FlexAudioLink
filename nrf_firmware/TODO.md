# TODO

Documented here for later follow-up from review feedback on the recent GFSK/CLI refactor.

## High Priority

### 1. GFSK config deadlock while headset is searching

- File: `src/prop_gfsk/link.c`
- Problem: when the local role is `headset` and no dongle packets have been received yet, the link thread can end up calling `k_poll()` with `K_FOREVER`.
- Impact: `prop_gfsk_link_set_config()` callers such as `linktest off`, `set role`, or `set mode` can time out after 1 second because the link thread stops servicing `g_prop_gfsk_config_queue` until an RF packet arrives.
- Fix direction: ensure the link thread always wakes often enough to process config changes even during initial search / unlocked state.

### 2. Timeout can leave queued requests pointing at dead stack storage

- Files: `src/prop_gfsk/link.c`, `src/app_control.c`
- Problem: both `prop_gfsk_link_set_config()` and `app_control_set()` queue request structs that contain pointers to stack-local completion state (`struct k_sem *done`, `bool *result`) and then wait with fixed timeouts.
- Impact: if the caller times out and returns before the worker thread handles the queued request, the worker later dereferences dangling pointers and can corrupt memory instead of failing cleanly.
- Current relevance: this is still present in current code. It is especially plausible on the GFSK path because config requests can already time out while the headset is stuck searching.
- Fix direction: make queued requests own completion state for the full request lifetime, or add explicit cancellation/removal of timed-out requests before returning.

### 3. Web UI ACK parsing broken for `set role` / `set mode`

- File: `src/cli.c`
- Problem: the CLI response was changed from stable keys like `OK role=...` / `OK mode=...` to `OK applied role=...`.
- Impact: the Web UI tracks pending acknowledgements by the original parameter name (`role` / `mode`), so these commands appear to fail or hang in the UI even when the firmware applies them.
- Fix direction: restore ACK keys expected by the Web UI, or update the Web UI protocol/parser in lockstep.

## Medium Priority

### 4. `#S` status lines no longer expose `conn=`

- File: `src/cli.c`
- Problem: periodic `#S` status output replaced `conn=` with `lock=` / `state=`.
- Impact: the current Web UI parser only updates connection state from `conn=`, so the dashboard connection indicator no longer reflects the real device link state.
- Fix direction: either preserve `conn=` in firmware status pushes, or update the Web UI parser to understand the new fields.
