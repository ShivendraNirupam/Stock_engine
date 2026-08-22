# Stock Exchange in C++20

I'm building a small stock exchange from scratch in modern C++20.

The goal is to build a low-latency, single-threaded limit order book and matching engine without relying on an external matching library. One of the main constraints is that the hot path should not perform dynamic allocations.


## Current Status

The core matching engine is working. `OrderBook` and `MatchingEngine` currently handle most of the standard order types, and this is the part I've actually implemented and tested.

The remaining entry points are still placeholders:

- `exchange_main.cpp`
- `trader_terminal.cpp`
- `liquidity_bot.cpp`

They currently just print `"Hello World"`. I haven't connected the engine to a real process boundary yet, whether that's a CLI, IPC, or network interface. That's one of the next things I want to tackle.

## Reference
https://www.systemdesignhandbook.com/guides/design-a-stock-exchange-system/


## Architecture

```text
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
│   orders_ : OrderId -> OrderRecord       (O(1) lookup)  │
│   stop_buys_/stop_sells_ : trigger_price -> Order*     │
│                                                         │
│   order_pool_ : MemoryPool<Order, N>     (fixed size)   │
│   level_pool_ : MemoryPool<Level, N>     (fixed size)   │
│                                                         │
│   -> emits Event[] (Trade, Accepted, Rested, Filled...) │
└─────────────────────────────────────────────────────────┘
```

### One order book per symbol

`MatchingEngine` owns one `OrderBook` per `Symbol`. A book is created lazily when the first order for that symbol arrives, or ahead of time with `prepare_symbols`.

Each book is self-contained. It owns its own price levels, order index, and memory pools, so activity in one symbol doesn't affect another. This also gives me a clean path toward sharding books across cores later if I decide to go there.

### Price-time priority

The book uses price-time priority.

`bids_` and `asks_` are `std::pmr::map<Price, Level*>` containers. Bids are ordered with `greater` and asks with `less`, which means the best price is always at `begin()`.

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

The book's associative containers (`bids_`, `asks_`, `stop_buys_`, `stop_sells_`, and `orders_`) use `std::pmr` and are backed by `OwnedMonotonicBuffer`.

The idea is simple: allocate one larger arena up front and let the PMR containers allocate their nodes from it. That keeps the implementation simpler than writing a separate custom pool for every map type while still giving me predictable allocation behavior.

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

This also makes the matching engine easier to test because I can assert against the exact event sequence. More importantly, it gives me a clean boundary for a future sequencer, journal, or network layer without making the matching engine depend on transport.

### No exceptions

The `exchange_project` target is compiled with `-fno-exceptions`.

Instead of throwing, the engine reports failures through explicit `ReasonCode` values and `std::optional` results. That fits the predictable-latency goal of the project and keeps error handling explicit.

## Supported Order Types

The order types are defined in `core/types.h::OrderType`.

| Type | Behavior |
| --- | --- |
| `LIMIT` | Standard resting limit order. |
| `MARKET` | Executes immediately against the best available prices. Rejected if the opposite side of the book is empty. |
| `IOC` | Matches whatever it can immediately. Any remaining quantity is canceled instead of resting. |
| `FOK` | Rejected unless the entire quantity can be filled immediately. The check is done up front using `available_liquidity`. |
| `GTC` | Resting limit order. In this engine, it behaves like `LIMIT` because there is no separate expiry logic yet. |
| `STOP` | Waits in `stop_buys_` or `stop_sells_` until the last trade price crosses the trigger price. It then becomes a `MARKET` order and enters matching. |
| `STOP_LIMIT` | Same trigger mechanism as `STOP`, but becomes a `LIMIT` order at the specified price. |
| `ICEBERG` | Only `peak_qty` / `display_qty` is visible at a time. When the visible quantity is exhausted, hidden quantity is replenished at the back of the FIFO queue. |
| `POST_ONLY` | Rejected if it would immediately cross the book, guaranteeing that it only adds liquidity. |

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
   If the order is completely filled, it is retired as `FILLED`.

   If quantity remains and the order type is allowed to rest (`LIMIT`, `GTC`, `ICEBERG`, `POST_ONLY`, or a triggered `STOP_LIMIT`), the order is inserted into the book and an `ORDER_RESTED` event is emitted.

   Otherwise, such as with `IOC`, `FOK`, or a remaining `MARKET` quantity, the remainder is discarded.

6. **Trigger stop orders.**  
   After a trade, `trigger_stop_orders` checks whether the new `last_trade_price_` crossed any stop prices. Triggered orders are converted and injected back into the matching engine. This repeats until there are no more triggers, so cascading stop orders are handled too.

## Modifying Orders

`modify_order` follows a simple rule based on how real exchanges generally treat priority changes.

- If the price changes, the order is canceled and reinserted. It loses its original time priority.
- If the price stays the same and the quantity is only reduced, the order is modified in place and keeps its queue position.
- Increasing quantity forfeits the existing priority and sends the order to the back of the queue.

I wanted this behavior to be explicit rather than hiding the priority rules behind a bunch of special cases.

## Event Model

`matching/events.h` defines a tagged union:

```text
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
- `ORDER_CANCELED`
- `TRADE`

All payload structures are `trivially_copyable`, enforced with `static_assert`.

The longer-term idea is that the event stream should be cheap to copy into a journal, log, or wire format without requiring an expensive serialization layer in the matching path.

## Core Primitives

The main building blocks live under `src/core/`:

- **`types.h`** — Strong-ish aliases such as `Price`, `Quantity`, `OrderId`, and `Timestamp`. It also contains the fixed-size `Symbol` type and the `PRICE_SCALE` used for fixed-point pricing.
- **`memory_pool.h`** — Fixed-capacity, free-list `MemoryPool<T, N>` with cache-line-aligned slots.
- **`spsc_queue.h`** — Lock-free single-producer/single-consumer ring buffer with power-of-two capacity and cache-line-padded head/tail cursors. This is currently intended for future handoff between the matching thread and something like a network or logging thread.
- **`clock.h`** — A monotonic `LogicalClock` for deterministic sequencing plus a `steady_clock`-based nanosecond timestamp helper.

## Project Layout

```text
src/
├── core/
│   ├── types.h
│   ├── memory_pool.h
│   ├── spsc_queue.h
│   └── clock.h
├── matching/
│   ├── order.h
│   ├── level.h
│   ├── events.h
│   ├── order_book.h
│   └── matching_engine.h
├── exchange_main.cpp
└── ...

tools/
├── trader_terminal.cpp
└── liquidity_bot.cpp

CMakeLists.txt
```

## Building

You'll need:

- C++20
- GCC 11+ or Clang 14+
- CMake 3.24+

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The build produces:

```text
build/src/exchange
build/tools/trader_terminal
build/tools/liquidity_bot
```

All targets link against the header-only `exchange_project` interface library and are built with:

```text
-Wall -Wextra -Wpedantic -fno-exceptions
```

## Design Decisions

### Fixed capacities instead of dynamic growth

`OrderBook` and `MatchingEngine` take `OrderCapacity`, `LevelCapacity`, `EventCapacity`, and `StopCapacity` as non-type template parameters.

The defaults are:

- 1M orders
- 10K levels
- 262K events
- 64K stops

This makes the memory footprint a build-time decision. The downside is less flexibility, but I think that trade-off makes sense for a low-latency engine where I want memory usage to be predictable.

### FIFO instead of pro-rata

I'm using FIFO matching for now because it keeps the price-level structure simple: an intrusive doubly-linked list is enough.

Pro-rata or size-priority matching would need a different structure, probably something sorted by size or another specialized data structure. That's something I may experiment with later.

### Failing fast on configuration problems

The engine uses `std::abort()` for event-buffer overflow and level allocation failure.

I'm treating both as configuration errors rather than normal runtime conditions. If the configured pools are too small for the workload, I'd rather find that out clearly than silently introduce dynamic allocation or unpredictable recovery behavior.

## What's Next?

There is still a lot I want to build on top of the core engine:

- [ ] Wire `exchange_main.cpp` to actually run the `MatchingEngine`
- [ ] Add network transport/protocol for `trader_terminal` and `liquidity_bot`
- [ ] Persist and journal the event stream
- [ ] Add the next stage of concurrency using the existing SPSC queue
- [ ] Build a real trader terminal
- [ ] Build the liquidity/market-making bot
- [ ] Ship Part 2 and Part 3

This is still a learning project, so I'm expecting the design to evolve as I test it under more realistic workloads.

If you spot something that could be designed better, or notice a bug in the approach, feel free to open an issue. I'm especially interested in feedback around the matching logic, memory management, and the trade-offs I'm making for predictable latency.
