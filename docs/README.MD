# Design

What I built and why it is built this way. For how to build and run it, see the [README](README.MD).

The core is a low-latency, single-threaded limit order book and matching engine that doesn't rely on an external matching library. One of the main constraints is that the hot path should not perform dynamic allocations. Around that core there is a TCP gateway, a journal with crash recovery, a trader terminal and a liquidity bot.

## Contents

- [Architecture](#architecture)
- [Supported Order Types](#supported-order-types)
- [How add_order() Works](#how-add_order-works)
- [Modifying Orders](#modifying-orders)
- [Event Model](#event-model)
- [Journal and Recovery](#journal-and-recovery)
- [Wire Protocol](#wire-protocol)
- [Core Primitives](#core-primitives)
- [Project Layout](#project-layout)
- [Design Decisions](#design-decisions)
- [Reference](#reference)

## Architecture

### The process

```
 clients <--TCP--> [ gateway thread ] --inbound--> [ matching thread ] --journal--> [ journal thread ] --> disk
                          ^                               |
                          +-----------outbound------------+
```

The three threads are connected by the lock-free `SPSCQueue`.

- **Gateway thread.** Owns every socket (`poll`-based, non-blocking). It decodes requests, assigns order ids, checks that a cancel or modify comes from the order's owner, and turns engine events into the messages each client should see.
- **Matching thread.** The only thread that touches the order books. It pops one command, runs it through `MatchingEngine`, and publishes the resulting events plus a top-of-book update if the best bid or offer changed.
- **Journal thread.** Appends commands and events to disk, so the matching thread never waits on I/O.

Back-pressure is explicit. If the journal can't keep up, the matching thread waits. If the matching thread can't keep up, the gateway stops reading from sockets. A client that stops reading its own replies is disconnected once 8 MB is queued for it.

### The matching core

```
┌─────────────────────────────────────────────────────────┐
│                      MatchingEngine                      │
│   symbol -> OrderBook   (std::pmr::map<Symbol, ...>)     │
└───────────────────────────┬─────────────────────────────┘
                            │ InboundOrder
                            │ ADD / CANCEL / MODIFY
                            ▼
┌─────────────────────────────────────────────────────────┐
│                        OrderBook                         │
│                                                         │
│   bids_ (price desc)        asks_ (price asc)            │
│   ┌───────────────┐         ┌───────────────┐           │
│   │ Level (price) │  ...    │ Level (price) │  ...     │
│   │ head <-> tail │         │ head <-> tail │           │
│   │  FIFO orders  │         │  FIFO orders  │           │
│   └───────────────┘         └───────────────┘           │
│                                                         │
│   orders_ : OrderId -> Order*            (O(1) lookup)  │
│   stop_buys_/stop_sells_ : trigger_price -> Order*     │
│                                                         │
│   order_pool_ : MemoryPool<Order, N>     (fixed size)   │
│   level_pool_ : MemoryPool<Level, N>     (fixed size)   │
│                                                         │
│   -> emits Event[] (Trade, Accepted, Rested, Filled...) │
└─────────────────────────────────────────────────────────┘
```

### One order book per symbol

`MatchingEngine` owns one `OrderBook` per `Symbol`. A book is created lazily when the first order for that symbol arrives, or ahead of time with `prepare_symbols`. Lazy creation can be switched off, which is what the server does: it only accepts the symbols it was started with.

Each book is self-contained. It owns its own price levels, order index, and memory pools, so activity in one symbol doesn't affect another. Event sequence numbers are the one thing they share: the engine hands every book the same counter, so the event stream has one global, gapless sequence. This also gives me a clean path toward sharding books across cores later if I decide to go there.

### Price-time priority

`bids_` and `asks_` are `std::pmr::map<Price, Level*>` containers. Bids are ordered with `greater` and asks with `less`, which means the best price is always at `begin()`. A level is erased from the map as soon as its last order leaves, so `begin()` never has to skip over dead entries.

Each `Level` contains an intrusive doubly-linked list of `Order*`. Orders at the same price are therefore matched in FIFO order.

### Why intrusive lists?

Instead of using `std::list`, the `Order` itself stores the pointers it needs:

- `prev`
- `next`
- `parent_level`

Adding or removing an order from a price level is then just pointer manipulation. There is no separate list node allocation, and I don't have to worry about list iterators being invalidated by unrelated operations.

### Fixed-capacity memory pools

One of the main design goals is keeping allocations off the hot path.

I don't call `new` for every `Order` or `Level`. Both come from a fixed-capacity `MemoryPool<T, N>`. The pool uses a free list over one contiguous, cache-line-aligned (`alignas(64)`) backing array whose size is decided at compile time.

`allocate()` and `deallocate()` are O(1) pointer operations.

The trade-off is intentional: I would rather decide the memory footprint up front than have the general-purpose allocator unexpectedly run during order matching.

### `std::pmr` for the index structures

The book's associative containers (`bids_`, `asks_`, `stop_buys_`, `stop_sells_`, and `orders_`) use `std::pmr` and are each backed by a `FixedArenaResource`.

The idea is simple: allocate one arena up front and let the PMR containers allocate their nodes from it. That keeps the implementation simpler than writing a separate custom pool for every map type while still giving me predictable allocation behavior.

The arena keeps a free list per node size, so a node freed by `erase` is reused by the next `insert`. This matters. My first version used `std::pmr::monotonic_buffer_resource`, which never reuses freed memory, so a book that churned through enough orders would eventually run its buffer dry no matter how few orders were open at once.

`tests/test_no_alloc.cpp` replaces global `operator new` with a counting version and checks that it is never called while 400,000 mixed orders run through the engine.

### Events instead of callbacks

Every mutating operation (`add_order`, `cancel_order`, and `modify_order`) returns a `std::span<const Event>`.

The span points to a reusable buffer containing everything that happened because of that one inbound order:

- accepted
- rested
- trades
- fills
- rejects
- cancels

The buffer is reset and reused for the next call, so I'm not allocating an object for every event.

This also makes the matching engine easier to test because I can assert against the exact event sequence. More importantly, it is the boundary everything else is built on: the journal, the gateway and the market data feed all consume this stream, and the matching engine knows nothing about any of them.

### No exceptions

Everything is compiled with `-fno-exceptions`.

Instead of throwing, the engine reports failures through explicit `ReasonCode` values and `std::optional` results. That fits the predictable-latency goal of the project and keeps error handling explicit.

## Supported Order Types

The order types are defined in `core/types.h::OrderType`.

| Type         | Behavior                                                                                                                                                    |
| ------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `LIMIT`      | Standard resting limit order.                                                                                                                               |
| `MARKET`     | Executes immediately against the best available prices. Rejected if the opposite side of the book is empty. Any unfilled remainder is cancelled.            |
| `IOC`        | Matches whatever it can immediately. Any remaining quantity is cancelled instead of resting.                                                                |
| `FOK`        | Rejected unless the entire quantity can be filled immediately. The check is done up front and counts hidden iceberg quantity, since that is matchable too.  |
| `GTC`        | Resting limit order. In this engine, it behaves like `LIMIT` because there is no separate expiry logic yet.                                                 |
| `STOP`       | Waits in `stop_buys_` or `stop_sells_` until the last trade price reaches the trigger price. It then becomes a `MARKET` order and enters matching.          |
| `STOP_LIMIT` | Same trigger mechanism as `STOP`, but becomes a `LIMIT` order at the specified price.                                                                       |
| `ICEBERG`    | Only `display_qty` is visible at a time. When the visible quantity is exhausted, hidden quantity is replenished at the back of the FIFO queue.              |
| `POST_ONLY`  | Rejected if it would immediately cross the book, guaranteeing that it only adds liquidity. A modify that would make it cross is rejected too.               |

## How `add_order()` Works

The basic flow is:

1. **Validate the order.**
`validate_new_order` checks things such as zero quantity, duplicate IDs, missing or invalid prices, invalid iceberg display sizes, `POST_ONLY` crossing, insufficient `FOK` liquidity, and empty opposite-side books for `MARKET` orders. Invalid orders produce an `ORDER_REJECTED` event with the appropriate `ReasonCode`.

2. **Allocate the order.**
The order comes from `order_pool_`. If the pool is full, the engine rejects the order with `ORDER_POOL_EXHAUSTED` rather than falling back to heap allocation.

3. **Handle stop orders.**
`STOP` and `STOP_LIMIT` orders are parked in the stop-order maps until their trigger condition is met.

4. **Match the order.**
The engine walks the opposite side of the book starting from the best price. Orders at each price level are matched FIFO through `match_fifo_at_level`.

    Iceberg replenishment happens during this process. When a passive order runs out of visible quantity but still has hidden quantity, another visible slice is added to the back of the queue.

5. **Deal with the remainder.**
If the order is completely filled, it is released.

    If quantity remains and the order type is allowed to rest (`LIMIT`, `GTC`, `ICEBERG`, `POST_ONLY`, or a triggered `STOP_LIMIT`), the order is inserted into the book and an `ORDER_RESTED` event is emitted.

    Otherwise, such as with `IOC` or a remaining `MARKET` quantity, the remainder is cancelled with reason `UNFILLED_REMAINDER`.

6. **Trigger stop orders.**
Once the inbound order has been fully handled, `drain_triggered_stops` checks whether the new `last_trade_price_` reached any stop prices. Triggered orders are converted and run through matching, one at a time, until there are no more triggers, so cascading stop orders are handled too.

    Stops run *after* the order that tripped them has rested its remainder. That ordering matters: the incoming order arrived first, so a triggered stop should be able to trade against what it left on the book. Stops at the same trigger price fire in arrival order. A stop whose trigger has already been reached when it is submitted fires immediately.

## Modifying Orders

`modify_order` follows a simple rule based on how real exchanges generally treat priority changes.

- If the price changes, the order is cancelled and reinserted. It loses its original time priority.
- If the price stays the same and the quantity is only reduced, the order is modified in place and keeps its queue position. For an iceberg the reduction comes out of the hidden reserve first.
- Increasing quantity forfeits the existing priority and sends the order to the back of the queue.

The quantity in a modify is the new total open quantity. A modify that changes nothing is rejected, and parked stop orders can't be modified (cancel and resubmit instead).

I wanted this behavior to be explicit rather than hiding the priority rules behind a bunch of special cases.

## Event Model

`matching/events.h` defines a tagged union:

```
Event {
    EventType type;
    union Payload;
}
```

It currently covers:

- `ORDER_ACCEPTED`
- `ORDER_RESTED`
- `ORDER_REJECTED`
- `ORDER_REDUCED`
- `ORDER_PARTIALLY_FILLED`
- `ORDER_FILLED`
- `ORDER_CANCELLED`
- `TRADE`

All payload structures are `trivially_copyable`, enforced with `static_assert`, and each begins with a sequence number and timestamp. That is what lets the same struct go through an SPSC queue and into the journal without a serialization step.

A few details worth knowing if you consume the stream:

- `ORDER_CANCELLED` carries a reason: `NONE` for a requested cancel, `UNFILLED_REMAINDER` for IOC/market leftovers, `REPLACED` for the cancel half of a cancel-and-reinsert modify (an `ORDER_ACCEPTED` for the same id follows).
- `TRADE` is the source of truth for fills, one per match, with both order ids and the aggressor side. The resting order also gets an `ORDER_FILLED`/`ORDER_PARTIALLY_FILLED` per trade; the incoming order gets one summary fill event per sweep.
- The events are complete: `tests/test_fuzz.cpp` rebuilds every open order's quantity from events alone and checks it against the book after each step.

## Journal and Recovery

The journal (`journal/journal.h`) is an append-only file of fixed-size records, each followed by a CRC-32. The server writes two kinds of record:

- **Commands**: every order, cancel and modify, exactly as it was handed to the engine, including the order id and timestamp.
- **Events**: everything the engine produced in response.

Because the engine is deterministic, the commands alone are enough to rebuild all state. On start-up the server replays them through a fresh engine, which restores every book, open order, order owner and the last trade prices. While replaying it compares the regenerated events with the journaled ones. If they differ (for example because the journal was written with a different symbol list or different capacities), it refuses to start rather than quietly coming up with different state.

A torn or corrupt record at the tail, as left by a crash mid-write, is detected by its checksum, dropped, and overwritten by the next append.

Limits I'm aware of:

- Writes are asynchronous. Data reaches the OS as soon as the journal thread is idle and is `fdatasync`ed at least every `--sync-ms` (default 50 ms). A process crash loses nothing that reached the OS; a power failure can lose up to that window, including orders that were already acknowledged.
- There are no snapshots, so start-up replays the whole journal and the file grows forever.
- Records are raw memory, so a journal is tied to the build that wrote it.

`journal_dump` prints a journal in readable form (see the README).

## Wire Protocol

Defined in `protocol/messages.h`. Every message is a 4-byte header (`uint16 length`, `uint8 type`, `uint8 reserved`) followed by a fixed-size little-endian struct. The structs have explicit padding and are checked at compile time to contain no padding bytes, so nothing uninitialised ever goes on the wire.

| Client → exchange     | Purpose                                                        |
| --------------------- | -------------------------------------------------------------- |
| `LOGIN`               | Must come first. Carries the participant id.                   |
| `NEW_ORDER`           | Any order type. Carries a client order id that is echoed back. |
| `CANCEL_ORDER`        | By exchange order id.                                          |
| `MODIFY_ORDER`        | New price and new open quantity.                               |
| `MASS_CANCEL`         | Cancel all of the participant's orders.                        |
| `OPEN_ORDERS_REQUEST` | List the participant's open orders.                            |
| `SUBSCRIBE` / `UNSUBSCRIBE` | Trades and top-of-book for a symbol.                     |
| `DEPTH_REQUEST`       | Ten best levels each side.                                     |

| Exchange → client  | Purpose                                                                 |
| ------------------ | ----------------------------------------------------------------------- |
| `LOGIN_ACK`        | Status and the list of tradable symbols.                                |
| `EXECUTION_REPORT` | One per thing that happens to one of your orders (accepted, rested, fill, cancelled, rejected...). |
| `TRADE_TICK`       | Public trade print. No order or participant ids.                        |
| `TOP_OF_BOOK`      | Best bid and offer, sent when it changes.                               |
| `DEPTH_SNAPSHOT`   | Reply to `DEPTH_REQUEST`, and sent once on `SUBSCRIBE`.                 |
| `OPEN_ORDER` / `OPEN_ORDERS_END` | Reply to `OPEN_ORDERS_REQUEST`.                           |

Orders belong to a participant, not a connection. They stay on the book when the client disconnects or the exchange restarts, and the same participant can reconnect and manage them. A participant can have one connection at a time.

There is no authentication: anyone who can reach the port can log in as any participant, which is why the server binds to `127.0.0.1` by default.

## Core Primitives

The main building blocks live under `src/core/`:

- **`types.h`** — Strong-ish aliases such as `Price`, `Quantity`, `OrderId`, and `Timestamp`. It also contains the fixed-size `Symbol` type and the `PRICE_SCALE` used for fixed-point pricing.
- **`price.h`** — Parsing and formatting of fixed-point prices (`"150.25"` ↔ `1502500`).
- **`memory_pool.h`** — Fixed-capacity, free-list `MemoryPool<T, N>` with cache-line-aligned slots.
- **`arena_resource.h`** — `FixedArenaResource`, the recycling `std::pmr` resource behind the index containers.
- **`spsc_queue.h`** — Lock-free single-producer/single-consumer ring buffer with power-of-two capacity and cache-line-padded head/tail cursors. It carries commands, events and journal records between the server's threads.
- **`clock.h`** — A monotonic `LogicalClock` for deterministic sequencing plus nanosecond timestamp helpers.

## Project Layout

```
src/
├── core/                 types, price, memory_pool, arena_resource, spsc_queue, clock
├── matching/             order, level, events, order_book, matching_engine
├── journal/              crc32, journal (Writer / Reader)
├── protocol/             messages, codec (framing), text (enum names)
├── net/                  socket helpers, Client
├── exchange/             config, records, order_tracker, exchange (the server)
└── stock_exchange_main.cpp

tools/
├── trader_terminal.cpp
├── liquidity_bot.cpp
├── bench_engine.cpp
└── journal_dump.cpp

tests/                    one executable per file, no external test framework
scripts/demo.sh
CMakeLists.txt
README.MD                 how to build, run and what it measures at
DESIGN.md                 this document
```

## Design Decisions

### Fixed capacities instead of dynamic growth

`OrderBook` and `MatchingEngine` take `OrderCapacity`, `LevelCapacity`, `EventCapacity`, and `StopCapacity` as non-type template parameters.

The library defaults are:

- 1M orders
- 10K levels
- 262K events
- 64K stops

The server uses smaller ones (65,536 orders, 8,192 levels and stops, 262,144 events per symbol, about 40 MB per symbol), set in `exchange/config.h` and overridable from CMake.

This makes the memory footprint a build-time decision. The downside is less flexibility, but I think that trade-off makes sense for a low-latency engine where I want memory usage to be predictable.

### FIFO instead of pro-rata

I'm using FIFO matching for now because it keeps the price-level structure simple: an intrusive doubly-linked list is enough.

Pro-rata or size-priority matching would need a different structure, probably something sorted by size or another specialized data structure. That's something I may experiment with later.

### Running out of capacity is an answer, not a crash

Originally the engine called `std::abort()` when the event buffer overflowed or a level couldn't be allocated, on the theory that both were configuration errors. That stopped being acceptable once clients could reach the engine over a socket: anyone could take the exchange down by resting orders at enough distinct prices, or by sweeping a large iceberg.

Every limit now has a defined outcome:

| Limit hit                         | What happens                                                                           |
| --------------------------------- | -------------------------------------------------------------------------------------- |
| Order pool full                   | New order rejected, `ORDER_POOL_EXHAUSTED`                                             |
| Stop capacity full                | New stop rejected, `STOP_CAPACITY_EXHAUSTED`                                           |
| No level available to rest on     | Remainder cancelled, `LEVEL_POOL_EXHAUSTED` (fills that already happened stand)        |
| Event buffer nearly full mid-sweep| Matching stops, remainder cancelled, `EVENT_BUFFER_EXHAUSTED`                          |

The last one means a single order can produce at most about `EventCapacity` events. In that extreme case even a `FOK` can end up partially filled; size `EventCapacity` so it doesn't come up.

The index arenas still abort if they run dry, but they are sized from the same capacities, so the checks above are hit first.

### Finished orders are forgotten

When an order is filled or cancelled, the book drops it completely. That keeps memory bounded by the number of *open* orders, but it means a cancel for an order that finished long ago and a cancel for an id that never existed both get `ORDER_NOT_FOUND`, and an order id can be reused once its order is gone. The server never reuses ids: the gateway hands them out from a counter that survives restarts.

## Reference

<https://www.systemdesignhandbook.com/guides/design-a-stock-exchange-system/>