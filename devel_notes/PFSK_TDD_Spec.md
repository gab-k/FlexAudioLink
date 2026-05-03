# PFSK Time Division Duplex Spec

Implementation reference: `firmware/src/prop_fsk/`.

- `radio_core.c`:

	authoritative for radio timing and DMA ownership.
	Its entirely Timer/[DPPI](https://docs.nordicsemi.com/bundle/ps_nrf54LM20A/page/dppi.html) and ISR based.

- `session.c`:

	is authoritative for service state, app queues, payload sequencing, and loss accounting.

## Protocol

- Both peers run identical half-duplex ping-pong logic.
- No role-specific slot timing exists.
- Device waits for RX to be done then sends itself, then listens again.
- A TX turn always sends either one queued payload packet or one keepalive.
- Listen timeouts include random jitter; the first peer to time out transmits, breaking deadlock.

## Relevant RADIO Peripheral Events

Reference: [nRF RADIO peripheral docs](https://docs.nordicsemi.com/bundle/ps_nrf54LM20A/page/radio.html)

| Event | Meaning |
|---|---|
| `ADDRESS` | Address field sent or matched |
| `PHYEND` | Full packet (including CRC) transmitted or received |
| `CRCOK` | Received packet CRC matched |
| `CRCERROR` | Received packet CRC failed |

Handled inside `radio_isr`.

## Radio Turn State

Active protocol states:

| State | Meaning |
|---|---|
| `LISTEN` | Radio waits for peer `ADDRESS` or listen deadline. |
| `IN_RX` | Peer packet has reached `ADDRESS`; RX deadline is armed. |
| `IN_TX` | Local TX has been committed; late peer `ADDRESS` is ignored. |

Implementation sentinel:

| State | Meaning |
|---|---|
| `DISABLED` | Radio path stopped; not a protocol turn. |


## Session Events

`radio_core` emits one ordered event stream to `session.c`.

```c
struct pfsk_session_event {
	enum pfsk_session_event_type type;
	uint32_t tick;
	int16_t rssi_dbm;
};
```

| Event | Tick | Notes |
|---|---|---|
| `RX_OK` | RX `PHYEND` | RSSI valid. |
| `RX_BAD` | RX `PHYEND` | RSSI valid. |
| `TX_END` | TX `PHYEND` | TX already ended. |
| `RX_INCOMPLETE` | deadline | RX packet did not finish in time. |
| `LISTEN_TIMEOUT` | deadline | No peer `ADDRESS` before deadline. |
| `TX_TRIGGER_FAILED` | deadline | Timeout TX could not be triggered. |

## Packet Format

Packet struct:

```c
struct pfsk_packet {
	uint8_t  length;
	uint16_t seq;
	uint8_t  payload[PFSK_PAYLOAD_MAX_LEN];
} __packed __aligned(4);
```

- packets never use `seq = PFSK_KEEPALIVE_SEQ = UINT16_MAX` unless they are keepalive packets.
- zero-length app TX frames are invalid

Constants:
- `PFSK_PACKET_METADATA_LEN = 2U`
- `PFSK_PAYLOAD_MAX_LEN = 252`
- `PFSK_KEEPALIVE_PAYLOAD_LEN = 16U`
- `PFSK_KEEPALIVE_SEQ = UINT16_MAX`

Note: Do not confuse `payload` inside `struct pfsk_packet` with `PAYLOAD` field inside [nRF RADIO Peripheral docs -> Packet configuration](https://docs.nordicsemi.com/bundle/ps_nrf54LM20A/page/radio.html#ariaid-title2)

## Buffers

`radio_core.c` owns the packet buffers. `PACKETPTR` always points to the
current read/write slot in these rings. The RADIO peripheral DMA transfers
directly to/from the pointed-to ring entry.

```c
tx_ring[PFSK_RADIO_TX_RING_DEPTH]
rx_ring[PFSK_RADIO_RX_RING_DEPTH]
keepalive_packet
```

Ring ownership:

| Ring index | Writer |
|---|---|
| TX write | session thread |
| TX read | radio ISR |
| RX write | radio ISR |
| RX read | session thread |

Rings are Single Producer Single Consumer (SPSC). One slot remains unused to distinguish full from empty.

TX ring rules:

- TX ring contains payload packets only.
- Keepalives are never queued in the TX ring.
- Session gets a pointer to the next TX slot via `pfsk_radio_tx_get_wr_ptr()`,
  copies into it, then commits with `pfsk_radio_tx_advance_wr_idx()`.
- Session-thread does not touch `PACKETPTR`.

RX ring rules:

- RADIO DMA writes to `rx_ring[rx_wr_idx]`.
- `CRCOK`/`CRCERROR` queues a session event.
- RX write index advances only if the RX ring has room and event queueing succeeds.
- Session releases a consumed RX slot with `pfsk_radio_rx_advance_rd_idx()`.

## RADIO Shortcuts

Reference: [nRF RADIO peripheral docs -> SHORTS register](https://docs.nordicsemi.com/bundle/ps_nrf54LM20A/page/radio.html#ariaid-title112)

Base shorts (always active):

- `READY_START` - READY -> START: begin TX/RX as soon as ramp-up completes.
- `PHYEND_DISABLE` - PHYEND -> DISABLE: shut down radio after every packet.
- `ADDRESS_RSSISTART` - ADDRESS -> RSSISTART: sample signal strength on peer packet arrival.

Turn-selection shorts (one active at a time):

- `DISABLED_RXEN` - DISABLED -> RXEN: after disable, ramp back into RX.
- `DISABLED_TXEN` - DISABLED -> TXEN: after disable, ramp into TX.

`PHYEND_DISABLE` + a turn-selection short chains the whole turnaround in hardware:
packet ends -> radio disables -> radio re-enables in the next direction, no ISR needed.

The active shortcut set prepares the *next* direction:

- while receiving: `BASE | DISABLED_TXEN` (turnaround into TX)
- while transmitting: `BASE | DISABLED_RXEN` (turnaround into RX)

## PACKETPTR and Shortcut Programming

PACKETPTR and shortcuts are always set together. Programming an RX pointer selects `DISABLED_TXEN`, programming a TX pointer selects `DISABLED_RXEN`.

RX programming:

```text
PACKETPTR = &rx_ring[rx_wr_idx]
```

TX programming:

```text
PACKETPTR = &tx_ring[tx_rd_idx]   if TX ring non-empty
PACKETPTR = &keepalive_packet     if TX ring empty
```

Programming points:

| Point | Action |
|---|---|
| `pfsk_radio_start()` | Program initial RX pointer. |
| `ADDRESS` while `LISTEN` | Program next TX pointer. |
| `LISTEN`/`IN_RX` timeout | Program TX pointer directly before switchover to TX. |
| `ADDRESS` while `IN_TX` | Program next RX pointer. |

## Deadlines

Timer:

- TIMER10 runs at 1 MHz.
- CC[2] captures `ADDRESS`.
- CC[3] is the semantic deadline compare.
- CC[4] captures `PHYEND`.

Constants:
- `PFSK_RADIO_LISTEN_TIMEOUT_BASE_US` — fixed listen window before timeout.
- `PFSK_RADIO_LISTEN_TIMEOUT_JITTER_US` — max random offset added to listen timeout. Determined by xorshift PRNG seeded from device ID and cycle counter.
- `PFSK_RADIO_MAX_PACKET_AIRTIME_US` — worst-case packet duration on air.
- `PFSK_RADIO_DEADLINE_MIN_LEAD_US` — minimum time a deadline must be in the future.

Deadline rules:

| Entry | Deadline |
|---|---|
| start -> `LISTEN` | `now + BASE_US + jitter` |
| `ADDRESS` -> `IN_RX` | `address_tick + MAX_PACKET_AIRTIME_US` |
| TX `PHYEND` -> `LISTEN` | `tx_phyend_tick + MAX_PACKET_AIRTIME_US + jitter` |

If a deadline is too close, `radio_core` clamps it forward by
`DEADLINE_MIN_LEAD_US` and increments `deadline_late_count`.

TX ring publication never changes deadlines.

## State Transitions

### `LISTEN -> IN_RX`

Trigger: RADIO `ADDRESS`.

Actions:

- set `turn_state = IN_RX`
- arm RX deadline
- program TX `PACKETPTR` to TX ring head or keepalive

### `LISTEN -> IN_TX`

Trigger: listen timeout deadline.

Actions:

- queue `LISTEN_TIMEOUT`
- program TX `PACKETPTR` to TX ring head or keepalive
- set shorts to `DISABLED_TXEN`
- clear pending `ADDRESS`, RX ADDRESS could arrive between timeout and `DISABLE`
- set `turn_state = IN_TX`
- trigger RADIO `DISABLE`
- if trigger fails, queue `TX_TRIGGER_FAILED`

### `IN_RX -> IN_TX`

Triggers:

- RX `PHYEND`, after `CRCOK`/`CRCERROR`
- RX timeout deadline

RX `PHYEND` actions:

- `CRCOK` queues `RX_OK`; `CRCERROR` queues `RX_BAD`
- RX write index advances if event queueing succeeds
- `PHYEND` handler sets `turn_state = IN_TX`
- hardware short starts TX

RX timeout deadline actions:

- queue `RX_INCOMPLETE`
- program TX `PACKETPTR` to TX ring head or keepalive
- set shorts to `DISABLED_TXEN`
- clear pending `ADDRESS`, RX ADDRESS could arrive between timeout and `DISABLE`
- set `turn_state = IN_TX`
- trigger RADIO `DISABLE`
- if trigger fails, queue `TX_TRIGGER_FAILED`

### `IN_TX -> LISTEN`

Trigger: TX `PHYEND`.

Actions:

- advance TX read index only if the transmitted packet came from TX ring buffer
- set `turn_state = LISTEN`
- arm post-TX listen deadline
- increment TX stats
- queue `TX_END`

`TX_END` is queued from `PHYEND`, not from `DISABLED`. The next RX pointer was
already programmed by TX `ADDRESS`.

## Session Handling

`RX_OK`:

- read RX packet from RX ring
- reject malformed length and advance RX read index (early return)
- reset `consecutive_rx_misses`
- enter `IN_SERVICE`
- if keepalive marker: no app frame, no seq accounting
- if payload: queue app RX frame and update seq/loss accounting
- advance RX read index

`RX_BAD`:

- reset `consecutive_rx_misses`
- advance RX ring buffer read index
- do not enter service from `NO_SERVICE`

`RX_INCOMPLETE`:

- increment `rx_incomplete_count`
- reset `consecutive_rx_misses`

`LISTEN_TIMEOUT`:

- if `IN_SERVICE`, increment `consecutive_rx_misses`
- if threshold reached, mark `NO_SERVICE`
- if already `NO_SERVICE`, log rate-limited waiting warning

`TX_END`:

- refill TX ring with queued payloads until full or app TX queue empty

`TX_TRIGGER_FAILED`:

- increment `tx_trigger_fail_count`
- mark `NO_SERVICE`
- log error
- stop radio path

## Service State

States:

- `NO_SERVICE`
- `IN_SERVICE`

Enter service:

- valid `RX_OK` packet, including keepalive

Stay in service:

- `RX_OK`, `RX_BAD`, and `RX_INCOMPLETE` all reset misses
- `RX_BAD` and `RX_INCOMPLETE` reset misses but do not enter service on their own

Leave service:

- only consecutive `LISTEN_TIMEOUT` events count as missed peer activity
- threshold is `PFSK_SESSION_SYNC_LOSS_TURNS`

On service loss:

- increment outage counter if previously in service
- reset miss counter
- clear in-service timestamp
- clear RX sequence continuity
- do not reset radio turn state

## Responsibilities

`radio_core` owns:

- RADIO/TIMER configuration
- turn state
- deadlines and jitter
- `PACKETPTR` and shorts
- RX/TX rings
- keepalive fallback packet
- session event queue
- hardware stats

`session.c` owns:

- service state
- app TX/RX queues
- payload copy into TX ring
- payload sequence/loss accounting
- user-visible session stats/logs

## Invariants

1. Both roles use identical timing logic.
2. `PHYEND` anchors packet end.
3. `ADDRESS` is the only peer packet start gate.
4. `FRAMESTART` and `SYNC` are not protocol inputs.
5. `session.c` does not own radio turn state.
6. Session events do not carry packet payloads.
7. TX ring entries are payload-only.
8. Empty TX ring means transmit `keepalive_packet`.
9. Keepalive does not consume payload sequence numbers.
10. Payload sequence generation skips `PFSK_KEEPALIVE_SEQ`.
11. Session-thread TX publication does not program `PACKETPTR`.
12. `PACKETPTR` and turn-selection shortcut are always programmed together.
13. `ADDRESS` must program the next-direction `PACKETPTR` before `PHYEND` fires.
14. Only `RX_OK` can enter service; `RX_BAD` and `RX_INCOMPLETE` cannot.
15. Listen timeout is a hard turn boundary.
16. Stop purges stale session events and leaves radio state disabled.
