# Many files at once

**Experimental.** `<blake3pp/async_io.hpp>` is the newest of the three
file layers and the only one whose surface may still move. What is
labelled experimental about it is named at the bottom of this page; the
run model and the lifetime rules are the parts meant to stay.

## Three layers, and when each is right

```cpp
#include <blake3pp/io.hpp>          // sequential, standard library only
#include <blake3pp/parallel_io.hpp> // one file, over a scheduler
#include <blake3pp/async_io.hpp>    // many files, over a context
```

`io.hpp` reads a file through a windowed pipeline on the calling thread
and hashes it there. It needs no execution provider at all.

`parallel_io.hpp` adds the scheduler: complete windows go out as
subtrees and come back as chaining values, so a file is hashed at
storage speed rather than core speed. It blocks, and it owns everything
it needs for the one file it is hashing — a driver, an arena of windows,
the thread that called it.

`async_io.hpp` moves that storage up into a context and turns the call
into a sender. Reach for it when either is true:

- **You have many files.** The blocking form allocates and first-touches
  an arena per call, and no two files ever overlap. A context allocates
  once and lets several files share it.
- **You have work of your own.** A sender composes: `then`, `let_value`,
  `when_all`, a stop token from your own receiver.

For one large file, `hash_file` is still the right call and is not
slower.

## The shape

```cpp
#include <blake3pp/async_io.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

blake3pp::async::io_context ctx;
exec::static_thread_pool pool{std::thread::hardware_concurrency()};
auto sched = pool.get_scheduler();

stdexec::inplace_stop_source stop;

auto work = stdexec::when_all(
    blake3pp::async::async_hash_file(ctx, "a.bin", sched),
    blake3pp::async::async_hash_file(ctx, "b.bin", sched),
    blake3pp::async::async_hash_file(ctx, "c.bin", sched));

auto [a, b, c] = blake3pp::async::sync_wait(ctx, std::move(work)).value();
```

`sync_wait(ctx, sender)` starts the sender and drives the context on the
calling thread until it completes: the values come back as a tuple, a
stop as an empty optional, an error rethrown (an `error_code` as
`std::system_error`). Any sender with one value completion works,
composed or not. Connecting and starting by hand, then
`ctx.run_until_idle()`, is the same thing with a receiver of your own.

Two senders, matching the two blocking entry points, and a third over
many files:

| sender | completes with | the hasher |
|---|---|---|
| `async_hash_file(ctx, path, sched, opts)` | `set_value(digest)` | its own |
| `async_update_file(ctx, h, path, sched, opts)` | `set_value()` | yours, streamed into |
| `async_update_files(ctx, paths, sched, opts, make, done)` | `set_value()` once | one per file, from `make` |

The first two also complete with `set_error(std::error_code)`,
`set_error(std::exception_ptr)` or `set_stopped()`, and never with more
than one of the four.

`async_update_file` is the primitive: the hasher keeps its mode, so
keyed and `derive_key` hashers work exactly as they do everywhere else,
and several files can go into one hasher in sequence.

`async_update_files` runs a bounded number of files at once (the
context's windows divided by its queue depth, or `opts.in_flight`) and
refills a slot as each file ends. Every file's end goes to
`done(index, hasher&, file_outcome)` on the context's thread, with the
hasher that streamed it, so the caller finalizes to whatever length and
mode it wants; a file that fails is an outcome, not the end of the run.
The sender completes once, after the last outcome, or with
`set_stopped()` when a stop ended it. This is what `blake3ppsum` runs
over a list of files:

```cpp
auto work = blake3pp::async::async_update_files(
    ctx, paths, sched, {.io = io_opts},
    [&](std::size_t) { return blake3pp::make_hasher(io_opts); },
    [&](std::size_t i, blake3pp::hasher& h, const blake3pp::async::file_outcome& o) noexcept {
      if (o.ok()) print(i, h.finalize()); else report(i, o);
    });
blake3pp::async::sync_wait(ctx, std::move(work));
```

## The run model

One thread runs the context. It is the thread that touches the driver,
runs every window's completion, and invokes every receiver.

```cpp
void run();                        // until request_stop() and nothing is owed
void run_until_idle();             // until nothing is in flight
void run_until(const bool& done);  // until a completion on this thread sets it
void request_stop();               // from any thread
```

- `run()` with nothing started **blocks** until `request_stop()`. That is
  what a thread dedicated to the context wants. A thread with other work
  should call `run_until_idle()` instead, which returns as soon as
  nothing is in flight — including immediately, when nothing was.
- Only one thread may run a context. A second is a contract violation
  and debug builds assert on it.
- Any thread may start an operation on the context: `start()` only joins
  it, and the open and the first reads happen on the running thread in
  the operation's first round.
- `request_stop()` is the other member another thread may call. It asks
  every started operation to stop; each one then completes with
  `set_stopped()`, or with its value if it had already finished. The
  request clears when the run call returns, so a context is reusable.
- An operation completes **inside** a run call, never inside `start()`.
  A file that cannot even be opened is reported from the first round,
  not from the `start()` that found it.

## Lifetimes

The rules are the ones P2300 already implies, written out because a
context makes them easy to get wrong:

- A started operation must not outlive its context, its scheduler, or —
  for `async_update_file` — its hasher and path.
- A context with a started, uncompleted operation must not be destroyed.
  Debug builds assert on it. Destroying one whose operations have all
  completed is fine, and so is starting more on it afterwards.
- The hasher an `async_update_file` names is that operation's alone
  until it completes. Nothing else may read or write it in between.

## What the context costs

```cpp
blake3pp::async::io_context ctx({
    .window_bytes = 8 * 1024 * 1024,  // per window
    .queue_depth  = 4,                // windows ONE operation may hold
    .windows      = 16,               // windows in the shared arena
    .direct_io    = true});
```

The arena is `windows * window_bytes`, allocated and first-touched once.
Left at zero, `windows` is what the plan asks for: 16 at the defaults, so a
128 MiB arena, which keeps four operations of `queue_depth` 4 moving. A
context meant to keep *n* files moving wants around `n * queue_depth`
windows, and `arena_bytes` is the same request as a memory budget, which is
usually the number a caller actually has.

Fewer windows than that is not a deadlock — an operation that finds none
free waits, and resumes when any other gives one back — but it
does serialise the files.

The part-table budget is the context's rather than the call's, because
the context owns the tables:

```cpp
blake3pp::async::basic_io_context<blake3pp::stack_budget{8 * 1024}> small(opts);
```

`blake3pp::async::io_context` is that template with the default budget.

## What is experimental

Two things may still change:

- **The error channels.** Today a failed read arrives as
  `set_error(std::error_code)` and anything thrown inside the pipeline as
  `set_error(std::exception_ptr)`. Collapsing those into one channel is
  still open.
- **The context option names.** `windows` describes the arena in units
  the caller has to compute; `arena_bytes` is the byte budget that
  derives it, and may become the only spelling.

The sender signatures, the run model and the lifetime rules above are
not expected to move.
