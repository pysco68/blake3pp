# Direct I/O

[← blake3pp](../README.md)

`file_io_options::direct_io` and `file_write_options::direct_io`
(`<blake3pp/io.hpp>`, both default `true`) ask the file pipeline to
bypass the OS page cache: every read or write goes straight to the
device instead of through buffered pages. [The API](api.md#from-memory-or-from-a-file)
covers where the option sits among the pipeline's other knobs; this page
is what the request means on each platform, what it demands of the
caller, and when to turn it off.

## What the option asks for

| OS | async engine | cache-bypass flag |
|----|--------------|--------------------|
| Linux | io_uring | `O_DIRECT` |
| Windows | IOCP | `FILE_FLAG_NO_BUFFERING` |
| macOS | GCD (libdispatch) | `F_NOCACHE` |

Each is a per-open property. `src/io/posix_file.hpp` engages `O_DIRECT`
by reopening the path a second time with the flag set, and keeps the
plain, always-buffered descriptor from the first open beside it for
whatever the direct one cannot carry. Darwin has no `O_DIRECT`;
`src/io/gcd_backend.hpp` instead flips `F_NOCACHE` on the single
descriptor with `fcntl`, per-fd rather than per-open. Windows carries two
independent per-open flags, `FILE_FLAG_NO_BUFFERING` and
`FILE_FLAG_OVERLAPPED` (the port); `src/io/iocp_backend.hpp` asks for
both together on the first reopen and, if that is refused, asks again
for the port alone before falling back further. The port is kept on its
own deliberately: `NO_BUFFERING` is usually what a filesystem (SMB
shares, some virtual volumes) actually rejects, and buffered-but-async
reads measure within noise of direct-and-async ones on Windows, so
giving up both together for a refusal that only concerned one of them
would discard the half that still works.

Bypassing the cache means every transfer does real device work: a page
the kernel already held will not save a second trip, and a drive earning
its number has to actually move the bytes.

## The alignment it demands, and how the library meets it

`O_DIRECT` and `FILE_FLAG_NO_BUFFERING` both reject a transfer unless
the buffer address, the length and the file offset are all multiples of
one granule. `src/io/backend.hpp` fixes that granule at 4096 bytes
(`direct_align`), the common sector and page size on the platforms this
applies to. `F_NOCACHE` carries no such contract: macOS serves a
misaligned edge through the cache transparently instead of rejecting it,
which is why the GCD backend needs none of the machinery below.

Two things in the pipeline exist to meet the granule without the caller
having to think about it:

- **The arena.** The buffer pool behind every reader and writer is one
  allocation made with `operator new(bytes, std::align_val_t{direct_align})`,
  so every window buffer starts aligned regardless of what size was
  requested.
- **Window size.** Reading, and writing over a scheduler, round
  `window_bytes` to a power of two between 64 KiB and 1 GiB, which keeps
  every full window a multiple of the granule without per-call
  arithmetic. The single-threaded `write_file()` forms instead round up
  to a plain multiple of 4 KiB (the same minimum and maximum) rather than
  a power of two -- a different geometry for the same options struct, but
  one that still satisfies the granule, because 4 KiB is exactly it.

What a power-of-two window cannot fix is the tail: a file whose length
is not a whole number of granules, or the last short write of a
generator that stops mid-window. On Linux, every read and write first
passes `may_ride()`, the one gate in `src/io/uring_backend.hpp` that
decides whether a transfer -- the initial one, or what is left after a
short completion -- may go on the ring at all: it refuses a length that
is not a multiple of the granule, and, once direct I/O is engaged,
refuses an offset that is not one either. Whatever it refuses goes on a
deferred list instead, and `run_one_deferred()` walks that list
synchronously, one entry per `poll()` call, through the plain buffered
descriptor the direct one sits beside -- picking the transfer up exactly
where the ring left off. Nothing above the backend can tell a window's
bytes arrived whole on the ring from arriving mostly on the ring and
finishing synchronously; both land in the same buffer. Windows' `place()`
and its own deferred list apply the identical ladder, for the identical
reason (`NO_BUFFERING` rejects the same shapes `O_DIRECT` does). GCD
needs no deferred list for alignment at all, because `F_NOCACHE` imposes
none -- its deferred list exists only for the separate case of GCD itself
being unavailable.

## Degradation is visible, never silent

A file a filesystem will not open for direct I/O -- tmpfs, overlayfs,
some network and virtual filesystems -- is read or written buffered
instead, automatically: asking for `direct_io = true` and having it
refused is not an error, and nothing throws because the device could not
be bypassed. That is as far as "automatic" goes, though. A caller unable
to tell afterward which path actually ran is not how this library
behaves.

Every reader and writer reports, after the fact, exactly which rung it
landed on:

- `file_reader::backend()` returns a `std::string_view`: `"io_uring+direct"`,
  `"iocp+direct"`, `"gcd+nocache"`, `"pread"`, `"readfile"`, or `"stdio"`.
- `write_report::backend` names the write side the same way:
  `"io_uring+direct"`, `"iocp+direct"`, `"gcd-write+nocache"`, `"pwrite"`,
  `"writefile"`, or `"stdio"`.

`"stdio"` is not a rung anything degrades to: it is the fixed answer on
platforms with none of the three async engines (wasm), the same way it
is the answer on no other platform. `" (inline submit)"` is appended on
Linux when `offload_submit` was turned off. When the async engine itself
was asked for and refused, not just the cache bypass -- a kernel too old
for io_uring, or io_uring blocked by seccomp policy -- the Linux backend
appends why: `" [io_uring: EPERM, blocked by policy]"` or
`" [io_uring: ENOSYS, kernel too old]"`. A benchmark or a bug report
therefore reads differently depending on which rung actually ran, never
only the one that was asked for.

## When direct I/O is the wrong choice

Bypassing the cache is a bet that this read or write happens once. The
bet stops paying off, or actively costs, in a few shapes:

- **A file that will be read again soon.** Direct I/O denies a second
  read the free hit a warm page cache would have given it. The
  benchmark's own cold-cache discipline (`bench/file_throughput.cpp`)
  depends on exactly this effect in reverse: without dropping the cache
  before every repetition, a `--no-direct` run's first repetition warms
  the cache and every later one reports RAM speed, not device speed. A
  workload that revisits the same bytes wants that warming, which direct
  I/O exists to refuse.
- **A small file.** The extra reopen for the flag, and the granule
  bookkeeping around it, are a fixed cost per file; a long transfer
  amortizes it and a short one does not.
- **A filesystem that refuses it.** Network filesystems, overlay and
  virtual filesystems, and some container volumes decline `O_DIRECT` /
  `FILE_FLAG_NO_BUFFERING` outright. The degradation above makes this
  work correctly, but what runs afterward is buffered I/O with an extra
  refused reopen paid for nothing.
- **Linux kernels before 7.0, specifically.** Measured on a four-drive
  PCIe 5 NVMe stripe through this pipeline: kernel 6.8 read 3.84 GiB/s
  buffered against 6.79 GiB/s direct, with the reading thread spending
  72% of wall time inside the kernel's own buffered-read copy path
  rather than in this library's code; the same device on kernel 7.0 read
  6.03 GiB/s buffered, matching its own direct throughput, with that
  copy cost down to about two cores of io-wq work. Windows and macOS
  show no such gap on any machine measured -- their buffered reads
  measure the same as their direct ones. The pre-7.0 Linux gap is the
  kernel's buffered-read path, not this library's; whether it still
  applies to every kernel still in use is an open question.

None of this argues for defaulting `direct_io` to `false` -- it is why
the option exists rather than being assumed, and why turning it off is
one flag away rather than a rebuild.

## How to tell what you got

In code, read `backend()` on the reader or `write_report::backend` after
a write -- the exact strings are in "Degradation is visible, never
silent" above.

From the command line:

- `blake3ppsum` has no flag that prints its backend; `--no-direct` turns
  the request off (`--qd`/`--window` pick the rest of the pipeline's
  shape).
- `blake3ppgen --output FILE -v` reports the engaged write backend on
  stderr; `--no-direct` (needs `--output`) keeps the page cache instead.
- `blake3pp_bench_file FILE` always prints the engaged backend in its
  header line; `--no-direct` drives the same pipeline buffered, which is
  how the kernel gap above gets measured in the first place.
