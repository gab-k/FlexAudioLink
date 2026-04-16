# nRF GFSK Ping-Pong TDD Spec

## Status

Design target for the next `nrf_firmware/` link-layer rewrite.
This replaces the fixed time slots with a simpler symmetric ping-pong protocol.
This version is intentionally minimal.

## Rewrite Rule

This rewrite should be treated as a replacement, not an incremental layering
exercise.

Implementation rule:

- anything in the current implementation that is not strictly needed for this
  four-state ping-pong design should be removed entirely

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

- `DISABLED`
- `NO_SERVICE`
- `IN_SERVICE`

This remains shared between dongle and headset.

## Internal State Machine

The first implementation should use exactly these four internal states:

- `IN_RX`
- `PREPARE_PACKET`
- `IN_TX`
- `LISTEN`

This state machine is shared by both devices.

On enable (`DISABLED` -> `NO_SERVICE`), the internal state machine enters
`LISTEN` with a randomized initial `rx_deadline_tick`.

## State Meaning

### `IN_RX`

Meaning:

- a packet is currently being received
- the transition into this state is triggered by `ADDRESS`

Notes:

- this does not mean the packet is valid yet
- validity is only known at `PHYEND` / CRC result
- if `PHYEND` does not arrive within `ADDRESS_tick + MAX_PACKET_AIRTIME_US`,
  treat as a failed reception and transition to `PREPARE_PACKET` (same as
  `RX_BAD`)

### `PREPARE_PACKET`

Meaning:

- the local side owns the next turn
- the reply packet is being assembled
- local TX is not yet committed to hardware
- there is no protocol-level path from this state back to `IN_RX`
- if this state was entered from a `LISTEN` timeout, that timeout already
  decided turn ownership; any later peer `ADDRESS` is too late for this turn
- transmission may be committed once both are true:
  - packet preparation is complete
  - `IFS_US` has elapsed since the relevant anchor

This is intentionally simple:

- packet preparation and IFS waiting are merged into one state

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

`LISTEN` has two different timeout policies depending on external service
state:

- in `IN_SERVICE`, `LISTEN` waits for peer `ADDRESS` after a local TX
- in `NO_SERVICE`, `LISTEN` waits for a randomized probe deadline before
  initiating a probe TX

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

### `LISTEN -> PREPARE_PACKET`

Trigger:

- `LISTEN` timeout exceeded

Meaning:

- if `service_state == IN_SERVICE`:
  - we expected a peer reply after our previous TX
  - no peer `ADDRESS` arrived before `rx_deadline_tick`
  - reclaim the turn
- if `service_state == NO_SERVICE`:
  - the randomized probe timeout expired
  - initiate a probe TX

Action:

- transition to `PREPARE_PACKET`
- once this transition happens, the turn is considered reclaimed; there is no
  `PREPARE_PACKET -> IN_RX` transition for a late peer packet

### `IN_RX -> PREPARE_PACKET`

Trigger:

- RX `PHYEND`

Meaning:

- a peer packet just finished
- valid or invalid, the medium is now free at a known end time

Action:

- record RX outcome
- use RX `PHYEND` as the new timing anchor
- transition to `PREPARE_PACKET`

### `PREPARE_PACKET -> IN_TX`

Trigger:

- packet preparation complete
- `IFS_US` elapsed since the current anchor

Meaning:

- there is no additional protocol event needed
- once payload is ready and IFS has elapsed, transition to `IN_TX` and then
  enable hardware TX
- once in `IN_TX`, there is no going back to RX for this turn

Action:

- transition to `IN_TX`
- enable TX in hardware
- if hardware enable fails after the transition:
  - transition to `LISTEN`
  - set `rx_deadline_tick = now + 2 * MAX_PACKET_AIRTIME_US`
  - treat as a missed turn and wait for possible peer reclaim activity

### `IN_TX -> LISTEN`

Trigger:

- TX `PHYEND`

Meaning:

- local transmission just ended
- now wait for the peer reply

Action:

- store the next `rx_deadline_tick`
- transition to `LISTEN`

## Timing Deadlines

The protocol uses two live timing values:

- `tx_ready_tick`
- `rx_deadline_tick`

Rules:

- on `ADDRESS` (entering `IN_RX`), `rx_deadline_tick = ADDRESS_tick + MAX_PACKET_AIRTIME_US`
- after RX completes, `tx_ready_tick = rx_end_tick + IFS_US`
- after TX completes (entering `LISTEN`) while `service_state == IN_SERVICE`,
  `rx_deadline_tick = tx_end_tick + MAX_PACKET_AIRTIME_US`
- on entry to `NO_SERVICE`, `LISTEN` is re-seeded with a randomized probe
  timeout:
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

Required rule:

- all deadline comparisons must be wrap-safe
- do **not** compare live ticks using raw relational operators such as
  `now >= deadline`, `now < deadline`, or `a > b`

Use signed modular subtraction instead:

```c
static inline int32_t tick_diff(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b);
}

static inline bool tick_reached(uint32_t now, uint32_t deadline)
{
	return tick_diff(now, deadline) >= 0;
}

static inline bool tick_before(uint32_t a, uint32_t b)
{
	return tick_diff(a, b) < 0;
}
```

Meaning:

- `tick_reached(now, deadline)` is true once `now` reaches or passes
  `deadline`, even across 32-bit wrap
- `tick_before(a, b)` is true when `a` is still earlier than `b`, even across
  wrap

Allowed arithmetic:

- `deadline = anchor + delta_us`
- `delta_us` values in this protocol are always small relative to `2^31` ticks
- therefore signed-difference comparisons are unambiguous for all live
  deadlines in this design

Implementation rule:

- any logic that waits for `tx_ready_tick` or `rx_deadline_tick` must use the
  wrap-safe helpers above or an equivalent signed-difference form
- the same rule applies to timeout polling, missed-turn detection, and any
  hardware scheduling guard that checks whether a tick is in the past

## First-Iteration Timing Constants

Keep constants minimal:

- `IFS_US`
  - delay from packet end to local reply start
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
- do not immediately reclaim the turn on the old in-service cadence
- after a `NO_SERVICE` probe TX completes, the following `LISTEN` timeout is
  anchored from that probe packet's `PHYEND`:
  `rx_deadline_tick = phyend_tick + MAX_PACKET_AIRTIME_US + random(0..RANDOM_TICKS_US)`

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
  `now + FIXED_TICK_US + random(0..RANDOM_TICKS_US)` to break symmetry on
  simultaneous boot
- whichever device times out first transitions to `PREPARE_PACKET` and
  sends a probe TX
- the other device sees `ADDRESS` and receives
- after this first exchange, the protocol is self-sustaining

The same randomized probe scheduling is used after service loss, not just on
initial boot.

Collision / symmetry note:

- if both devices time out and reclaim at nearly the same time, both may enter
  `PREPARE_PACKET` and transmit in the same turn
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

- external service state becomes `DISABLED`
- the internal four-state machine is no longer active
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

First version runtime should stay small:

```c
enum prop_gfsk_internal_state {
	PROP_GFSK_STATE_IN_RX = 0,
	PROP_GFSK_STATE_PREPARE_PACKET,
	PROP_GFSK_STATE_IN_TX,
	PROP_GFSK_STATE_LISTEN,
};

struct prop_gfsk_link_runtime {
	struct prop_gfsk_link_config config;
	enum prop_gfsk_link_state service_state;      // DISABLED / NO_SERVICE / IN_SERVICE
	enum prop_gfsk_internal_state state;          // IN_RX / PREPARE_PACKET / IN_TX / LISTEN
	struct link_stats stats;

	uint32_t tx_ready_tick;       // earliest legal TX start while in PREPARE_PACKET
	uint32_t rx_deadline_tick;  // packet-end deadline in IN_RX, ADDRESS/probe deadline in LISTEN

	uint8_t consecutive_rx_misses;
	uint64_t in_service_since_cyc;
};
```

Meaning of the timing fields:

- `tx_ready_tick`
  - earliest legal TX start after the current turn anchor plus `IFS_US`
- `rx_deadline_tick`
  - deadline for current RX completion in `IN_RX`
  - latest acceptable peer `ADDRESS` in `LISTEN` while `IN_SERVICE`
  - randomized probe deadline in `LISTEN` while `NO_SERVICE`

This is simpler than keeping historical RX/TX end ticks in link state.
The link only stores the deadlines it still cares about.

## Radio Event Model

For simplicity, the hardware layer should provide one ordered event stream.

Suggested event types:

```c
enum prop_gfsk_radio_event_type {
	PROP_GFSK_RADIO_EVENT_RX_ADDRESS = 0,
	PROP_GFSK_RADIO_EVENT_RX_OK,
	PROP_GFSK_RADIO_EVENT_RX_BAD,
	PROP_GFSK_RADIO_EVENT_TX_END,
};
```

Suggested payload:

```c
struct prop_gfsk_radio_event {
	enum prop_gfsk_radio_event_type type;
	uint32_t tick;                 // ADDRESS tick or PHYEND tick
	struct prop_gfsk_packet packet; // valid only for RX_OK, keep current definition
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
  - local TX ended

## State Handling Rules

### On `RX_ADDRESS`

If current state is:

- `LISTEN`
  - transition to `IN_RX`
- otherwise
  - ignore

Core rule:

- only `LISTEN` may accept `ADDRESS` as a turn-taking event
- after `LISTEN` times out and the link enters `PREPARE_PACKET`, a later
  `ADDRESS` does not pull the state machine back into `IN_RX`

RX is logically active only during `LISTEN`. During `PREPARE_PACKET` and
`IN_TX`, the link does not accept new peer turn claims, even if hardware has
not yet been deliberately disabled on the timeout-to-prepare path.

### On `RX_OK`

Actions:

1. use `tick` as the new turn anchor
2. synchronously prepare the next local packet
3. set:
   - `tx_ready_tick = tick + IFS_US`
4. transition to `PREPARE_PACKET`
5. queue payload to app RX queue
6. reset `consecutive_rx_misses = 0`
7. enter or remain `IN_SERVICE`

### On `RX_BAD`

Actions:

1. use `tick` as the new turn anchor
2. synchronously prepare the next local packet
3. set:
   - `tx_ready_tick = tick + IFS_US`
4. transition to `PREPARE_PACKET`
5. update CRC stats
6. reset `consecutive_rx_misses = 0`

Reason:

- even a bad packet still occupied the medium until a known `PHYEND`
- the structural state transition stays identical
- only bookkeeping differs between valid and invalid RX
- a bad CRC packet proves the peer is present, so it does **not** increment
  `consecutive_rx_misses` — only listen timeouts do

### On `IN_RX` timeout

When in `IN_RX`, if:

- `tick_reached(now, rx_deadline_tick)`

where `rx_deadline_tick = ADDRESS_tick + MAX_PACKET_AIRTIME_US`

then:

- `PHYEND` never arrived — treat as a failed reception
- handle identically to `RX_BAD` (use `rx_deadline_tick` as the turn anchor)

### On prepare completion

When in `PREPARE_PACKET`:

packet preparation is done immediately on state entry.

If:

- `tick_reached(now, tx_ready_tick)`

then:

- transition to `IN_TX`
- call `radio_hw` to enable TX
- if hardware enable fails after the transition:
  - transition to `LISTEN`
  - set `rx_deadline_tick = now + 2 * MAX_PACKET_AIRTIME_US`
  - treat as a missed turn and wait for possible peer reclaim activity

No extra protocol event is required.

### On `TX_END`

Actions:

1. if `service_state == IN_SERVICE`, set:
   - `rx_deadline_tick = phyend_tick + MAX_PACKET_AIRTIME_US`
2. if `service_state == NO_SERVICE`, set:
   - `rx_deadline_tick = phyend_tick + MAX_PACKET_AIRTIME_US + random(0..RANDOM_TICKS_US)`
3. transition to `LISTEN`

### On listen timeout

When in `LISTEN`, if:

- `tick_reached(now, rx_deadline_tick)`

then:

1. if `service_state == IN_SERVICE`:
   - increment `consecutive_rx_misses`
   - if threshold reached:
     - enter `NO_SERVICE`
     - reset `consecutive_rx_misses`
     - set
       `rx_deadline_tick = now + FIXED_TICK_US + random(0..RANDOM_TICKS_US)`
     - remain in `LISTEN`
   - otherwise:
     - set `tx_ready_tick = rx_deadline_tick + IFS_US`
     - transition to `PREPARE_PACKET`
2. if `service_state == NO_SERVICE`:
   - set `tx_ready_tick = rx_deadline_tick + IFS_US`
   - transition to `PREPARE_PACKET`

Important:

- `LISTEN` timeout is a hard protocol boundary
- once timeout has fired and the state machine enters `PREPARE_PACKET`, the
  local side owns the turn
- there is no late-`ADDRESS` recovery path back to `IN_RX`

This splits `LISTEN` behavior cleanly:

- in `IN_SERVICE`, timeout means no peer `ADDRESS` was received in time after
  our TX
- in `NO_SERVICE`, timeout means the randomized probe deadline expired

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

RX is logically active only during `LISTEN`. During `PREPARE_PACKET` and
`IN_TX`, the protocol does not listen for a peer turn claim. This is simple
because the protocol is strict ping-pong — each peer transmits exactly one
packet per turn. There is no reason to listen during our own turn.

On the `LISTEN` timeout path specifically:

- the timeout itself decides turn ownership
- even if the radio hardware is still physically in RX until the later TX
  commit path disables it, that does not create a `PREPARE_PACKET -> IN_RX`
  transition
- any such late `ADDRESS` is ignored by core link logic

Re-arm policy:

- `radio_hw` auto-re-arms RX immediately in the TX_END ISR, before queuing
  the `TX_END` event. This eliminates any gap between TX completion and RX
  readiness — the peer's reply is never missed due to link thread scheduling
  latency.
- `radio_hw` does **not** re-arm RX after RX completion (PHYEND). The radio
  stays idle until the next TX_END.
- on startup / enable (entering `LISTEN` from `NO_SERVICE`): `link.c`
  explicitly arms RX once.

If the peer replies fast enough that ADDRESS and RX_OK/RX_BAD are queued
before the link thread processes TX_END, the event ordering is still correct:
TX_END → LISTEN, ADDRESS → IN_RX, RX_OK → PREPARE_PACKET.

## Hardware Responsibilities

`radio_hw` should own:

- RX `ADDRESS` timestamp capture
- RX `PHYEND` timestamp capture
- TX `PHYEND` timestamp capture
- immediate TX launch (DISABLE → TXEN sequence)
- ordered delivery of radio events

`radio_hw` should expose:

- `dequeue_radio_event(...)`
- `try_start_tx(...)` or `try_arm_tx(...)`

`radio_hw` should not own:

- service/no-service policy
- packet sequencing policy
- retransmission policy

## Link Responsibilities

`link.c` should own:

- the four-state machine
- packet preparation
- service/no-service transitions
- sequence/loss accounting
- app TX/RX queues

## Diagnostics To Keep

Keep existing useful counters:

- `lost`
- `crc_err`
- `outages`
- `in_service_ms`
- `txi`
- `txm`
- loss-burst histogram

Possible new counters:

- `listen_timeouts`
- `rx_bad_turn_anchors`

## Invariants

1. Both devices use the same steady-state protocol logic.
2. `PHYEND` is the only authoritative turn boundary.
3. `ADDRESS` is the only early RX-start-like gate used in core logic.
4. `FRAMESTART` is not used.
5. The link-layer state machine has only four internal states:
   `IN_RX`, `PREPARE_PACKET`, `IN_TX`, `LISTEN`.
6. RX is only armed during `LISTEN`. `PREPARE_PACKET` and `IN_TX` do not
   listen.
7. Missed peer replies in `LISTEN` + `IN_SERVICE` are modeled using
   `MAX_PACKET_AIRTIME_US`.
8. `LISTEN` timeout is a hard boundary; there is no `PREPARE_PACKET -> IN_RX`
   recovery path for a late peer packet.
9. All tick comparisons are modulo-32-bit wrap-safe; raw relational
   comparisons on live ticks are forbidden.
10. Simultaneous reclaim collisions are allowed in the first iteration;
    recovery may fall back through `NO_SERVICE` randomized reacquisition.
11. Disable overrides protocol state; after disable, pre-disable radio events
    must not re-enter the state machine.

## First Iteration Success Criteria

The first implementation is successful if:

- there is no per-role slot timing logic in `link.c`
- both boards run the same internal state machine
- timing is driven by packet ends, not fixed frame intervals
- variable payload sizes work without timing-policy changes
- the code is easier to understand than the current fixed-slot scheduler
