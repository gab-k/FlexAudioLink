# nRF GFSK Ping-Pong TDD Spec

## Status

Living spec for the `nrf_firmware/src/prop_gfsk/` link layer. Replaces the
previous fixed time-slot scheduler with a symmetric ping-pong protocol.
Implemented with a three-state machine (IN_RX, IN_TX, LISTEN) and
timer-based deadlines. Section names use the symbols actually present in
the code (`pgfsk_*`, `PGFSK_*`).

## Rewrite Rule

This rewrite should be treated as a replacement, not an incremental layering
exercise.

Implementation rule:

- anything in the current implementation that is not strictly needed for this
  three-state ping-pong design should be removed entirely

That means:

- do not keep compatibility glue for the fixed-slot scheduler
- do not keep unused timing variables "just in case"
- do not keep role-specific timing branches once the shared ping-pong state
  machine replaces them
- do not preserve diagnostics that only made sense for the old slot-based
  design

The target is a smaller codebase, not two scheduling models living side by
side.

In practice, once the new design is working, the implementation should delete
old machinery such as:

- fixed dongle/headset slot timing policy
- `next_tx_tick` fallback/rx-sync source arbitration
- old role-specific schedule math
- split RX-queue plus TX-done-semaphore control flow if the unified radio
  event queue replaces it

Only keep:

- code required by this spec
- diagnostics explicitly listed in this spec
- hardware-local safety logic required to execute the protocol correctly

## Goal

Use the same timing logic on both devices:

- no fixed dongle/headset slot durations
- no steady-state master/slave cadence
- packet end as the turn anchor
- dynamic payload sizes
- simple state machine

The link should behave like half-duplex ping-pong:

- after a received packet ends, prepare a reply
- after a transmitted packet ends, wait for a peer reply
- if no peer packet arrives in time, reclaim the turn

For the first test-mode iteration:

- no special minimal/keepalive packet behavior is required
- if there is no payload, packet formatting policy can stay simple and be
  improved later

## Important Hardware Decision

For proprietary GFSK mode:

- use `ADDRESS` as the earliest reliable RX-start-like event
- use `PHYEND` as the authoritative packet-end timing anchor
- do **not** use `FRAMESTART`
- do **not** rely on `SYNC` for core state transitions

Reason:

- the nRF54LM20 datasheet describes `FRAMESTART` as an IEEE 802.15.4 event
- the datasheet explicitly warns that `SYNC` can be false, missing, or later
  than `ADDRESS`

So the state machine should be built around:

- `ADDRESS`
- `PHYEND`

## External Link State

Keep the existing shared external state:

- `NO_SERVICE`
- `IN_SERVICE`

This remains shared between dongle and headset. Whether the link is running
at all is tracked separately via the config `enabled` flag (exposed through
`pgfsk_link_is_enabled()`); when disabled, the external state reads as
`NO_SERVICE`.

## Internal State Machine

The implementation uses exactly three internal states:

- `IN_RX`
- `IN_TX`
- `LISTEN`

This state machine is shared by both devices.

On enable, the external state (already `NO_SERVICE` while disabled) stays
`NO_SERVICE` and the internal state machine enters
`LISTEN` with a randomized initial `rx_deadline_tick` and a pre-armed TX
packet. In the current implementation, startup first enters RX and waits
until the radio is actively in `RX` before pre-arming the first TX, even
though `PACKETPTR` is double-buffered.

## State Meaning

### `IN_RX`

Meaning:

- a packet is currently being received
- the transition into this state is triggered by `ADDRESS`

Notes:

- this does not mean the packet is valid yet
- validity is only known at `PHYEND` / CRC result
- a hardware timer deadline is set at `ADDRESS_tick + MAX_PACKET_AIRTIME_US`; if the timer fires before `PHYEND`, the hardware triggers
  `DISABLE` → TX via the pre-armed `DISABLED_TXEN` short
- in steady state, `IN_RX` exits directly to `IN_TX` on RX PHYEND because
  the HW chain into TX is pre-armed during the previous `LISTEN` — see the
  RX Re-Arm Rule section

### `IN_TX`

Meaning:

- local TX has been committed to hardware
- there is no longer any protocol-level path back to `IN_RX`
- this state includes:
  - hardware pre-TX / ramp-up
  - actual on-air transmit time
- this state ends on TX `PHYEND`

### `LISTEN`

Meaning:

- not currently receiving
- not currently transmitting
- waiting either for a peer packet to start or for the current listen timeout
  to expire
- TX is always pre-armed (invariant): the next TX packet is composed and
  staged via `pgfsk_hw_prepare_tx()`, with `DISABLED_TXEN` short active

`LISTEN` has two different timeout policies depending on external service
state:

- in `IN_SERVICE`, `LISTEN` waits for peer `ADDRESS` after a local TX
- in `NO_SERVICE`, `LISTEN` waits for a randomized probe deadline before
  initiating a probe TX

A hardware timer fires at `rx_deadline_tick`; if no peer `ADDRESS` arrives
first, the timer triggers `DISABLE` → TX via the pre-armed short.

This is the passive waiting state after a local TX or during out-of-service
acquisition.

## State Transitions

### `LISTEN -> IN_RX`

Trigger:

- `ADDRESS` event from the radio

Meaning:

- the peer packet has started enough that we should receive instead of trying
  to take the turn

Action:

- transition to `IN_RX`
- set new deadline timer at `ADDRESS_tick + MAX_PACKET_AIRTIME_US`

### `LISTEN -> IN_TX` (timeout)

Trigger:

- hardware timer fires at `rx_deadline_tick` (delivered as `TIMEOUT` event)

Meaning:

- if `service_state == IN_SERVICE`:
  - we expected a peer reply after our previous TX
  - no peer `ADDRESS` arrived before `rx_deadline_tick`
  - reclaim the turn
- if `service_state == NO_SERVICE`:
  - the randomized probe timeout expired
  - initiate a probe TX

Action:

- the timer ISR posts a `TIMEOUT` event
- the link thread calls `pgfsk_hw_trigger_prepared_tx()` to trigger the pre-armed
  TX (hardware chains `DISABLE` → `TXEN` via the `DISABLED_TXEN` short)
- transition to `IN_TX`
- once this transition happens, the turn is considered reclaimed; there is no
  `IN_TX -> IN_RX` transition for a late peer packet

### `IN_RX -> IN_TX`

Trigger:

- RX `PHYEND` (delivered as `RX_OK` or `RX_BAD`), or
- hardware timer fires (delivered as `TIMEOUT` event) if `PHYEND` doesn't
  arrive in time

Meaning:

- a peer packet just finished (or timed out); hardware has already chained
  into TXEN via the pre-armed `DISABLED_TXEN` short — local TX is on air

Action:

- clear the deadline timer
- record RX outcome (valid/invalid, seq/loss accounting)
- use RX `PHYEND` (or timeout tick) as the new timing anchor
- transition to `IN_TX`

### `IN_TX -> LISTEN`

Trigger:

- TX `PHYEND` (delivered as `TX_END`)

Meaning:

- local transmission just ended
- hardware has already chained TX→RX via `DISABLED_RXEN` (shorts swap was
  performed in the TX-phase `ADDRESS` ISR)
- now wait for the peer reply

Action:

- increment `next_tx_seq`
- set `rx_deadline_tick = event->tick + MAX_PACKET_AIRTIME_US`
  (in service), or `+ random(0..RANDOM_TICKS_US)` additional jitter in
  `NO_SERVICE`
- transition to `LISTEN`
- compose the next reply into `prepared_tx_packet`
- call `pgfsk_hw_prepare_tx(&prepared_tx_packet)` to stage DMA +
  swap shorts to `DISABLED_TXEN`
- hardware is now pre-armed so the next RX PHYEND (or timeout) will
  chain directly into TX
- arm the deadline timer at `rx_deadline_tick`

## Timing Deadlines

The protocol uses a single live timing value:

- `rx_deadline_tick`

This value is programmed into a hardware timer compare register. When the
timer fires, the ISR posts a `TIMEOUT` event to the link thread.

Rules:

- on `ADDRESS` (entering `IN_RX`):
  `rx_deadline_tick = ADDRESS_tick + MAX_PACKET_AIRTIME_US`
- after TX completes (entering `LISTEN`) while `service_state == IN_SERVICE`:
  `rx_deadline_tick = tx_end_tick + MAX_PACKET_AIRTIME_US`
- after TX completes while `service_state == NO_SERVICE`:
  `rx_deadline_tick = tx_end_tick + MAX_PACKET_AIRTIME_US +
  random(0..RANDOM_TICKS_US)`
- on entry to `NO_SERVICE`:
  `rx_deadline_tick = now + FIXED_TICK_US + random(0..RANDOM_TICKS_US)`

`rx_deadline_tick` is reused across `IN_RX` and `LISTEN`:

- in `IN_RX`, it is the deadline for the current packet to finish after
  `ADDRESS`
- in `LISTEN` + `IN_SERVICE`, it is the reclaim timeout after our previous TX
- in `LISTEN` + `NO_SERVICE`, it is the randomized probe deadline

The two states are mutually exclusive so a single field is sufficient.

There is no fixed frame interval and no per-role slot timing.

## Tick Arithmetic And Wrap Safety

All live radio/link timestamps in this design are 32-bit modulo ticks.

Assumption:

- the hardware timer is a free-running 32-bit microsecond counter
- tick arithmetic therefore wraps naturally every `2^32` ticks

Allowed arithmetic:

- `deadline = anchor + delta_us`
- `delta_us` values in this protocol are always small relative to `2^31` ticks

Hardware timer behavior:

- the timer compare register (CC[3]) is programmed with `rx_deadline_tick`
- the hardware handles wrap correctly: the compare fires when the timer
  counter reaches the CC value
- no software wrap-safe comparison is needed since timeouts are handled via
  hardware interrupt, not polling
- when software arms a deadline too late or too close to the current tick,
  `pgfsk_hw_set_deadline()` clamps it forward by a small minimum lead instead
  of programming an immediately stale compare value

## First-Iteration Timing Constants

Keep constants minimal:

- `MAX_PACKET_AIRTIME_US`
  - worst-case on-air duration for one packet in the current mode
  - also reused as the in-service `LISTEN` reclaim window after TX
- `SYNC_LOSS_TURNS`
  - number of consecutive listen-timeout turns with no peer activity before
    leaving service
- `FIXED_TICK_US`
- `RANDOM_TICKS_US`
  - randomized probe timing when entering or re-entering `NO_SERVICE`:
    `FIXED_TICK_US + random(0..RANDOM_TICKS_US)`
  - used to break symmetry on simultaneous boot and after service loss

## Service Behavior

`IN_SERVICE` tracks link continuity and peer activity, not whether the current
payload decoded successfully.

### Entering service

Enter `IN_SERVICE` on the first valid received packet.

### Staying in service

Stay in service while peer packets continue arriving often enough.

Specifically:

- `RX_OK` counts as peer activity
- `RX_BAD` also counts as peer activity
- packet validity affects payload delivery and CRC/accounting, not whether the
  connection is still considered in service

### Leaving service

Leave service when `SYNC_LOSS_TURNS` consecutive turns have no peer activity
at all (listen timeouts only).

On any transition into `NO_SERVICE`:

- reset `consecutive_rx_misses`
- remain in `LISTEN`
- re-seed `rx_deadline_tick = now + FIXED_TICK_US + random(0..RANDOM_TICKS_US)`
- restore RX listen posture
- ensure the next probe/reply TX is staged again via `pgfsk_hw_prepare_tx()`
- do not immediately reclaim the turn on the old in-service cadence
- after a `NO_SERVICE` probe TX completes, the following `LISTEN` timeout is
  anchored from that probe packet's `PHYEND`:
  `rx_deadline_tick = phyend_tick + MAX_PACKET_AIRTIME_US + random(0..RANDOM_TICKS_US)`

Implementation note:

- if restoring RX listen posture or TX pre-arm fails on entry to `NO_SERVICE`,
  the implementation may hard-reset the radio block and retry once before
  giving up and disabling the link

Important distinction:

- a bad CRC packet still gives useful timing and proves the peer is present
- a bad CRC packet should **not** count as a missed turn for service loss
- this is intentional: `IN_SERVICE` models connection continuity, not payload
  validity
- only listen timeouts (no peer activity) increment `consecutive_rx_misses`

## Acquisition / Recovery

There is no fixed master/slave timing and no dedicated acquisition state.

Both devices start in `LISTEN` when entering `NO_SERVICE`. The normal
state machine handles acquisition:

- the initial `rx_deadline_tick` is set to
  `now + FIXED_TICK_US + random(0..RANDOM_TICKS_US)` to break
  symmetry on simultaneous boot
- whichever device times out first transitions to `IN_TX` and sends a probe
- the other device sees `ADDRESS` and receives
- after this first exchange, the protocol is self-sustaining

The same randomized probe scheduling is used after service loss, not just on
initial boot.

Collision / symmetry note:

- if both devices time out and reclaim at nearly the same time, both may enter
  `IN_TX` and transmit in the same turn
- during that collision turn, neither side is listening, so no peer activity
  is observed
- in `NO_SERVICE`, this is acceptable for the first iteration; the randomized
  probe deadlines eventually break symmetry
- in `IN_SERVICE`, repeated simultaneous reclaim collisions are treated like
  repeated listen-timeout turns with no peer activity
- if that persists long enough, both sides may leave `IN_SERVICE`, return to
  `NO_SERVICE`, and recover using the same randomized probe behavior
- no special collision-resolution or tie-breaker mechanism is required in the
  first iteration

## Disable / Stop Behavior

Disable is an administrative control action, not a protocol turn transition.
It overrides the internal state machine from any state.

On disable request:

- `pgfsk_link_is_enabled()` returns false; external service state is reset to
  `NO_SERVICE`
- the internal three-state machine is no longer active
- no further RX/TX turn-taking decisions are made until re-enabled
- pending app TX and RX queue contents may be discarded
- pending or stale radio events may be discarded
- re-enable always starts fresh in `NO_SERVICE` / `LISTEN`; prior turn timing
  and pending packet state are not preserved

Hardware expectation:

- `radio_hw` should stop or abort any in-progress RX/TX activity as needed to
  bring the radio to an idle/quiescent state
- after disable completes, no pre-disable hardware event may mutate link-layer
  protocol state

## Minimal Runtime State

Runtime should stay small. Actual shape (`link.c`):

```c
enum pgfsk_internal_state {
	PGFSK_STATE_IN_RX = 0,
	PGFSK_STATE_IN_TX,
	PGFSK_STATE_LISTEN,
};

struct pgfsk_link_runtime {
	struct pgfsk_link_config config;
	enum pgfsk_link_state service_state;
	enum pgfsk_internal_state state;
	struct link_stats stats;

	uint32_t rx_deadline_tick;    // programmed into hardware timer

	uint8_t  consecutive_rx_misses;
	uint64_t in_service_since_cyc;

	uint16_t next_tx_seq;
	uint16_t last_rx_seq;
	bool     have_last_rx_seq;

	struct pgfsk_packet prepared_tx_packet;   // staging buffer for next TX

	struct k_spinlock lock;
};
```

Meaning of the timing fields:

- `rx_deadline_tick`
  - programmed into hardware timer CC register
  - when timer fires, ISR posts `TIMEOUT` event
  - in `IN_RX`: deadline for current RX completion
  - in `LISTEN` + `IN_SERVICE`: reclaim timeout after our previous TX
  - in `LISTEN` + `NO_SERVICE`: randomized probe deadline

Invariant: when entering `LISTEN`, the next TX packet is always pre-composed
into `prepared_tx_packet` and staged via `pgfsk_hw_prepare_tx()`. This
ensures the hardware-chained RX→TX path works, and timeout-triggered TX
works without additional preparation.

## Packet Format

On-air frame (per nRF radio config with `lflen = 8`, `s0len = 0`, `s1len = 0`):

```
[PREAMBLE] [ACCESS_ADDR] [LENGTH] [PAYLOAD] [CRC]
```

PREAMBLE, ACCESS_ADDR, and CRC are managed by hardware and never appear in
the DMA buffer. The buffer pointed to by `PACKETPTR` contains:

```
offset 0:    LENGTH byte (value written by link, read by peer HW)
offset 1..N: PAYLOAD bytes (N = LENGTH)
```

The link-layer packet struct mirrors this layout:

```c
struct pgfsk_packet {
	uint8_t  length;                           // HW LFLEN byte
	uint16_t seq;                              // first 2 bytes of PAYLOAD
	uint8_t  data[PGFSK_PAYLOAD_MAX_LEN];      // remaining PAYLOAD bytes
} __packed __aligned(4);
```

Rules:

- `length = PGFSK_PACKET_METADATA_LEN + data_bytes_used`
  (currently `METADATA_LEN = 2`, covering `seq`)
- `PGFSK_PAYLOAD_MAX_LEN = 252` sets the capacity of `data[]`
- Effective maximum `length` byte value = `METADATA_LEN + PAYLOAD_MAX_LEN = 254`
- Each packet transmits only `length` bytes of PAYLOAD on air — short packets
  (e.g. 2-byte seq-only keepalive) use less airtime; full audio packets
  (`length = 254`) reach the LFLEN=8 upper bound
- `__packed` is load-bearing: without it, natural alignment of `seq`
  introduces a pad byte at offset 1 and silently breaks the wire format.
  Compile-time asserts in `radio_hw.c` lock `offsetof(seq)` and
  `offsetof(data)` to catch this.

## Radio Event Model

The hardware layer (`radio_hw`) provides one ordered event stream consumed by
the link thread.

Event types:

```c
enum pgfsk_hw_event_type {
	PGFSK_HW_EVENT_RX_ADDRESS = 0,
	PGFSK_HW_EVENT_RX_OK,
	PGFSK_HW_EVENT_RX_BAD,
	PGFSK_HW_EVENT_TX_END,
	PGFSK_HW_EVENT_TIMEOUT,
};
```

Event payload:

```c
struct pgfsk_hw_event {
	enum pgfsk_hw_event_type type;
	uint32_t tick;                 // ADDRESS tick, PHYEND tick, or deadline tick
	struct pgfsk_packet packet;    // valid only for RX_OK
	int16_t  rssi_dbm;             // valid for RX_OK and RX_BAD
};
```

Use:

- `RX_ADDRESS`
  - drives `LISTEN -> IN_RX`
- `RX_OK`
  - valid packet ended at `PHYEND`
- `RX_BAD`
  - invalid packet ended at `PHYEND`
- `TX_END`
  - local TX ended at `PHYEND`
- `TIMEOUT`
  - hardware timer fired at `rx_deadline_tick`
  - drives `LISTEN -> IN_TX` or `IN_RX -> IN_TX`

## State Handling Rules

### On `RX_ADDRESS`

If current state is:

- `LISTEN`
  - transition to `IN_RX`
  - set deadline timer at `ADDRESS_tick + MAX_PACKET_AIRTIME_US`
- otherwise
  - ignore

Core rule:

- only `LISTEN` may accept `ADDRESS` as a turn-taking event
- after `LISTEN` times out and the link enters `IN_TX`, a later `ADDRESS`
  does not pull the state machine back into `IN_RX`

RX is logically active only during `LISTEN`. During `IN_TX`, the link does
not accept new peer turn claims.

### On `RX_OK`

Actions:

1. clear deadline timer
2. queue payload to app RX queue
3. reset `consecutive_rx_misses = 0`
4. enter or remain `IN_SERVICE`; update seq / loss accounting
5. transition to `IN_TX` (hardware already chained RX→TX via `DISABLED_TXEN`)

### On `RX_BAD`

Actions:

1. clear deadline timer
2. reset `consecutive_rx_misses = 0`
3. update CRC stats (tracked in `radio_hw` stats)
4. transition to `IN_TX` (hardware already chained RX→TX via `DISABLED_TXEN`)

Reason:

- even a bad packet still occupied the medium until a known `PHYEND`
- the structural state transition stays identical
- only bookkeeping differs between valid and invalid RX
- a bad CRC packet proves the peer is present, so it does **not** increment
  `consecutive_rx_misses` — only listen timeouts do

### On `TIMEOUT`

When in `IN_RX`:

- `PHYEND` never arrived — treat as a failed reception
- increment `rx_incomplete_count`
- reset `consecutive_rx_misses = 0`
- call `pgfsk_hw_trigger_prepared_tx()` to trigger the already-staged TX
- transition to `IN_TX`

When in `LISTEN`:

1. if `service_state == IN_SERVICE`:
   - increment `consecutive_rx_misses`
   - if threshold reached:
     - enter `NO_SERVICE`
     - reset `consecutive_rx_misses`
     - set `rx_deadline_tick = now + FIXED_TICK_US +
       random(0..RANDOM_TICKS_US)`
     - restore RX listen posture and re-stage the next TX
     - arm deadline timer
     - remain in `LISTEN`
     - return (do not TX)
2. call `pgfsk_hw_trigger_prepared_tx()` to trigger the already-staged TX
3. transition to `IN_TX`

Important:

- `LISTEN` timeout is a hard protocol boundary
- once timeout has fired and the state machine enters `IN_TX`, the local
  side owns the turn
- there is no late-`ADDRESS` recovery path back to `IN_RX`

### On `TX_END`

Actions:

1. increment `next_tx_seq`
2. transition to `LISTEN`
3. if `service_state == IN_SERVICE`, set:
   - `rx_deadline_tick = phyend_tick + MAX_PACKET_AIRTIME_US`
4. if `service_state == NO_SERVICE`, set:
   - `rx_deadline_tick = phyend_tick + MAX_PACKET_AIRTIME_US +
     random(0..RANDOM_TICKS_US)`
5. compose the next reply into `prepared_tx_packet`, then call
   `pgfsk_hw_prepare_tx(&prepared_tx_packet)` to stage the HW chain. The
   next RX PHYEND (or timeout) will chain directly into TX.
6. arm deadline timer at `rx_deadline_tick`

## ISR Ownership

For the first version:

- ISRs should not mutate link-layer state directly
- ISRs should only generate radio events
- the link thread owns the state machine

Exception:

- hardware-local flags needed for TX cancel/freeze are fine inside `radio_hw`

This keeps the design understandable:

- `radio_hw` owns exact radio mechanics
- `link.c` owns protocol state

## RX Re-Arm Rule

RX is logically active only during `LISTEN` (at the protocol layer). During
`IN_TX`, the protocol does not accept a peer turn claim. This is simple
because the protocol is strict ping-pong — each peer transmits exactly one
packet per turn. There is no reason to listen during our own turn.

On the `LISTEN` timeout path specifically:

- the timeout itself decides turn ownership
- even if the radio hardware is still physically in RX until the timeout
  handler calls `pgfsk_hw_trigger_prepared_tx()`, that does not create an
  `IN_TX -> IN_RX` transition
- any late `ADDRESS` is ignored by core link logic

### Hardware re-arm mechanism

Re-arm is driven entirely by RADIO SHORTS (DPPI-style hardware chains), not
by the TX_END ISR. The base shorts active at all times are:

- `READY_START`          (ramp-up → start on time)
- `PHYEND_DISABLE`       (end-of-packet → DISABLED)
- `ADDRESS_RSSISTART`    (address detected → start RSSI sample)

A single additional bit selects what DISABLED chains into:

- `DISABLED_RXEN` — after any PHYEND, the radio ramps back into RX
- `DISABLED_TXEN` — after the NEXT PHYEND (or manual DISABLE), the radio
  ramps into TX using the currently programmed `PACKETPTR`

Steady-state sequence:

1. `pgfsk_hw_start()` sets shorts = `BASE | DISABLED_RXEN`; `link.c` calls
   `pgfsk_hw_start_listen()` once to trigger `RXEN` initially. On initial
   enable, `link.c` then waits for `pgfsk_hw_wait_for_rx_active()` before
   calling `pgfsk_hw_prepare_tx()`, to avoid corrupting the first RX packet
   by switching `PACKETPTR` to the TX buffer too early.
2. After any RX PHYEND, `DISABLED_RXEN` chains the radio back into RX with
   no CPU involvement. `radio_hw` does not re-arm RX in software.
3. To pre-arm a reply, `link.c` calls `pgfsk_hw_prepare_tx(&packet)`. This
   copies the packet into the TX DMA buffer, sets `PACKETPTR` to it, and
   swaps shorts to `BASE | DISABLED_TXEN`. The NEXT PHYEND (i.e. the
   incoming peer RX's PHYEND) now chains directly into TXEN — a
   hardware-chained RX→TX turnaround.
4. During that local TX, the RADIO `ADDRESS` interrupt fires while the
   outbound preamble/address is on air. The ISR notices `in_tx_phase`,
   repoints `PACKETPTR` back to the RX buffer, and swaps shorts back to
   `BASE | DISABLED_RXEN`. PHYEND of the local TX then chains the radio
   back into RX — a hardware-chained TX→RX turnaround.
5. On the listen-timeout or IN_RX-timeout path, `link.c` calls
   `pgfsk_hw_trigger_prepared_tx()` which triggers the `DISABLE` task.
   Since TX was already pre-armed (shorts = `DISABLED_TXEN`, packet staged),
   DISABLE chains into TXEN.

### TX pre-arm invariant

When entering `LISTEN`, the link always pre-arms the next TX:

- `link.c` composes the packet into `prepared_tx_packet` and calls
  `pgfsk_hw_prepare_tx()` to stage it
- shorts are swapped to `DISABLED_TXEN`
- a deadline timer is armed

This invariant ensures:

- RX PHYEND chains directly into TX (hardware path)
- timeout-triggered TX works via `pgfsk_hw_trigger_prepared_tx()` (the packet
  is already staged, so `trigger_prepared_tx` just triggers `DISABLE`)

If `pgfsk_hw_trigger_prepared_tx()` fails, the current turn is abandoned and
the link falls back to `NO_SERVICE` reacquisition rather than trying to
preserve steady-state timing.

If `pgfsk_hw_prepare_tx()` fails (rare edge case), the link logs a warning
but continues. The next turn may be missed.

If the peer replies fast enough that ADDRESS and RX_OK/RX_BAD are queued
before the link thread processes TX_END, the event ordering is still
correct: TX_END → LISTEN, ADDRESS → IN_RX, RX_OK → IN_TX.

## Hardware Responsibilities

`radio_hw` owns:

- RX `ADDRESS` timestamp capture (DPPI → TIMER CC[2])
- RX/TX `PHYEND` timestamp capture (DPPI → TIMER CC[4])
- deadline timer (TIMER CC[3] compare → `TIMEOUT` event)
- continuous RX via `DISABLED_RXEN` short (no software re-arm)
- hardware-chained RX→TX via `DISABLED_TXEN` short (staged by
  `pgfsk_hw_prepare_tx`)
- hardware-chained TX→RX via shorts swap in the TX-phase `ADDRESS` ISR
- manual `DISABLE`-and-TX for timeout reclaims (`pgfsk_hw_trigger_prepared_tx`)
- ordered delivery of radio events via `pgfsk_hw_event_msgq()`

`radio_hw` exposes (see `radio_hw.h`):

- `pgfsk_hw_init()`, `pgfsk_hw_start()`, `pgfsk_hw_stop()`
- `pgfsk_hw_set_role(role)`
- `pgfsk_hw_start_listen()`           — explicit initial RX arm
- `pgfsk_hw_wait_for_rx_active()`     — startup-only wait until radio state is `RX`
- `pgfsk_hw_prepare_tx(packet)`       — stage packet + swap shorts to TXEN
- `pgfsk_hw_trigger_prepared_tx()`   — trigger DISABLE for the pre-staged TX
- `pgfsk_hw_set_deadline(tick)`       — arm timer CC[3], enable interrupt
- `pgfsk_hw_clear_deadline()`         — disarm timer, clear pending interrupt
- `pgfsk_hw_dequeue_event(evt, to)` / `pgfsk_hw_event_msgq()`
- `pgfsk_hw_get_tick()`               — 1 µs free-running timer read
- `pgfsk_hw_get_stats(stats)` / `pgfsk_hw_reset_stats()`

`radio_hw` does not own:

- service/no-service policy
- packet sequencing policy
- retransmission policy
- the three-state link state machine

## Link Responsibilities

`link.c` owns:

- the three-state machine (IN_RX, IN_TX, LISTEN)
- packet preparation and pre-arming (invariant: always armed in LISTEN)
- deadline timer management (arm/clear via `pgfsk_hw_set_deadline` /
  `pgfsk_hw_clear_deadline`)
- service/no-service transitions
- sequence/loss accounting
- app TX/RX queues

## Diagnostics To Keep

Keep existing useful counters:

- `lost`
- `crc_err`
- `dlate` (`deadline_late_count`)
- `rxi` (`rx_incomplete_count`)
- `outages`
- `in_service_ms` (current uninterrupted in-service streak; derived live from
  `in_service_since_cyc`, reports 0 while out of service, resets on service
  loss)
- `txf` (`tx_trigger_fail_count`)
- loss-burst histogram

Possible new counters:

- `listen_timeouts`
- `rx_bad_turn_anchors`

## Invariants

1. Both devices use the same steady-state protocol logic.
2. `PHYEND` is the only authoritative turn boundary.
3. `ADDRESS` is the only early RX-start-like gate used in core logic.
4. `FRAMESTART` is not used.
5. The link-layer state machine has only three internal states:
   `IN_RX`, `IN_TX`, `LISTEN`.
6. RX is only armed during `LISTEN`. `IN_TX` does not listen.
7. TX is always pre-armed when entering `LISTEN` (packet staged, shorts set
   to `DISABLED_TXEN`).
8. Timeouts are handled via hardware timer compare → `TIMEOUT` event, not
   software polling.
9. `LISTEN` timeout is a hard boundary; there is no `IN_TX -> IN_RX`
   recovery path for a late peer packet.
10. Simultaneous reclaim collisions are allowed; recovery may fall back
    through `NO_SERVICE` randomized reacquisition.
11. Disable overrides protocol state; after disable, pre-disable radio events
    must not re-enter the state machine.

## First Iteration Success Criteria

The implementation is successful if:

- there is no per-role slot timing logic in `link.c`
- both boards run the same three-state internal state machine
- timing is driven by packet ends and hardware timer deadlines
- variable payload sizes work without timing-policy changes
- the link thread is fully event-driven (no polling loops)
- TX is always pre-armed when entering `LISTEN`
