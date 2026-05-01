# nRF GFSK Ping-Pong TDD Spec

Implementation reference: `nrf_firmware/src/prop_gfsk/`.

`radio_hw.c` is authoritative for radio timing and DMA ownership. `link.c`
is authoritative for service state, app queues, payload sequencing, and loss
accounting.

## Protocol

- Both peers run identical half-duplex ping-pong logic.
- `ADDRESS` is the only early RX-start gate.
- `PHYEND` is the packet-end timing anchor.
- `FRAMESTART` and `SYNC` are not used for core state transitions.
- No role-specific slot timing exists.
- A TX turn always sends either one queued payload packet or one keepalive.

## Hardware Turn State

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

`link.c` must not duplicate this state machine.

## Hardware Events

`radio_hw` emits one ordered metadata-only event stream to `link.c`.

```c
struct pgfsk_hw_event {
	enum pgfsk_hw_event_type type;
	uint32_t tick;
	int16_t rssi_dbm;
};
```

Events never carry packets. RX packets are read from the RX ring.

| Event | Tick | Notes |
|---|---|---|
| `RX_OK` | RX `PHYEND` | RSSI valid. |
| `RX_BAD` | RX `PHYEND` | RSSI valid. |
| `TX_END` | TX `PHYEND` | TX already ended. |
| `RX_INCOMPLETE` | deadline | RX packet did not finish in time. |
| `LISTEN_TIMEOUT` | deadline | No peer `ADDRESS` before deadline. |
| `TX_TRIGGER_FAILED` | deadline | Timeout TX could not be triggered. |

## Packet Format

RADIO config:

- `lflen = 8`
- `s0len = 0`
- `s1len = 0`

DMA buffer:

```text
offset 0:    LENGTH byte
offset 1..N: PAYLOAD bytes, N = LENGTH
```

Packet struct:

```c
struct pgfsk_packet {
	uint8_t  length;
	uint16_t seq;
	uint8_t  data[PGFSK_PAYLOAD_MAX_LEN];
} __packed __aligned(4);
```

Constants:

- `PGFSK_PACKET_METADATA_LEN = 2U`
- `PGFSK_PAYLOAD_MAX_LEN = 252`
- max transmitted `length = 254`
- keepalive `length = PGFSK_PACKET_METADATA_LEN`
- zero-length app TX frames are invalid

## Buffers

`radio_hw.c` owns the packet buffers:

```c
g_tx_ring[PGFSK_HW_TX_RING_DEPTH]
g_rx_ring[PGFSK_HW_RX_RING_DEPTH]
g_keepalive_packet
```

Ring ownership:

| Ring index | Writer |
|---|---|
| TX write | link thread |
| TX read | radio ISR |
| RX write | radio ISR |
| RX read | link thread |

Rings are SPSC. One slot remains unused to distinguish full from empty.

TX ring rules:

- TX ring contains payload packets only.
- Keepalives are never queued in the TX ring.
- Link publishes a payload by filling `pgfsk_hw_tx_get_wr_ptr()` and then
  calling `pgfsk_hw_tx_advance_wr_idx()`.
- Link-thread publication does not touch `PACKETPTR`.

RX ring rules:

- RADIO DMA writes to `g_rx_ring[g_rx_wr_idx]`.
- `CRCOK`/`CRCERROR` queues a metadata-only event.
- RX write index advances only if the RX ring has room and event queueing
  succeeds.
- Link releases a consumed RX slot with `pgfsk_hw_rx_advance_rd_idx()`.

## RADIO Shortcuts

Base shorts:

- `READY_START`
- `PHYEND_DISABLE`
- `ADDRESS_RSSISTART`

Turn-selection shorts:

- `DISABLED_RXEN`
- `DISABLED_TXEN`

The active shortcut set is:

- RX path: `BASE | DISABLED_RXEN`
- TX path: `BASE | DISABLED_TXEN`

## PACKETPTR Rules

RX programming:

```text
PACKETPTR = &g_rx_ring[g_rx_wr_idx]
```

TX programming:

```text
PACKETPTR = &g_tx_ring[g_tx_rd_idx] if TX ring non-empty
PACKETPTR = &g_keepalive_packet     if TX ring empty
```

Programming points:

| Point | Action |
|---|---|
| `pgfsk_hw_start()` | Program initial RX pointer. |
| `ADDRESS` while `LISTEN` | Program next TX pointer. |
| `LISTEN`/`IN_RX` timeout | Program TX pointer directly before switchover to TX. |
| `ADDRESS` while `IN_TX` | Program next RX pointer. |

## Deadlines

Timer:

- TIMER10 runs at 1 MHz.
- CC[2] captures `ADDRESS`.
- CC[3] is the semantic deadline compare.
- CC[4] captures `PHYEND`.

Deadline rules:

| Entry | Deadline |
|---|---|
| start -> `LISTEN` | `now + BASE_US + jitter` |
| `ADDRESS` -> `IN_RX` | `address_tick + MAX_PACKET_AIRTIME_US` |
| TX `PHYEND` -> `LISTEN` | `tx_phyend_tick + MAX_PACKET_AIRTIME_US + jitter` |

`radio_hw` clamps too-late deadlines forward by
`PGFSK_HW_DEADLINE_MIN_LEAD_US` and increments `deadline_late_count`.

TX ring publication never changes deadlines.

## State Transitions

### `LISTEN -> IN_RX`

Trigger: RADIO `ADDRESS`.

Actions:

- set `turn_state = IN_RX`
- arm RX deadline
- program TX `PACKETPTR` to TX ring head or keepalive
- set shorts to `BASE | DISABLED_TXEN`

### `LISTEN -> IN_TX`

Trigger: listen timeout deadline.

Actions:

- queue `LISTEN_TIMEOUT`
- program TX `PACKETPTR` to TX ring head or keepalive
- clear pending `ADDRESS`
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
- program TX pointer
- set `turn_state = IN_TX`
- trigger RADIO `DISABLE`

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

## Link Handling

`RX_OK`:

- read RX packet from RX ring
- reject malformed length and release slot
- reset `consecutive_rx_misses`
- enter `IN_SERVICE`
- if metadata-only: keepalive, no app frame, no seq accounting
- if payload: queue app RX frame and update seq/loss accounting
- release RX slot

`RX_BAD`:

- reset `consecutive_rx_misses`
- release RX slot
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

External states:

- `NO_SERVICE`
- `IN_SERVICE`

Enter service:

- valid `RX_OK` packet, including keepalive

Stay in service:

- `RX_OK`, `RX_BAD`, and `RX_INCOMPLETE` all reset misses

Leave service:

- only consecutive `LISTEN_TIMEOUT` events count as missed peer activity
- threshold is `PGFSK_LINK_SYNC_LOSS_TURNS`

On service loss:

- increment outage counter if previously in service
- reset miss counter
- clear in-service timestamp
- clear RX sequence continuity
- do not reset hardware turn state

## Responsibilities

`radio_hw` owns:

- RADIO/TIMER configuration
- turn state
- deadlines and jitter
- `PACKETPTR` and shorts
- RX/TX rings
- keepalive fallback packet
- metadata-only hardware event queue
- hardware stats

`link.c` owns:

- service state
- app TX/RX queues
- payload copy into TX ring
- payload sequence/loss accounting
- keepalive RX policy
- user-visible link stats/logs

## Invariants

1. Both roles use identical timing logic.
2. `PHYEND` anchors packet end.
3. `ADDRESS` is the only peer packet start gate.
4. `FRAMESTART` and `SYNC` are not protocol inputs.
5. `link.c` does not own hardware turn state.
6. RX events do not carry packet payloads.
7. TX ring entries are payload-only.
8. Empty TX ring means transmit `g_keepalive_packet`.
9. Keepalive does not consume payload sequence numbers.
10. Link-thread TX publication does not program `PACKETPTR`.
11. Listen timeout is a hard turn boundary.
12. Stop purges stale hardware events and leaves hardware state disabled.
