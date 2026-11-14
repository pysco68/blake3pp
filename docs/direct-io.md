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
| Windows | IoRing (Windows 11) or IOCP | `FILE_FLAG_NO_BUFFERING` |
| macOS | GCD (libdispatch) | `F_NOCACHE` |
| FreeBSD | POSIX AIO (`lio_listio`) on a kqueue | `O_DIRECT` |

Each is a per-open property. `src/io/posix_file.hpp` engages `O_DIRECT`
by reopening the path a second time with the flag set, and keeps the
plain, always-buffered descriptor from the first open beside it for
whatever the direct one cannot carry. Darwin has no `O_DIRECT`;
`src/io/gcd_backend.hpp` instead flips `F_NOCACHE` on the single
descriptor with `fcntl`, per-fd rather than per-open. Windows carries two
independent per-open flags, `FILE_FLAG_NO_BUFFERING` and
`FILE_FLAG_OVERLAPPED` (the asynchronous handle); `src/io/iocp_backend.hpp`
and `src/io/ioring_backend.hpp` both ask for the two together on the
first reopen and, if that is refused, ask again for the asynchronous
handle alone before falling back further. The asynchrony is kept on its
own deliberately: `NO_BUFFERING` is usually what a filesystem (SMB
shares, some virtual volumes) actually rejects, and buffered-but-async
reads measure within noise of direct-and-async ones on Windows, so
giving up both together for a refusal that only concerned one of them
would discard the half that still works.

Windows has two completion mechanisms, so it is the one platform whose
backend is chosen at runtime. An I/O ring (`ioringapi.h`, Windows 11) is
the submission-queue model io_uring introduced: entries are built into a
queue the application owns, one `SubmitIoRing` hands a whole batch to the
kernel, and completions are popped from a second queue. Where the machine
offers `IORING_VERSION_3` -- reads, writes, cancel and registration --
`src/io/backend_select.hpp` takes it; everything older gets the completion
port. `file_io_options::ioring` and `file_write_options::ioring` (both
default `true`) choose the port instead, which is what the IOCP-against-
IoRing comparison needs, and the rung names below say which one ran. The
entry points are resolved from `KernelBase.dll` at first use rather than
linked, so a binary built against the Windows 11 SDK still loads on
Windows 10.

FreeBSD reaches the device through the one widely deployed Unix
asynchronous file API that is not io_uring: POSIX AIO. `src/io/aio_backend.hpp`
fills a `struct aiocb` per transfer, hands whole batches to the kernel
with one `lio_listio(LIO_NOWAIT, ...)`, and asks for each completion as
an `EVFILT_AIO` event on the context's kqueue
(`aio_sigevent.sigev_notify = SIGEV_KEVENT`), so the loop is the same
pull-style submit-and-reap io_uring's is. The AIO facility is the
kernel's own, not a thread pool in the process: the requests are served
by the `aiod` kernel threads whose count `vfs.aio.target_aio_procs` and
`vfs.aio.max_aio_procs` bound, and `vfs.aio.max_aio_queue_per_proc`
bounds how many a process may have outstanding. The context reads that
last one at construction and cuts every batch to what is still free, so
the limit shows up as fewer requests in flight rather than as a refusal;
a limit of zero, or a kernel with no AIO at all, is the whole facility
refused and the names below say so.

`O_DIRECT` on FreeBSD is a hint rather than a mode. UFS honours it for
page-aligned buffers, offsets and lengths, and quietly serves anything
else through the cache. ZFS honours it from OpenZFS 2.3 under the
dataset's `direct` property: `direct=standard` (the default there) serves
requests whose offset and length are 4 KiB aligned and answers `EINVAL`
for the rest, `direct=disabled` ignores the flag, `direct=always` takes
every request. Releases before OpenZFS 2.3 have no such property and no
direct path at all. Since neither the release nor the property is
something a caller should have to know, the backend asks the file
system instead: one page-sized `pread` through the direct descriptor at
open, and an `EINVAL` degrades that file to buffered with the
`" [direct: EINVAL]"` suffix on its name. Everything else -- a short
read, an end-of-file zero -- is the descriptor being served.

What the probe answers is whether the descriptor is *served*, which is
all `O_DIRECT` has ever promised on a system where it is a hint: a
dataset on OpenZFS 2.2 or older, or one with `direct=disabled`, takes
the flag without complaint and reads through the ARC anyway, and the
rung still reads `"aio+direct"`. On FreeBSD 14.5 (OpenZFS 2.2.9) that is
exactly what happens, and `zfs get direct` there answers "invalid
property": the release has no direct path to ask for.

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
generator that stops mid-window. Almost every file has one, and a file
smaller than a window is nothing else. On Linux, every piece of a
transfer -- the initial one, or what is left after a short completion --
passes `piece_for()` in `src/io/uring_backend.hpp`, which says how much
of it the ring takes and through which descriptor: a direct file's piece
rides the `O_DIRECT` descriptor for the whole granules it holds at a
granule offset, and what that leaves -- the sub-granule end, a
resumption off a boundary, and every piece of a buffered file -- rides
the ring through the plain buffered descriptor that sits beside the
direct one, which takes any shape. So the last window of a file is as
asynchronous as the others, and the synchronous path inside `poll()`
(`run_one_deferred()`, one entry per call) serves only a context with no
ring, a submit the kernel refused and a zero-length transfer. Nothing
above the backend can tell a window's bytes arrived through one
descriptor from arriving through two; both land in the same buffer.

FreeBSD asks it with the same function under the same name:
`piece_for()` in `src/io/aio_backend.hpp` picks the direct descriptor for
the whole granules at a granule offset and the plain one for the rest,
both as AIO requests, so a direct file's last window is as asynchronous
as the others there too.

Windows' `port_len()` asks the same question with one difference: the
plain handle there is not overlapped and cannot be given to the port, so
a `NO_BUFFERING` file's sub-granule rest -- at most 4095 bytes, once per
file -- is the one piece that finishes synchronously; a buffered file on
the port hands it every length whole. GCD needs none of this, because
`F_NOCACHE` imposes no alignment -- its deferred list exists only for the
separate case of GCD itself being unavailable.

## What was actually engaged

A file a filesystem will not open for direct I/O -- tmpfs, overlayfs,
some network and virtual filesystems -- is read or written buffered
instead, automatically: asking for `direct_io = true` and having it
refused is not an error, and nothing throws because the device could not
be bypassed. Which rung was engaged is reported after the fact.

Every reader and writer names the rung it landed on. A name starts
with an engine token and continues with the decorations that open
earned. The token comes from `include/blake3pp/detail/io_engine.def`,
which also generates the `blake3pp::io_engine` enum and
`to_string()`; `compiled_io_engines()`, `available_io_engines()` and
`best_io_engine()` give the same words without opening anything. The
spellings below document the decorations. Code that compares a token
should get it from `to_string()`, not from this page. The cache bypass
and whether one open kept its engine need a path: that is
`probe_file_io()`. See [the API reference](api.md).

- `blake3pp::probe_file_io(path, opts)` answers before any read: it opens
  the file as `update_file()` would, returns the plan's geometry, whether
  the bypass was asked for, and the rung the open landed on, then closes
  it. The spellings are the same as below, so a buffered rung beside
  `direct_io == true` says the filesystem declined.
- During or after a hash the rung reaches a caller through the trace:
  `blake3pp::read_backend(buffer.driver())` after an `update_file()` that
  was given a `trace_buffer` ([Performance tracing](tracing.md)). The
  spellings are `"io_uring+direct"`, `"ioring+direct"`, `"iocp+direct"`,
  `"gcd+nocache"`, `"aio+direct"`, `"pread"`, `"readfile"` and
  `"stdio"`.
- `write_report::backend` names the write side the same way:
  `"io_uring+direct"`, `"ioring+direct"`, `"iocp+direct"`,
  `"gcd-write+nocache"`, `"aio+direct"`, `"pwrite"`, `"writefile"`, or
  `"stdio"`, and on
  Windows adds how the preallocation
  was kept from zeroing ahead of the writes: `"+sparse"` or `"+validdata"`
  (next section).

`"stdio"` is not a rung anything degrades to: it is the fixed answer on
platforms with none of the five async engines (wasm), and on no other
platform. `" (inline submit)"` is appended on
Linux when `offload_submit` was turned off. When the async engine itself
was asked for and refused, not just the cache bypass -- a kernel too old
for io_uring, or io_uring blocked by seccomp policy -- the Linux backend
appends why: `" [io_uring: EPERM, blocked by policy]"` or
`" [io_uring: ENOSYS, kernel too old]"`. The ring does the same where it
is used directly rather than through the selector: `" [ioring: no api]"`,
`" [ioring: version 1]"`, `" [ioring: no read/write ops]"` (a version-3
ring whose op probe refuses reads or writes) or
`" [ioring: CreateIoRing failed 0x...]"`, and
a ring the selector asked for and could not have puts the same suffix on
the completion port's names, as in `"iocp+direct [ioring: no api]"`, so a
run on such a machine reads as what it is. FreeBSD's backend says it the
same way, with the two things it needs named apart: `" [aio: EAGAIN]"`
when the per-process AIO queue is closed
(`vfs.aio.max_aio_queue_per_proc` at zero),
`" [aio: ENOSYS, no kernel AIO]"` when the facility is not in the kernel
at all, and `" [kqueue: errno N]"` when there was nowhere to deliver a
completion to -- so the FreeBSD ladder reads `"aio+direct"`, `"aio"`,
`"pread+direct"`, `"pread"`, with `" [direct: EINVAL]"` on a file whose
filesystem refused the direct descriptor. A benchmark or a bug
report
therefore reads differently depending on which rung actually ran, never
only the one that was asked for.

## Preallocation on NTFS

A write that extends a file serializes on its allocation, which collapses
an asynchronous direct queue to one write at a time, so every writer here
preallocates the whole file at the open. On Windows that leaves a second
cost: NTFS keeps a *valid data length* per file, and every write that
lands beyond it makes NTFS zero the bytes in between first, so that no
reader ever sees the blocks a deleted file left there. With several
windows in flight their completions land out of order, every one of them
past the valid data length, and the zeroing is most of the write's cost:
measured on a PCIe 4 NVMe, 2.2-2.4 GiB/s at the automatic depth where the
device writes 5.9.

Two ways out, and the writer takes them in this order:

- **The valid data length set up front** (`SetFileValidData`), asked for
  with `file_write_options::waive_zero_fill`. Nothing is zeroed and
  nothing is zeroed later: 4.4-5.6 GiB/s at the automatic depth on the
  same drive. It needs `SeManageVolumePrivilege`, which the request
  enables on the process token, and until the writes land the file
  exposes whatever its disk blocks held before -- which is what the
  privilege gates, and why this is never the default. Reported as
  `"+validdata"`.
- **A sparse file**, the default, and the fallback where the privilege is
  not held. The file is made sparse before the preallocation, so the gaps
  stay holes that read as zeros instead of being written, and the flag is
  cleared once the file is complete, leaving a plain file: 4.5-4.8 GiB/s
  at the automatic depth. A run that fails leaves a sparse file with
  holes where no window landed. Reported as `"+sparse"`.

A file system that refuses sparse files gets neither, and the writer
runs with the zeroing; the report then names the rung alone. Unix file
systems preallocate unwritten extents that read as zeros at no cost, so
none of this exists there.

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
  amortizes it and a short one does not. Measured on four PCIe 5 drives
  in RAID 0 over 1000 MiB of files per row, buffered reads 5-10% faster
  than direct at every size up to 256 KiB and direct 12% faster at 1 MiB,
  so the plan reads a file below `min_pipeline_bytes` (512 KiB, the size
  at which the pipeline is skipped as well) through the page cache
  whatever `direct_io` says. A file whose size the filesystem will not
  give counts as large.
- **A filesystem that refuses it.** Network filesystems, overlay and
  virtual filesystems, and some container volumes decline `O_DIRECT` /
  `FILE_FLAG_NO_BUFFERING` outright. The degradation above makes this
  work correctly, but what runs afterward is buffered I/O with an extra
  refused reopen paid for nothing.
- **Linux kernels before 7.0.** On a 48-vCPU EPYC Milan machine with a
  local disk and kernel 6.8, this pipeline read 3.84 GiB/s buffered
  against 6.79 GiB/s direct, with the reading thread spending 72% of wall
  time inside the kernel's own buffered-read copy path rather than in
  this library's code. On a kernel 7.0 machine with two local SSDs in
  RAID 0 the buffered rate equals the direct one, 6.03 GiB/s both ways,
  with the copy down to about two cores of io-wq work. Those are two
  different machines, so what the pair shows is a pre-7.0 kernel path
  that is gone on 7.0 rather than one device measured twice; whether a
  given kernel still in use has it is a question for that kernel.
  Above that rate the buffered path was limited by this pipeline rather
  than by any kernel, and is not any more: holding one window at a time
  left one buffered read outstanding and so one io-wq worker copying, and
  the same z3 machine on one kernel build reads 3.48 GiB/s raw buffered at
  the old depth against 8.21 with several windows in flight, 91% of its
  direct rate. [Performance](performance.md#direct-against-buffered) has
  the pair.
  On Windows and on macOS the two measure the same, closely enough that an
  M4 Max reads 6.12 GiB/s buffered against 5.86 direct -- buffered 4%
  ahead of `F_NOCACHE`.

## How to tell what you got

In code, `write_report::backend` names the write side after a write, and
`blake3pp::read_backend(trace.driver())` the read side -- the exact strings
are in "What was actually engaged" above.

From the command line:

- `blake3ppsum` has no flag that prints its backend; `--no-direct` turns
  the request off (`--qd`/`--window` pick the rest of the pipeline's
  shape).
- `blake3ppgen --output FILE -v` reports the engaged write backend on
  stderr; `--no-direct` (needs `--output`) keeps the page cache instead.
- `blake3pp_bench_file FILE` always prints the engaged backend in its
  header line; `--no-direct` drives the same pipeline buffered, which is
  how the kernel gap above gets measured in the first place.
