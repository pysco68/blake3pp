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

Tracing covers the file pipeline: `update_file()` and `hash_file()`,
sequential and over a scheduler, `update_stream()`, `write_file()` over a
scheduler, and the senders of the experimental
[many-files layer](async.md). In-memory hashing is not instrumented. The
finest timed unit is one part, a subtree of at least 16 chunks; nothing
below that is timed.

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
| `file` | the file's index in a many-files operation (`async_update_files`), which fills one buffer with every file's windows; 0 for a single file |
| `flags` | `flag_parallel` (fanned out over the scheduler), `flag_last` (reaches the end of the file), `flag_short_first` (the short window that brings a part-way hasher back onto a window boundary), `flag_part_run` (a stream window that reached the tree as a run of parts: one the producer left short, or the one after it that brings the grid back) |

### `agent_record`

One piece of a window's work on one agent. For a bulk invocation over
the window's parts: which window and file, when it started (`t_begin`),
how long it was busy, how many parts it took, the shortest and longest of
those parts, the chunks per part, and the CPU it was on at `t_begin`
(`sched_getcpu()` on Linux and FreeBSD, processor group × 64 + number on
Windows, `UINT32_MAX` where the platform cannot say).
Bytes compressed is `parts * part_chunks * chunk_size`, which over
`busy_ns` is that invocation's effective rate. A record with `parts == 0`
is the fold of the window's chaining values, which runs on the agent
that finished the bulk last, and its `busy_ns` is the fold's own time; on
the write side a record is one window's fill. In the Chrome trace each
of these is a slice on its CPU's lane, named `compress`, `fold` or
`fill`, with a flow arrow from the window's compute phase on the
pipeline side, so selecting either end highlights the other.

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
| `transfers_landed`, `transfer_bytes` | windows whose transfer landed, and the bytes they moved |
| `device_wait_ns` | summed per-window time the device had the transfer, stamped by whoever performed it; where the driver cannot attribute (memory, a stream), the admission-to-landing span |
| `starved_ns` | the pipeline unable to use a window it already held: reading, no free window; writing, nothing to write |
| `queue_depth_final`, `queue_depth_peak` | the depth the run ended on and the deepest it reached |
| `agent_busy_ns` | summed over every window and agent: the pool compressing on a read run, filling on a write one |
| `device_wait_min_ns` | the shortest of those per-transfer times: the device with nothing queued ahead of it |
| `device_depth` | the most transfers the engine could have at the device at once: the queue depth on an asynchronous engine, the I/O threads on the synchronous rung (one on Windows); 0 where the driver cannot say |
| `lane_wait_ns` | time transfers waited for one of those I/O threads before they started; nonzero only where those threads served transfers |
| `read_backend` | the rung the reads engaged, through `read_backend(d)` |

The four transfer counters are kept on every run, traced or not, and a
write run fills them as a read run does. They cost two clock reads per
window. `agent_busy_ns` is collected the same way -- for a buffer with no
window storage at all -- because the pool's busy share is the hash side's
answer to the question the device counters answer for the device, and a
caller may want it without paying for one record per window. It costs one
relaxed add per window on a read run, and two clock reads and an add on a
write one.

`starved_ns` is NOT a measure of how starved the pool was on a read run.
It counts "bytes left and no free window", which can only fire where the
arena is no wider than the queue depth, and it fires there even when the
run is purely hash-limited: a 64 MiB x depth 16 arm at 96% `agent_busy_ns`
still showed 1.33 s of it. Read it as "the arena is not wider than the
depth" and use `agent_busy_ns` for the hash side. Writing, it is the clean
signal it reads like, because there the windows in flight are the
generator's parallelism.

`parked_ns` sums waiting for a transfer and waiting for a window. That
answers "is the driver the bottleneck" and neither end on its own, so
`device_wait_ns` and `starved_ns` separate the two.

`device_wait_ns / transfers_landed` is the device's mean per-window
latency. Against `device_wait_min_ns` the same mean says how queued the
device was: a mean at the depth times the minimum is a queue that was full
for the run, and a mean at the minimum is a device that served every
transfer alone and had capacity to spare. The absolute latency says
neither on its own, since a queue's latency is proportional to its depth
by construction.

`queue_depth_final` is the geometry to pass back as `queue_depth` to
repeat a run.

Fields are appended to `driver_stats`, never inserted, because a caller
may read it out of a build it did not compile.

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

## Asking the library

`<blake3pp/trace.hpp>` does the same arithmetic the tools print. Pass a
finished run's `driver_stats` and the wall clock measured around the call:

```cpp
#include <blake3pp/trace.hpp>

std::vector<blake3pp::window_record> windows(count);
blake3pp::trace_buffer trace(windows);

const auto t0 = std::chrono::steady_clock::now();
blake3pp::update_file(h, path, {.trace = &trace});
const double wall_ns = std::chrono::duration<double, std::nano>(
    std::chrono::steady_clock::now() - t0).count();

const blake3pp::diagnosis d = blake3pp::diagnose_read(
    trace.driver(), wall_ns, threads, planned_windows, window_bytes);
std::println("{}", blake3pp::to_string(d.label));
```

`diagnose_write()` is the write side's equivalent and takes no window
count, because there the arena and the queue depth are one number. Both
report and neither tunes: nothing in them runs a second geometry, predicts
a rate or recommends a number. The readings are
`blake3pp::bottleneck` and the strings `to_string()` returns are the ones
in [Diagnosing a run](tools.md#diagnosing-a-run). Both are API: the
enumerators are appended, never reordered or renamed, and a string changes
only when the value it names changes meaning.

`bands` is the policy each share is read against, and the caller may pass
its own. A figure that lands between the bands gets no label: `label` is
`bottleneck::undecided` and `first` and `second` name the two candidates.
`measured()` says whether anything landed, `long_enough()` whether the run
turned its queue over the four times a reading needs.

`bottleneck::in_flight` is asserted against the test suite's synthetic
device only; that device is the only source known to produce it.

## In the tools

`blake3pp_bench_file` is the worked example: it sizes the storage, prints
a per-window summary and writes the run as Chrome trace JSON.
`blake3ppsum --trace PATH` records the same way over its own path, the
batch of files through `async_update_files`, every file's windows in one
buffer with the file's index on each record. `blake3ppgen --trace PATH`
records the write side on its pipelined path: the same five stamps, read
as admitted, fill begun on an agent, fill done, write landed, released,
which the JSON names `queue`, `fill`, `write` and `release`. Both take
`--trace-agents`. Both also take `--diag`, which reads the same run off
`driver_stats` alone and names the side that was the constraint; see
[Diagnosing a run](tools.md#diagnosing-a-run). See
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

The captures below are from a 16-thread Zen 5 machine (Ryzen AI MAX+ 395)
under WSL2, on its virtual disk and without cache drops, so the device
figures are that disk's rather than an SSD's. The read-limited run's file
was created with `--make` and left to settle for four minutes first. The
shape of the output is the subject.

### A compute-bound run

`--null-source` runs the pipeline with no device under it, so the hash is
the only thing left to be the limit:

```
$ blake3pp_bench_file --null-source 16 --reps 1 --cooldown 0 \
      --trace null.json --trace-agents
null source: 16.00 GiB, window 8 MiB, qd 4, 16 threads
null           67.1 GiB/s   (8390e37432eb3939...)  [digest matches the in-memory hash]
  trace (last rep): 2048 windows (2048 fanned out, 0 dropped), wall 0.238 s
    per window: wait  12.5%  hash 1366.4%  absorb 169.0%  release   0.1%
    agent slices: compress 21125 (3241.7 ms, 99.9%)  fold 2048 (2.4 ms,  0.1%)
    pool occupancy: 85.4% of 16 threads x wall; no agent busy at all for  5.5% of it
    read backend: null
    driver busy   3.0% of its own 0.238 s, 999 iterations, 998 polls (888 blocking, 338 woken), 2048 completions, 0 admission stalls, reducer held at most 20, at most 4 reads and 16 windows in flight
    queue depth: settled at 4 (peak 4); device 0.014 ms per window over 2048 transfers, starved 0.224 s
    median per window: submit->read      2.2 us  read->cv   1637.6 us  cv->reduced     49.7 us
    pool busy  85.3% of 16 threads, 10.3 agents active per fanned window
    io-wq cpu = 0.00 cores (worker threads' cpu / wall); machine (/proc/stat): user 11.39 nice 0.00 sys 0.50 idle 4.03 iowait 0.00 irq 0.00 softirq 0.00 steal 0.00 cores
```

Line by line. All 2048 windows fanned out and none was dropped, so the
storage was sized right. The `hash` share of 1366% is about fourteen
windows' worth of overlap, not an error: each window spent 1.6 ms being
compressed while others did the same. `absorb` overlaps the same way.
`wait` at 12.5% is the source handing back buffers it never read. The
agent slices are compression all but 0.1% of the time, and the pool was
occupied 85.4% of sixteen threads times the wall clock, with no agent at
work for 5.5% of it. The queue depth line says the device was free and
the pipeline was not: 0.014 ms per window against 0.224 s of 0.238
starved, which is the hash behind a source with nothing to do. The driver
was busy 3.0% of its own time, so the loop is not the limit. `pool busy
85.3% of 16 threads` is the number that is: the agents' summed compress
time over threads × wall. The 10.3 agents per window beside it is how
many invocations found parts left to take, out of the sixteen threads the
pool has.

With `--trace-agents`, the same run resolves that per core:

```
    23173 agent records (0 dropped) on 16 cpus:
        cpu  records    parts    busy ms      effective      irqs
          0     1252    59199      179.9     5.02 GiB/s       258
          1     1454    66210      205.1     4.92 GiB/s       240
          ...
         15     1539    65117      198.3     5.01 GiB/s       214
      per-cpu GiB/s: min 4.62, median 4.95, max 5.13; worst max/min part on one agent: 126.5x
      sum of per-cpu rates 78.90 GiB/s; the run delivered 67.09 GiB/s, 85.0% of it
```

The spread of 4.62 to 5.13 GiB/s is one core's compress rate against
another's, and on a machine whose cores are alike that is how it should
look; a single outlier would be the core to look at. The `irqs` column is
each core's interrupt count over the rep, which is how a core that
services the device is told from a slow one; here they are even, because
a null source raises none. The `max/min part` ratio of 126x is one
invocation whose longest part took that much longer than its shortest,
which is a scheduled-out agent rather than a slow core, since that core's
own rate is unremarkable. The last line bounds what is left to win: the
cores together managed 78.90 GiB/s while compressing, the run delivered
85.0% of that, and the missing 15% is everything that is not
compression.

### A read-limited run

The same flags over a real file:

```
$ blake3pp_bench_file blake3pp_bench_file.dat --reps 1 --cooldown 0 \
      --trace file.json --trace-agents
file: blake3pp_bench_file.dat (1024.0 MiB), backend: io_uring+direct, window 8 MiB, qd 4
trace: 129 window records and 2064 agent records, JSON to file.json
cache: not dropped (needs root or elevation); one untimed direct pass first, so every timed pass meets the same cache
raw io         3.62 GiB/s   [device ceiling, no hashing]
...
parallel:   16 threads, affinity mask: 16 cpus
parallel       3.59 GiB/s   (1ec8ab3adeac66e7...)  [ 99% of raw]
  trace (last rep): 128 windows (128 fanned out, 0 dropped), wall 0.279 s
    per window: wait 394.6%  hash  13.1%  absorb   2.1%  release   0.0%
    agent slices: compress 1733 (173.1 ms, 99.9%)  fold 128 (0.2 ms,  0.1%)
    pool occupancy:  3.9% of 16 threads x wall; no agent busy at all for 91.6% of it
    read backend: io_uring+direct
    driver busy   1.3% of its own 0.279 s, 254 iterations, 253 polls (253 blocking, 126 woken), 128 completions, 0 admission stalls, reducer held at most 16, at most 4 reads and 6 windows in flight
    queue depth: settled at 4 (peak 4); device 8.577 ms per window over 128 transfers, starved 0.000 s
    median per window: submit->read   8527.4 us  read->cv    273.7 us  cv->reduced     48.5 us
    pool busy   3.9% of 16 threads, 13.5 agents active per fanned window
    io-wq cpu = 0.04 cores (worker threads' cpu / wall); machine (/proc/stat): user 0.65 nice 0.00 sys 1.79 idle 11.61 iowait 0.75 irq 0.00 softirq 0.00 steal 0.00 cores
    1861 agent records (0 dropped) on 16 cpus:
        cpu  records    parts    busy ms      effective      irqs
          0      120     4407       11.7     5.76 GiB/s      1714
          1      121     3703       10.1     5.57 GiB/s      1613
          ...
         15      125     5535       14.3     5.90 GiB/s      1496
      per-cpu GiB/s: min 5.54, median 5.76, max 6.00; worst max/min part on one agent: 51.7x
      sum of per-cpu rates 92.09 GiB/s; the run delivered 3.59 GiB/s, 3.9% of it
```

Every line of the compute-bound reading inverts. The median window waited
8.5 ms for its read and was compressed in 0.27 ms, `wait` dominates the
shares at four windows' worth of overlap, the pool was occupied 3.9% of
sixteen threads with no agent at work for 91.6% of the wall clock, and the
driver was busy 1.3% of its own time. The run delivered 3.59 GiB/s
against a device ceiling of 3.62, so there is nothing left to win here at
all: the hash is free and the disk is the answer. Nothing in this run is a
hashing problem, and the `--window`/`--qd` sweep (`--io-sweep`) is what
one is handed to next.

The agent table reads differently too. Each core holds about a hundred
records rather than well over a thousand, and the last line says the
cores could have compressed 92.09 GiB/s between them while the run
delivered 3.9% of it. The `irqs` column is populated now, about 1,500 on
every core against about 230 in the null run: the device's completions
are spread across all sixteen cores, so no single core stands apart as
the one servicing the disk.

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
[Perfetto](https://ui.perfetto.dev/) or `chrome://tracing`. The excerpt
below is from `--null-source 1 --trace-agents` on the same machine:

```json
{"epoch_ns":41438975802747,"displayTimeUnit":"ns","pool_threads":16,"traceEvents":[
{"name":"process_name","ph":"M","pid":0,"args":{"name":"blake3pp_bench_file null driver"}},
{"name":"process_name","ph":"M","pid":1,"args":{"name":"blake3pp_bench_file null pipeline, by window object"}},
{"name":"thread_name","ph":"M","pid":0,"tid":0,"args":{"name":"driver loop"}},
{"name":"driver","ph":"X","pid":0,"tid":0,"ts":138850.180,"dur":17261.423,"args":{"busy_ns":1274040,"parked_ns":15981990,"iterations":70,"polls":69,"blocking_polls":58,"wakes":25,"queue_runs":128,"admission_stalls":0,"max_pending":17,"transfers_landed":128,"transfer_bytes":1073741824,"device_wait_ns":3599735,"starved_ns":15287607,"queue_depth_final":4,"queue_depth_peak":4,"agent_busy_ns":222153665,"device_wait_min_ns":452,"device_depth":0,"lane_wait_ns":0,"read_backend":"null"}},
{"name":"thread_name","ph":"M","pid":1,"tid":0,"args":{"name":"window object"}},
{"name":"wait","ph":"X","pid":1,"tid":0,"ts":138850.180,"dur":15.828,"args":{"window":0,"file":0,"slot":0,"bytes":8388608,"last":0,"parallel":1}},
{"name":"hash","ph":"X","pid":1,"tid":0,"ts":138866.008,"dur":3343.378,"args":{"window":0,"file":0,"slot":0,"bytes":8388608,"last":0,"parallel":1}},
{"name":"process_name","ph":"M","pid":2,"args":{"name":"blake3pp_bench_file null pool agents, by cpu"}},
{"name":"thread_name","ph":"M","pid":2,"tid":15,"args":{"name":"cpu"}},
{"name":"compress","ph":"X","pid":2,"tid":15,"ts":139241.254,"dur":160.643,"args":{"window":3,"file":0,"parts":50,"min_part_ns":2269,"max_part_ns":16169}},
{"name":"compress","cat":"window","ph":"s","id":1,"pid":1,"tid":3,"ts":138913.973},
{"name":"compress","cat":"window","ph":"f","bp":"e","id":1,"pid":2,"tid":15,"ts":139241.255},
{"name":"agents busy","ph":"C","pid":2,"tid":0,"ts":139150.538,"args":{"busy":1}},
```

| process | track | holds |
|---|---|---|
| `pid` 0, "driver" | `tid` 0, "driver loop" | one event over the whole run, carrying `driver_stats` in `args` |
| `pid` 1, "pipeline, by window object" | `tid` = `slot`, "window object" | the `wait`, `hash`, `absorb` and `release` events of every window that object carried |
| `pid` 2, "pool agents, by cpu" | `tid` = cpu (99 unknown), "cpu" | one `compress`, `fold` or `fill` event per agent record, with its parts and part extremes, and an "agents busy" counter |

Windows go on the track of the window object that carried them rather
than on one shared track, so several in flight read as parallel lanes
instead of as one track nesting into itself. Each agent slice has a flow
arrow from its window's compute phase (`"ph":"s"` on the window object's
track, `"ph":"f"` on the cpu's), so selecting either end highlights the
other. The "agents busy" counter is the number of agents at work at
each change point, which shows a starved pool as such rather than as
empty lanes. `ts` and `dur` are microseconds, as the format wants.

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
