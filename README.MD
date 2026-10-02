# Stock Exchange in C++20

A small stock exchange built from scratch in modern C++20: a limit order book and matching engine, a TCP server around it with a crash-safe journal, a trading terminal, and a bot that keeps a market going.

This page covers how to build it, run it and what it measures at. For how it works inside and why it's built the way it is, see [DESIGN.md](DESIGN.md).

## What's in the box

| Binary            | What it does                                                        |
| ----------------- | ------------------------------------------------------------------- |
| `stock_exchange`  | The server: matching engine, TCP gateway and journal                |
| `trader_terminal` | Interactive (or scripted) client for placing and managing orders    |
| `liquidity_bot`   | A market maker, or a random taker, so there is something to trade   |
| `bench_engine`    | Benchmark of the matching core                                      |
| `journal_dump`    | Prints a journal file in readable form                              |

It has only been built and run on Linux (GCC 13). The code is plain POSIX and should work on macOS, but I haven't tried it there.

## Build

You'll need C++20 (GCC 11+ or Clang 14+) and CMake 3.24+.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Binaries land in `build/src`, `build/tools` and `build/tests`.

| Option                              | Effect                                              |
| ----------------------------------- | --------------------------------------------------- |
| `-DEXCHANGE_SANITIZE=ON`            | AddressSanitizer + UBSan                            |
| `-DEXCHANGE_SANITIZE_THREAD=ON`     | ThreadSanitizer                                     |
| `-DEXCHANGE_BUILD_TESTS=OFF`        | Skip the tests                                      |
| `-DEXCHANGE_ORDER_CAPACITY=N` (also `LEVEL`, `EVENT`, `STOP`) | Per-symbol capacities of the server |

## Try it in one command

The demo starts an exchange, a maker and a taker, and drops you into a terminal. Quitting the terminal shuts everything down.

```
./scripts/demo.sh
> book AAPL
> sub AAPL
> buy AAPL 50 market
> sell AAPL 100 @ 151.25
> orders
```

## Running the pieces yourself

Each in its own shell:

```
build/src/stock_exchange --port 9001
build/tools/liquidity_bot --participant 100 --symbol AAPL:150
build/tools/liquidity_bot --participant 200 --mode taker
build/tools/trader_terminal --participant 1
```

Every binary has `--help`.

### `stock_exchange`

| Option              | Default                 | Meaning                                              |
| ------------------- | ----------------------- | ---------------------------------------------------- |
| `--port N`          | 9001                    | TCP port                                             |
| `--bind ADDRESS`    | 127.0.0.1               | Address to listen on                                 |
| `--symbols A,B,C`   | AAPL,MSFT,GOOG,TSLA     | Tradable symbols (up to 16)                          |
| `--journal PATH`    | exchange.journal        | Journal file                                         |
| `--no-journal`      |                         | Run without journaling or recovery                   |
| `--sync-ms N`       | 50                      | Longest time journaled data may sit unsynced         |
| `--busy-spin`       | off                     | Spin the matching thread when idle (lower latency, one core at 100%) |
| `--quiet`           | off                     | No start-up or connection messages                   |

Stop it with Ctrl-C. Start it again with the same journal and every open order is back: orders belong to a participant id, not a connection, so you can reconnect as the same participant and carry on. To start from an empty book, delete the journal file.

If you change `--symbols` (or the capacities) and point it at an old journal, it will refuse to start rather than come up with different state. Use a new journal path.

**There is no authentication.** Anyone who can reach the port can log in as any participant. Keep it on localhost; this is a learning project, not something to expose.

### `trader_terminal`

```
build/tools/trader_terminal [--host 127.0.0.1] [--port 9001] [--participant 1]
```

A participant id can only be connected once at a time.

```
buy|sell SYMBOL QTY @ PRICE                 limit
buy|sell SYMBOL QTY market                  market
buy|sell SYMBOL QTY @ PRICE ioc|fok|gtc|post
buy|sell SYMBOL QTY @ PRICE iceberg DISPLAY_QTY
buy|sell SYMBOL QTY stop TRIGGER            stop (market when triggered)
buy|sell SYMBOL QTY @ PRICE stop TRIGGER    stop-limit
modify ORDER_ID QTY @ PRICE                 same price + smaller qty keeps queue position
cancel ORDER_ID | cancel all
orders                                      your open orders
book SYMBOL                                 ten best levels each side
sub SYMBOL | unsub SYMBOL                   live trades and best bid/offer
symbols | help | quit
```

A session looks like this:

```
> sell AAPL 50 @ 150.25 iceberg 20
[order 2] accepted: SELL 50 AAPL @ 150.25 ICEBERG
[order 2] resting: showing 20 of 50 @ 150.25
> buy AAPL 60 @ 150.50
[order 5] accepted: BUY 60 AAPL @ 150.50 LIMIT
[order 5] FILLED 20 @ 150.25 (BUY AAPL), 40 still open
...
```

It also reads commands from a pipe, with `wait MS` to pause, which makes it scriptable:

```
printf 'buy AAPL 100 @ 150\nwait 200\norders\n' | build/tools/trader_terminal
```

### `liquidity_bot`

```
build/tools/liquidity_bot --participant 100 --symbol AAPL:150 --symbol MSFT:300
build/tools/liquidity_bot --participant 200 --mode taker --interval-ms 400
```

- `--mode maker` (default) quotes a ladder of bids and offers around a fair value that drifts as a random walk. It leans its quotes against its own inventory, stops quoting a side past `--max-position`, and pulls its quotes on exit.
- `--mode taker` sends small random market orders, so the maker's quotes actually trade.

With no `--symbol` it trades every listed symbol, starting from the current market or `--price` (default 100.00). Other knobs: `--levels`, `--size`, `--half-spread`, `--step`, `--volatility`, `--interval-ms`, `--duration-sec`, `--seed`. It prints position and P&L every five seconds.

Neither mode is a trading strategy. They exist so there is a market to trade against.

### `journal_dump`

```
build/tools/journal_dump exchange.journal [--commands | --events] [--limit N]
```

```
COMMAND ADD     order=1 participant=100 client_id=1 BUY AAPL 100 LIMIT price=149.94 ts=...
  event #1        ORDER_ACCEPTED         order=1 BUY AAPL 100 @ 149.94 LIMIT
  event #2        ORDER_RESTED           order=1 showing 100 @ 149.94
```

## Results

### Tests

`ctest --test-dir build` runs eight test programs, 88 cases in all:

| Program                | What it checks                                                                  |
| ---------------------- | ------------------------------------------------------------------------------- |
| `test_core`            | Memory pool, SPSC queue (including two threads), arena, price parsing           |
| `test_order_book`      | Every order type, modify and cancel rules, stop cascades, icebergs, capacity limits |
| `test_matching_engine` | Routing by symbol, lazy books, global sequence numbers                          |
| `test_fuzz`            | 300,000 random operations compared trade-for-trade with a naive reference book; a second run with icebergs and stops rebuilds all open orders from events alone and checks them against the book |
| `test_no_alloc`        | Zero calls to `operator new` while 400,000 mixed orders run through the engine  |
| `test_journal`         | Round trip, torn tail, corrupted record, foreign file                           |
| `test_protocol`        | Framing under arbitrary fragmentation, malformed input                          |
| `test_exchange`        | A real server and real TCP clients: trading, ownership, rejects, a 30,000-order burst, restart recovery |

All of them pass, and also pass under ASan/UBSan and ThreadSanitizer (`test_no_alloc` is skipped there because the sanitizers replace the allocator). The GitHub Actions workflow in `.github/workflows/ci.yml` runs the same matrix, plus a Clang build, on every push.

### Benchmark

```
build/tools/bench_engine --orders 5000000
```

`bench_engine` feeds a pre-generated, market-like stream (55% passive limit orders, 30% cancels, 7% modifies, 8% aggressive orders) straight into `MatchingEngine::submit()` and times each call. On the one machine I've run it on (a single shared virtual core, GCC 13, `-O3`):

```
throughput  ~6.5 M orders/s
p50   111 ns      p99    461 ns
p90   195 ns      p99.9  630 ns
```

The clock read costs about 22 ns per sample and is not subtracted. Treat these as a rough indication rather than a result: it's one run on unpinned shared hardware, and it measures the matching core only. I haven't measured end-to-end latency through the gateway, and by default the matching thread sleeps for up to ~100 µs when idle (use `--busy-spin` to trade a core for latency).

### Memory

At the default capacities the server uses about 40 MB per symbol (about 175 MB resident with the four default symbols), all allocated at start-up.

## Known limits

- No authentication or per-participant risk limits.
- No self-trade prevention: a participant can trade with itself.
- Journal writes are asynchronous. A process crash loses nothing that reached the OS; a power failure can lose up to `--sync-ms` of acknowledged orders.
- No journal snapshots: start-up replays the whole file, and the file grows forever.
- A journal is tied to the build and symbol list that wrote it.
- `GTC` behaves like `LIMIT`; there is no order expiry.

## What's Next?

- [x] Wire `stock_exchange_main.cpp` to actually run the `MatchingEngine`
- [x] Add network transport/protocol for `trader_terminal` and `liquidity_bot`
- [x] Persist and journal the event stream
- [x] Add the next stage of concurrency using the existing SPSC queue
- [x] Build a real trader terminal
- [x] Build the liquidity/market-making bot
- [ ] Ship Part 2 and Part 3

Things I'd like to get to:

- [ ] Authentication, and per-participant risk limits
- [ ] Self-trade prevention
- [ ] Journal snapshots, so start-up doesn't replay everything
- [ ] Cancel-on-disconnect as a per-session option
- [ ] End-to-end latency measurement through the gateway
- [ ] Order expiry (so `GTC` means something different from `LIMIT`)
- [ ] `epoll`/`io_uring` instead of `poll`, and sharding books across cores

This is still a learning project, so I'm expecting the design to evolve as I test it under more realistic workloads.

If you spot something that could be designed better, or notice a bug in the approach, feel free to open an issue. I'm especially interested in feedback around the matching logic, memory management, and the trade-offs I'm making for predictable latency.