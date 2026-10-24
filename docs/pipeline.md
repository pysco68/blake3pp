# Inside the pipeline

[← blake3pp](../README.md)

`hash_file(path, sched)` is a `std::execution` program. The file is cut
into fixed windows, each window is one sender chain, several chains are in
flight at once, and one thread -- the one that made the call -- drives all
of them. What follows is that chain stage by stage, why each stage sits
where it does, and the rules that let completions arrive in whatever order
the device and the pool produce them with no lock on the hash.

The same engine runs `update_file()`, the scheduler-taking `hash()` over a
buffer in memory, `write_file()` and the senders in
[`<blake3pp/async_io.hpp>`](async.md). What differs between them is the
driver underneath and who owns the window storage.

## The shape of one window

```
read(window)                 // queued on the driver, completes inside poll()
  | let_value(compress)      // bulk over the window's parts, on your scheduler
  | continues_on(driver)     // back to the driver's thread
  | then(reduce_into_tree)   // order-free
```

Four stages, and the window's buffer, its part table and its connected
operation state are all allocated before the run starts and reused for
every window that passes through them.

### read

`read` is a sender over the fastest mechanism the platform and the
filesystem allow -- io_uring, IoRing or IOCP, GCD, POSIX AIO on a
kqueue, or a synchronous `pread` -- behind
one driver interface. Its `start()` queues the transfer and returns. It does
not flush, because the driver loop flushes once per round and a whole
batch of windows then reaches the OS in one call; and it does not
complete, because the backends call back only from inside `poll()`.

The value completion carries the window's bytes: the backends read exactly
what was asked for or report an error, so a window that completes is whole.
A transfer the device could not even accept, and one skipped because stop
had already been requested, park on the run queue and are delivered from
the loop like every other completion.

### compress

`let_value`, not `then`, because the compress stage cannot be built until
the read has landed: it needs the window's bytes and its plan. Inside it,
the stage is

```
schedule(sched) | bulk(par, parts, compress_one_part) | then(fold)
```

Starting that only enqueues onto the caller's scheduler. Every part, and
the fold behind the bulk, then run on an agent of that scheduler, so the
driver's share of a window's compute is one enqueue. With a trace buffer
set, `driver_stats::busy_ns` against `parked_ns` is where a provider that
ran the bulk on the starting thread would show up instead. On the captures in
[Performance tracing](tracing.md) the driver is busy 0.6% of its own time on
a read-limited file run, and 4.0% on a compute-bound one whose 32 pool
threads were busy 79%.

The bulk's shape only supplies the agents. Each invocation pulls part
indices from one atomic counter until none are left, so the split follows
each agent's actual speed rather than the fixed shares a provider's bulk
hands out, and every index runs exactly once however the implementation
distributes the calls.

A part is at least 16 chunks, and a full window's parts are reduced in
groups of 16 *inside* the bulk, by the agent whose part completes a group.
A 16-chunk part reduced on its own is 16 lane-filling chunk compressions
followed by four parent generations at 8, 4, 2 and 1 of a 16-lane kernel's
lanes; grouping the parts keeps those generations filled down to the last
`log2(degree)`. It measured +13.8% on Emerald Rapids and +2.8% on Zen 5 at
8 MiB windows, and nothing at 64 MiB
([Performance](performance.md)). Moving the reduction behind the bulk
instead would put a sixteenth of the window's blocks on one thread while
the other agents have nothing left to do.

The fold after the bulk turns the group chaining values into the window's
one chaining value, on whichever agent finished the bulk. The driver thread
never touches it.

### continues_on

The pool thread that finished the window may not touch the driver, so it
parks an intrusive node on the driver's run queue and wakes the driver.
That is the whole hand-off, and it is the last thing that operation state
does: publishing the node is what allows the driver to run the
continuation, re-emplace the cell the node lived in, and finish the file.

The run queue is the only shared structure in the pipeline -- many
producers, one consumer. Everything else belongs to the driver thread. A
drain reverses the pushed stack once so windows come back in the order they
finished rather than inverted, which is what makes a trace readable.

### reduce_into_tree

On the driver again, because the reducer and the hasher are
single-threaded by contract. The window's chaining value goes into the
[tree reducer](#the-tree-reducer) as the subtree over its chunk range.
Nothing here waits for the window to the left of it.

## Windows that are not subtrees

A full window is a power-of-two number of chunks at an aligned chunk
offset, which makes it a node of BLAKE3's final tree. Two windows of a run
are usually not:

- **The last window** carries the message's final chunk. BLAKE3 marks one
  node of the tree as the root and which node that is depends on where the
  message ends, so a merged range must never contain the final chunk. The
  whole parts that sit entirely before it go to the pool; the part holding
  it, and everything after, stays with the hasher and is absorbed last.
- **A short first window** appears when the hasher is already part-way
  through a message -- the second file of several streamed into one hasher.
  Its front, under one part in total, is absorbed by the hasher on the
  driver the moment the read lands, which is what puts the hasher on the
  boundary the nodes are numbered from. What follows is whole parts aligned
  in the message's own chunk space, like any other window's.

An edge window with at least two whole parts therefore runs the same chain,
but what comes back is not one chaining value: it is the canonical
decomposition of its run of parts into reducer nodes. Grouping is by
absolute position rather than by count -- a group of 2^j parts is a node of
the tree only when its first part's absolute index is a multiple of 2^j --
so taking the largest legal group at each step leaves at most one ascending
and one descending chain of nodes, one per level.

An edge window with fewer than two whole parts worth fanning out, and a
file smaller than one window, take a third shape: read it, hold it, and let
the hasher take the whole thing -- a read and a `then`, with no trip to the
pool.

Each shape is a separate chain builder, so the full window's path -- the
one every window of a large file takes -- carries no branch for the other
two.

## The rules

Four rules hold the shape together, each of them about where a completion
may run and who may touch what afterwards. A fifth covers how a run ends,
and it is why the stop path looks the way it does.

### One thread drives

Every `submit_read`, every flush and every `poll()` happens on the thread
inside the driver loop. `wake()` is the only member of a driver another
thread may call.

This is the contract the I/O backends are written to, and it is the reason
a scheduler exists in the chain at all: a pool thread cannot touch the
driver, so it parks a node and nudges the driver awake instead. The loop's
`get_forward_progress_guarantee` is `weakly_parallel`, which is the honest
answer for a scheduler that advances only while its owner is inside the
loop.

### A transfer's callback runs only inside `poll()`

A backend never calls back from `submit_read`, from a flush, or from
another thread. The driver thread is therefore the only thread running
completions, and a blocking `poll()` is the only place it sleeps.

The in-memory driver obeys this even though it has nothing to wait for: a
read of bytes already in memory is remembered and reported from `poll()`.
The reason is the next rule.

### Nothing completes inside a `start()`

The scope starts the next window from inside its own receiver. A sender
that completed its receiver from `start()` would run that receiver inside
itself, recursing through the scope's bookkeeping while an earlier
completion is still halfway through it, to a depth set by how many windows
happen to be ready at once.

So a read that cannot even be queued parks its failure on the run queue and
is completed from the loop, like everything else. This rule is why the
in-memory driver keeps a queue it does not otherwise need.

### After a push, the publisher touches only the loop

The push is the hand-off. From that instant the driver may run the
continuation, re-emplace the operation cell the node lived in, finish the
file and return to a caller that destroys everything. Only the loop itself
may be touched afterwards -- its publisher count and its driver's wake --
and the loop is what waits those threads out before it returns.
ThreadSanitizer is the only thing that catches a violation of this one.

### A run completes exactly once

A run completes exactly once: with the file absorbed, with the first
failure a window reported, or stopped. Stop is one `inplace_stop_source`
per run, handed to every stage through the receivers' environment. Asking
the device to abandon a read is a separate, advisory step that completes
nothing: a cancelled read is still owed its callback and still holds its
buffer, so nothing is destroyed while an operation is owed.

Failure and stop race by construction, since requesting a stop is what
makes the other windows finish and one of them may fail on the way out. A
latch decides: the first failure is the one the caller hears about, and a
failure outranks the stop it caused. Later ones are counted and dropped.

## The tree reducer

`hasher::push_subtree_cv()` takes complete subtrees strictly left to right
-- it is the binary counter of the BLAKE3 spec, and a subtree that arrives
early has nowhere to wait. A pipeline whose windows finish out of order has
to re-serialize them before absorbing, and the reducer is where that
happens: it holds pending nodes, merges each pair of siblings as soon as
both exist, and hands what is left to the hasher in order at the end.

### Why a window may be absorbed early

BLAKE3's tree is left-complete. Any chunk range `[p, p + s)` with `s` a
power of two, `p % s == 0`, and `p + s` at or below the message's final
chunk is a node of the final tree, whatever its siblings are doing. Two
pending nodes `(p, s)` and `(p + s, s)` with `p % 2s == 0` are then the two
children of `(p, 2s)`, which is a node of the tree by the same argument,
and compressing them is the same compression the hasher would have
performed later.

The digest is therefore identical to the sequential one, by construction
rather than by approximation. The final chunk is the single exception, which
is why the last window's tail belongs to the hasher.

### What it costs

It never allocates. The node storage is the caller's, a fixed array, which
is what bounds how many unmerged nodes may be in flight; the sibling of
`(p, s)` is at `p ^ s` with the same size, so a lookup is a linear scan of
tens of entries, and each merge frees the slot it came from, which is why a
cascade never needs storage of its own. `drain_into()` sorts by first chunk
with an insertion sort, because the array is nearly ordered whenever the
windows arrived nearly in order.

The compression work is the work the hasher would have done anyway. What
the reducer adds is the holding: 32 bytes and two integers per pending
node, and a parent compression charged to the driver thread rather than to
a pool agent.

How many nodes can be pending is bounded, not by the insertion order in
general -- an arbitrary order merges nothing, so the storage would have to
hold one node per inserted range -- but by the fact that reads start in file
order. Each contiguous segment of inserted ranges decomposes into at most
one ascending and one descending chain of aligned nodes, one per level, and
a message at BLAKE3's 2^64-byte limit is 54 levels deep. The pending count
is therefore that bound times the number of segments the holes leave.

## Admission, and the two bounds

A window may start only when three things are true: a window object is
free, fewer than the plan's reads are still reading, and the reducer has
room for everything this window will insert.

The first two bounds are separate on purpose. The device wants a few large
reads in flight; the pool wants windows to work on while the device serves
the next ones. One number cannot serve both, so `queue_depth` is what the
device sees and a window budget is what buys the windows behind it.

On a fast device the budget is the larger of the two levers. Measured on
the four-drive PCIe 5 stripe (32 threads, 64 GiB file, cold, the depth
held at four reads so the two axes are separate):

| window | arena | GiB/s | pool busy |
| --- | --- | --- | --- |
| 64 MiB | 256 MiB (4 windows) | 37.0 | 64% |
| 64 MiB | 384 MiB (6 windows) | 41.5 | 76% |
| 64 MiB | 512 MiB (8 windows) | 45.9 | 86% |
| 64 MiB | 1 GiB (16 windows) | 45.6 | 85% |
| 64 MiB | 2 GiB (32 windows) | 44.4 | 85% |
| 8 MiB | 32 MiB (4 windows) | 30.2 | 53% |
| 8 MiB | 64 MiB (8 windows) | 37.6 | 71% |
| 8 MiB | 128 MiB (16 windows) | 38.9 | 75% |
| 8 MiB | 256 MiB (32 windows) | 39.3 | 75% |
| 8 MiB | 1 GiB (128 windows) | 38.9 | 76% |
| 8 MiB | 2 GiB (256 windows) | 38.5 | 74% |

Each figure is the mean of a forward and a backward pass of the ladder.
Moving four reads to sixteen inside a 1 GiB arena, by contrast, is worth
1.4 GiB/s -- inside the repeatability floor. The pool-busy column is the
mechanism: with only the reads to work on, the hash pool stands idle for a
third of its 32 threads waiting for the device.

The return flattens at a window count rather than at a number of bytes --
eight windows at 64 MiB, sixteen at 8 MiB -- and that is what a byte
budget alone gets wrong: divided by a 64 MiB window, 128 MiB buys four
windows, 19% below eight. Narrowing the pool shows whose appetite it is. On
sixteen threads the same 8 MiB row saturates at eight windows and gains
nothing above it, the pool sitting at 98%, where 32 threads stand at 54%
on four windows and keep gaining to sixteen.

So the count follows the pool it feeds: one window per four hardware
threads, floored at 128 MiB and capped at 2 GiB, past which the stripe's
figure declines. A device that is already
saturated gains nothing from any of it, and on one of those the arena is
memory spent for no return.

The third bound is why the pipeline cannot reach a state where no window
may start. A full window reserves the one node it will insert; an edge
window reserves the whole decomposition of its run, since a reservation
of one against an insert of a hundred is the refusal this rule prevents.
Since reads start in file order, when nothing is in flight the windows that
have finished are a contiguous run from the beginning, which decomposes into
at most `2 * 54` nodes. A run's storage is sized for exactly that, plus both
edge windows' reservations, plus one node per window that may be in flight
-- windows complete out of order, and each can leave a node waiting for a
sibling -- with 256 nodes as the floor, so the deepest queue a caller asks
for cannot quietly admit fewer windows than it asked for. A run that is held back anyway counts the
event in `driver_stats::admission_stalls`; a configuration that could never
start a window at all is reported as a `std::logic_error` naming every
number involved rather than entered.

## Declared completions

`let_value` has to know how its child completes, and it asks without an
environment. A chain containing `schedule()` on a pool, or a provider's
`bulk`, cannot answer that question: the adaptors would be asked in turn,
with no environment, and fail to compile. Connecting asks with the real
environment, which is where the child is happy to answer.

So the compress stage is wrapped in an adaptor that *declares* its
completions instead of asking its child: what a scheduler and a bulk of
`noexcept` bodies can produce, with a channel the child never sends costing
nothing.

The same adaptor carries the stop token, and it is where an arriving value
becomes a stop. A stage whose agents stopped part-way folded a chaining
value from parts that were never compressed, and the reducer must not take
it. Only the value channel is intercepted -- an error still reaches the
scope as an error, which is what lets a failure outrank the stop it caused
-- and the token is read when the value arrives, after the stage's own work
has finished, so a stop requested while the agents were running is still
seen.

## The write direction

`write_file()` is the same chain read backwards:

```
schedule(sched)
  | then(fill)               // the caller's generator, on the pool
  | continues_on(driver)     // back to the driver's thread
  | let_value(write(window)) // queued on the driver, completes inside poll()
  | then(release)
```

Same driver loop, same rules, same read/write sender shape -- from the
pipeline's side a read and a write are one operation: queue it, do not
complete inside `start()`, take whatever `poll()` reports. The direction
decides the buffer's constness, which driver call submits it, and whether
the value completion carries bytes back.

One window's fill is sequential, so on this side the windows in flight are
the whole parallelism: the queue depth is the lever, and its default is
automatic -- one window per hardware thread over a scheduler, bounded so
that depth times window stays within 256 MiB in flight, and two in the
sequential forms ([Performance](performance.md)).

What the write side does not have is a reducer. A window's bytes depend on
its offset and nothing else, so there is no tree to rebuild and no last
window to keep for the end. The generator must therefore depend on its
offset alone in the scheduler-taking form, where windows are filled
concurrently and in no particular order.

The generator may throw, even on a pool thread inside a window already
counted in flight: nothing is submitted for that window before the fill, so
the exception owes the driver nothing and travels the chain's own error
channel -- `set_error(exception_ptr)` to the scope, which stops the run,
gives the window back and rethrows from `write_file()`.

Three public forms: a known length generated on the calling thread, a length
the generator decides by returning short of a whole window, and the windows
filled over a scheduler while the previous one is written. The first and the
third call `fill(std::span<std::byte> out, std::uint64_t offset)`; the
length-deciding one returns the bytes it produced, `std::size_t
fill(std::span<std::byte> out, std::uint64_t offset)`.
`blake3ppgen --output` is built on them. On failure the file is left as far
as it got and is not truncated, because a length invented for bytes that
were never produced would hide the failure.

## Window storage, and who owns it

A window is a buffer, a part table and a connected operation cell. The
buffers are one arena, aligned for direct I/O, taken once per driver and
valid until the driver is destroyed -- and the driver drains before it goes,
so a read is never in flight into memory that has already been freed. There
is no free, and a second reserve is a contract violation rather than a
reallocation, for the same reason.

That arena belongs to the driver rather than to the run, and the run borrows
windows from a pool over it. That is the whole difference between the two
layers that use senders:

- The **blocking entry points** (`hash_file`, `update_file`, `write_file`)
  build a driver, an arena and a loop for the one file they are hashing, and
  give them up when they return. One allocation per call, whose pages fault
  in on the first pass over it, and no two files ever overlap. For one large
  file that is the right shape, and it is not slower.
- **`<blake3pp/async_io.hpp>`** moves the storage up into an `io_context`
  that owns the driver, the arena and the run queue, and turns the call into
  a sender. Several files borrow windows from one arena at once, which is
  most of the win on a directory of small files. One loop per context,
  because the loop *is* the thread that may touch the driver, and each
  started operation is offered a round in turn. [Many files at
  once](async.md) has the run model and the lifetime rules.

A window held past its completion is what turns a shared arena into a
deadlock: with more runs than windows, each would keep one and none could
start. So a run gives every window back before it reports, on every path --
including the path where a `poll()` threw, which first waits out every chain
that is still on the pool, because a pool thread is about to publish into
an operation cell that is about to be destroyed.

Two allocations complete the picture: the reducer's node array, and the part
tables, which live with the windows. A full window's grouped reduction also
keeps a second table of `parts x simd_degree` chaining values, grown on the
first window that needs the size and kept. Sizes and the `stack_budget` knob
behind them are in [the API](api.md#the-part-table-and-stack).

The third file layer has no chain at all. `<blake3pp/io.hpp>` names no
execution provider, so without a scheduler there is nothing to fan a window
out over: the windowed reader delivers windows strictly in file order, the
hasher absorbs each one inline, and releasing a buffer is the backpressure.
No run queue, no reducer, no driver loop.

## The same chain over memory

The pipeline is a template over a driver concept, so the scheduler-taking
`hash(buffer, sched)` is the same four stages over a driver whose windows
are *views* of the caller's bytes. Nothing is allocated, nothing is copied,
and a read is the act of noticing that the bytes are already there --
reported from `poll()`, because nothing completes inside a `start()`. So the
tree reduction, the edge-window decomposition and the final-chunk discipline
are one implementation for files and for buffers, and a window folds
identically whichever path ran it.

Below `min_pipeline_bytes` the pipeline is not entered at all; the bytes go
straight into the hasher, and a file of that size is opened through the page
cache whatever `direct_io` asked for, since buffered reads are the faster
ones there ([Direct I/O](direct-io.md)).

## Where the time went

`<blake3pp/trace.hpp>` is the pipeline's own instrument. Set a
`trace_buffer` in the options and `update_file()` fills one
`window_record` per window -- `t_wait_begin`, `t_ready`, `t_joined`,
`t_absorbed`, `t_released`, the agents' summed compress time and how many
took part -- plus, on request, one `agent_record` per bulk invocation. The
library records; it never allocates, formats or writes. Storage that runs
out costs a counted drop, never a stall.

`driver_stats` is the thread's own split of busy against parked, with the
counts that say *why*: blocking polls, wakes that arrived with no
completion of the driver's own, run-queue completions, admission stalls,
and the high-water marks for reads in flight, windows in flight and pending
reducer nodes. Those are the numbers that answer whether the driver, the
device or the pool is the bottleneck. Tracing is a template parameter
settled once per run, so an untraced run carries one branch per window and
no branch inside the bulk.

## Where to go next

| | |
|---|---|
| [The API](api.md) | the entry points, the options and what each one costs |
| [Many files at once](async.md) | the same pipeline as senders over a context you drive |
| [Performance](performance.md) | the measurements, per machine, with the runs behind them |
| [Architectures and kernels](architectures.md) | what the compress stage actually runs |
