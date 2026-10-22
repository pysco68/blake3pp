# Performance tracing

[← blake3pp](../README.md)

A file hashing run has two halves that can each be the limit: the device
delivering windows and the cores compressing them. Which one it is cannot
be read off a throughput figure, so the file pipeline records where every
window's time went and hands the records back.

The library records and nothing more. It never allocates, formats or
writes: the caller provides the storage, reads it back after the call, and
turns it into whatever it needs. `<blake3pp/trace.hpp>` is the whole
facility, and it is in the umbrella header.

## What is recorded

| record | one per | written by |
|---|---|---|
| `window_record` | I/O window | the thread driving the pipeline |
| `agent_record` | bulk invocation that compressed at least one part, per window | the agent itself, opt-in |
| `driver_stats` | run | the driver thread, pipeline path only |

Tracing covers file input: `update_file()` and `hash_file()`, sequential
and over a scheduler. In-memory hashing is not instrumented, and neither
is the experimental [many-files layer](async.md). The finest timed unit is
one part, a subtree of at least 16 chunks; nothing below that is timed.

The sequential window loop has no driver thread, so it fills window
records and leaves `driver_stats` zeroed. Agent records exist only where
windows fan out over a scheduler.

## Turning it on

Storage is a span the caller owns, wrapped in a `trace_buffer` and handed
to the pipeline through `file_io_options::trace` (or
`hash_file_options::trace`, which converts to it):

```cpp
#include <blake3pp/parallel_io.hpp>
#include <blake3pp/trace.hpp>

constexpr std::size_t window_bytes = 8 * 1024 * 1024;

std::vector<blake3pp::window_record> windows(
    std::filesystem::file_size(path) / window_bytes + 1);
blake3pp::trace_buffer trace(windows);

blake3pp::hasher h;
blake3pp::update_file(h, path, sched,
                      {.window_bytes = window_bytes, .trace = &trace});

for (const blake3pp::window_record& w : trace.windows()) {
  report(w.index, w.t_ready - w.t_wait_begin,   // waiting for the read
                  w.t_joined - w.t_ready);      // compressing
}
const blake3pp::driver_stats& d = trace.driver();
```

The senders in `<blake3pp/async_io.hpp>` take the buffer differently.
An operation state is a type fixed when the receiver is connected, so a
sender cannot decide from a pointer at start whether its scope records;
it takes the decision as a type instead, as its last argument:
`blake3pp::buffer_tracer{&trace}` records, and the default
`blake3pp::null_tracer` records nothing at no cost. `file_io_options::trace`
is not read on that path.

```cpp
auto s = blake3pp::async::async_update_file(ctx, h, path, sched, {},
                                            blake3pp::buffer_tracer{&trace});
```

`async_update_files` takes the same argument and fills one buffer with
every file's windows; each record's `index` is the window's position in
its own file.

Per-agent records are opt-in, by giving the buffer a second span. One
bulk invocation is at most one record per thread per window, which sizes
it:

```cpp
std::vector<blake3pp::agent_record> agents(windows.size() * threads);
blake3pp::trace_buffer trace(windows, agents);
```

Records are claimed in order until the storage is full, and every claim
beyond that is a counted drop — never a stall, never a wrong digest. Check
`dropped_windows()` and `dropped_agents()` before reading anything else:
`window_bytes` is rounded *down* to a power-of-two multiple of
`chunk_size` (minimum 64 KiB), so storage sized from the requested value
can come out short. `clear()` forgets the records but keeps the epoch, so
a tool that runs several reps into one buffer can still compare
timestamps across them.

Timestamps are nanoseconds on `std::chrono::steady_clock` relative to the
buffer's construction, so a difference is a duration. `epoch_ns()` gives
that zero as absolute `steady_clock` nanoseconds (`CLOCK_MONOTONIC` on
Linux) for joining with timestamps another tool took on the same clock.

### What it costs

Whether a run is traced is a template parameter settled once, when the run
starts, not a null check on the hot path. A run with no buffer pays one
branch per window and nothing inside the compress body. A traced run pays
a handful of clock reads per window on the driving thread and two atomic
adds per bulk invocation; with agent records, one clock read per part and
one record claim per invocation on top.

## The fields

### `window_record`

Five timestamps, from the pipeline asking for a window to the buffer
being back with the reader:

| field | the instant |
|---|---|
| `t_wait_begin` | before the pipeline asked the reader for the window |
| `t_ready` | the reader handed it out; its bytes are in memory |
| `t_joined` | the compute is done: every agent joined, or `update()` returned for a window hashed inline |
| `t_absorbed` | the chaining values are in the hasher |
| `t_released` | the window's buffer is back with the reader |

The four differences are the stages a window passes through — **wait**,
**hash**, **absorb**, **release** — and are what the bench's summary and
its Chrome trace show. `t_absorbed` equals `t_joined` wherever the hasher
took the window whole and there was no separate absorb step: every window
on the sequential path, and the last window of any run, which carries the
message end and so can only be hashed by the hasher itself.
[Inside the pipeline](pipeline.md) is what those stages are made of.

The rest describes the window:

| field | meaning |
|---|---|
| `index` | position within this call, from 0 |
| `bytes` | bytes in the window |
| `agent_busy_ns` | summed over the agents that took part: nanoseconds compressing |
| `agents_active` | bulk invocations that compressed at least one part |
| `slot` | which window object carried it — the track its phases belong on when several are in flight; 0 on the sequential path, which has one |
| `flags` | `flag_parallel` (fanned out over the scheduler), `flag_last` (reaches the end of the file), `flag_short_first` (the short window that brings a part-way hasher back onto a window boundary) |

### `agent_record`

One bulk invocation over a window's parts: which window, when it started
(`t_begin`), how long it was busy, how many parts it took, the shortest
and longest of those parts, the chunks per part, and the CPU it was on at
`t_begin` (`sched_getcpu()` on Linux, `UINT32_MAX` where the platform
cannot say). Bytes compressed is `parts * part_chunks * chunk_size`, which
over `busy_ns` is that invocation's effective rate.

### `driver_stats`

Once several windows are in flight the stage shares of a window no longer
sum to the wall clock, so this is the record that says whether the driving
thread is the bottleneck.

| field | meaning |
|---|---|
| `busy_ns` | outside a blocking poll: submitting, reaping, running completions, inserting into the reducer |
| `parked_ns` | inside a blocking poll, waiting for a read or for a window to come back |
| `iterations` | times round the loop |
| `polls`, `blocking_polls` | `poll()` calls, and those of them that blocked |
| `wakes` | blocking polls released without a completion of their own: a thread handing a finished window back. One per window is the expected shape |
| `queue_runs` | completions run off the run queue |
| `admission_stalls` | windows held back because the reducer had no node free |
| `max_pending` | the most subtrees the reducer held at once: how far ahead of the tree the reads ran |
| `max_reads_in_flight` | the most reads submitted and not yet landed: what the device saw |
| `max_windows_in_flight` | the most windows reading, on the pool or waiting to reduce |
| `read_backend` | the rung the reads engaged, through `read_backend(d)` |

`read_backend(d)` is how the read side's degradation becomes visible to a
caller: `"io_uring+direct"` when everything was granted, `"io_uring"` when
the filesystem refused `O_DIRECT`, `"pread [io_uring: EPERM, blocked by
policy]"` when the engine itself was, and so on. [Direct I/O](direct-io.md)
lists the rungs.

## How to read a run

**`busy_ns` against `parked_ns` is the first question.** A driver parked
most of the run has room for more work and the limit is elsewhere; a busy
driver *is* the limit.

**Per-window stage shares can exceed 100% of the wall clock, and should.**
They are per-window shares of a pipeline, not a timeline: with several
windows in flight their sum exceeds the wall time by however much the
pipeline overlaps. A `wait` share far above 100% means windows spent most
of their lives queued behind the device.

**Agent records answer "which core".** Aggregated per CPU, bytes over busy
time is an effective rate, and the run's min, median and max of it
separate a slow core from a slow run. A large `max_part_ns / min_part_ns`
on one invocation is preemption or a cache-cold part, not a slow core.
Cores that only arrive after the part counter is drained produce no
records at all.

**Summed per-CPU rates against what the run delivered** is the gap that is
not compression: reading, waiting, and the stretches where a core had
nothing to compress.

## In the tools

`blake3pp_bench_file` is the worked example: it sizes the storage, prints
a per-window summary and writes the run as Chrome trace JSON.
`blake3ppsum` and `blake3ppgen` expose no tracing of their own; what they
expose are the knobs a trace settles. See
[Command-line tools](tools.md#tracing).

```
--trace PATH      record per-window timing; summarize the last rep of each
                  row and write it as Chrome trace JSON to PATH
--trace-agents    with --trace: also record each pool agent's part of every
                  window (parts, busy time, cpu)
```

Every rep starts from an empty buffer, so the last rep of each row is what
gets summarized and exported. With both rows, `PATH` becomes
`<stem>.seq.<ext>` and `<stem>.parallel.<ext>`.

The captures below are from a 32-thread Zen 5 machine (Ryzen AI MAX+ 395)
under WSL2, on its virtual disk and without cache drops, so the device
figures are that disk's rather than an SSD's. The shape of the output is
the subject.

### A compute-bound run

`--null-source` runs the pipeline with no device under it, so the hash is
the only thing left to be the limit:

```
$ blake3pp_bench_file --null-source 16 --reps 1 --cooldown 0 \
      --trace null.json --trace-agents
null source: 16.00 GiB, window 8 MiB, qd 4, 32 threads
null           84.0 GiB/s   (8390e37432eb3939...)  [digest matches the in-memory hash]
  trace (last rep): 2048 windows (2048 fanned out, 0 dropped), wall 0.190 s
    per window: wait  18.5%  hash 1337.2%  absorb 155.1%  release   0.1%
    driver busy   4.0% of its own 0.190 s, 870 iterations, 869 polls (814 blocking, 284 woken), 2048 completions, 0 admission stalls, reducer held at most 21, at most 4 reads and 16 windows in flight
    median per window: submit->read      2.4 us  read->cv   1296.4 us  cv->reduced     32.0 us
    pool busy  79.3% of 32 threads, 23.5 agents active per fanned window
    io-wq cpu = 0.00 cores (worker threads' cpu / wall); machine (/proc/stat): user 23.67 nice 0.00 sys 0.85 idle 6.51 iowait 0.00 irq 0.00 softirq 0.00 steal 0.00 cores
```

Line by line. All 2048 windows fanned out and none was dropped, so the
storage was sized right. The `hash` share of 1337% is sixteen windows'
worth of overlap, not an error: each window spent 1.3 ms being compressed
while fifteen others did the same. `absorb` overlaps the same way. `wait` at
18.5% is the source handing back buffers it never read. The driver was busy
4.0% of its own time, so the loop is not the limit. `pool busy 79.3% of 32
threads` is the number that is: the agents' summed compress time over
threads × wall, which is as close to the machine as this engine gets. The
23.5 agents per window beside it is how many invocations found parts left
to take, out of the thirty-two threads the pool has.

With `--trace-agents`, the same run resolves that per core:

```
    48202 agent records (0 dropped) on 32 cpus:
        cpu  records    parts    busy ms      effective      irqs
          0     1546    32901      151.7     3.31 GiB/s       227
          1     1555    32906      152.2     3.30 GiB/s       169
          ...
         31     1498    32925      152.0     3.30 GiB/s       191
      per-cpu GiB/s: min 3.26, median 3.30, max 3.40; worst max/min part on one agent: 371.9x
      sum of per-cpu rates 105.99 GiB/s; the run delivered 84.05 GiB/s, 79.3% of it
```

The spread of 3.26 to 3.40 GiB/s is one core's compress rate against
another's, and on a machine whose cores are alike that is how it should
look; a single outlier would be the core to look at. The `irqs` column is
each core's interrupt count over the rep, which is how a core that services
the device is told from a slow one — here they are even, because a null
source raises none. The `max/min part` ratio of 372x is one invocation
whose longest part took that much longer than its shortest, which is a
scheduled-out agent rather than a slow core, since that core's own rate is
unremarkable. The last line bounds what is left to win: the cores together
managed 105.99 GiB/s while compressing, the run delivered 79.3% of that,
and the missing fifth is everything that is not compression.

### A read-limited run

The same flags over a real file, parallel row:

```
$ blake3pp_bench_file --make 1024 --reps 1 --cooldown 0 \
      --trace file.json --trace-agents
file: blake3pp_bench_file.dat (1024.0 MiB), backend: io_uring+direct, window 8 MiB, qd 4
trace: 129 window records and 4128 agent records, JSON to file.json
cache: not dropped (needs root or elevation); direct I/O bypasses it, metadata stays warm
raw io         2.09 GiB/s   [device ceiling, no hashing]
...
parallel       2.14 GiB/s   (1ec8ab3adeac66e7...)  [102% of raw]
  trace (last rep): 128 windows (128 fanned out, 0 dropped), wall 0.468 s
    per window: wait 396.1%  hash  13.5%  absorb   1.2%  release   0.0%
    driver busy   0.6% of its own 0.468 s, 253 iterations, 252 polls (252 blocking, 125 woken), 128 completions, 0 admission stalls, reducer held at most 16, at most 4 reads and 6 windows in flight
    median per window: submit->read  14295.1 us  read->cv    489.4 us  cv->reduced     44.5 us
    pool busy   1.0% of 32 threads, 12.4 agents active per fanned window
    io-wq cpu = 0.04 cores (worker threads' cpu / wall); machine (/proc/stat): user 0.70 nice 0.00 sys 1.13 idle 28.61 iowait 0.96 irq 0.00 softirq 0.00 steal 0.00 cores
    1581 agent records (0 dropped) on 27 cpus:
        cpu  records    parts    busy ms      effective      irqs
          0       24      729        1.9     5.79 GiB/s      1805
          1      116     9610       21.1     6.95 GiB/s       858
          ...
         13        1        1        0.0      828 MiB/s       400
         30       89     5832       13.1     6.78 GiB/s      1034
```

Every line of the compute-bound reading inverts. The median window waited
14.3 ms for its read and was compressed in 0.49 ms, `wait` dominates the
shares at four windows' worth of overlap, the pool was busy 1.0% of
thirty-two threads and the driver 0.6% of its own time. The run delivered
2.14 GiB/s against a device ceiling of 2.09, so there is nothing left to
win here at all: the hash is free and the disk is the answer. Nothing in
this run is a hashing problem, and the `--window`/`--qd` sweep
(`--io-sweep`) is what one is handed to next.

The agent table reads differently too. Records land on 27 of the 32 cores
rather than all of them, several with one record and a rate that means
nothing, because there was not enough work to go round — a pool that is 1%
busy does not fill a machine. The `irqs` column is populated now (cpu 0 at
1805 against ~900 elsewhere): that is the core taking the device's
completions, and on a read-limited run it is the one core whose numbers are
not about hashing.

The last line is Linux's own accounting, printed with every traced summary
and interesting once there is a device. `io-wq cpu` is the CPU time of the
kernel's io_uring worker threads (`iou-wrk-*` under `/proc/self/task`)
over the rep's wall time, in cores — 0.04 here, which is what issuing
direct reads costs off the driving thread at this rate, and 0.00 in the run
above, which issues none. Workers come and go with the load, so the figure only
credits workers alive at both ends of the rep. `machine (/proc/stat)` is
the whole machine over the same window, so the kernel's share can be told
from the pool's. The agent table's `irqs` column needs `/proc/interrupts`
to carry per-CPU columns, which it does on a Linux host and inside WSL2,
and does not in every container.

### The Chrome trace

`--trace PATH` writes the same rep as Chrome trace JSON. Open it in
[Perfetto](https://ui.perfetto.dev/) or `chrome://tracing`:

```json
{"epoch_ns":177415647362187,"displayTimeUnit":"ns","pool_threads":32,"traceEvents":[
{"name":"process_name","ph":"M","pid":1,"args":{"name":"blake3pp_bench_file null"}},
{"name":"thread_name","ph":"M","pid":1,"tid":1,"args":{"name":"driver"}},
{"name":"driver","ph":"X","pid":1,"tid":1,"ts":244016.924,"dur":29103.355,"args":{"busy_ns":2564772,"parked_ns":26531260,"iterations":161,"polls":160,"blocking_polls":146,"wakes":68,"queue_runs":256,"admission_stalls":0,"max_pending":17}},
{"name":"thread_name","ph":"M","pid":1,"tid":10,"args":{"name":"window object 0"}},
{"name":"wait","ph":"X","pid":1,"tid":10,"ts":244016.924,"dur":877.162,"args":{"window":0,"slot":0,"bytes":8388608,"last":0,"parallel":1}},
{"name":"thread_name","ph":"M","pid":1,"tid":10,"args":{"name":"window object 0"}},
{"name":"hash","ph":"X","pid":1,"tid":10,"ts":244894.086,"dur":1203.918,"args":{"window":0,"slot":0,"bytes":8388608,"last":0,"parallel":1}},
```

| track | holds |
|---|---|
| `tid` 1, "driver" | one event over the whole run, carrying `driver_stats` in `args` |
| `tid` 10 + `slot`, "window object N" | the `wait`, `hash`, `absorb` and `release` events of every window that object carried |
| `tid` 100 + cpu (99 unknown), "cpu N" | one `compress` event per agent record, with its parts and part extremes |

Windows go on the track of the window object that carried them rather
than on one shared track, so several in flight read as parallel lanes
instead of as one track nesting into itself. The agent tracks put the
cores directly under the pipeline, which is where a window waiting on one
straggling part is visible as such. `ts` and `dur` are microseconds, as
the format wants.

### Pairing with perf

The records and `perf` share `CLOCK_MONOTONIC`, and the JSON's top-level
`epoch_ns` is the records' zero on that clock:

```bash
perf record -k CLOCK_MONOTONIC -e cycles --call-graph lbr -- \
    blake3pp_bench_file FILE --trace t.json --trace-agents
perf script -F time,cpu,ip,sym
```

A sample at time `T` seconds belongs to the window whose interval covers
`T * 1e9 - epoch_ns`, and to the agent record with the same `cpu` whose
`[t_begin, t_begin + busy_ns)` covers it. No probes or markers are
involved; the join is by time and CPU alone.
