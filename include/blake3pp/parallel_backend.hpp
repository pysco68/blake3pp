#pragma once

/// \file
/// Sizing the process-wide parallel scheduler.
///
/// P2079's `get_parallel_scheduler()` returns one scheduler per process
/// and takes no size; beman.execution's default backend runs on
/// `hardware_concurrency()` threads. A program chooses otherwise by
/// replacing the backend behind it, one definition per program, which is
/// what this header's definition does.
///
/// It is not part of <blake3pp/blake3pp.hpp>, and the definition lives in
/// the separate `blake3pp::parallel_backend` target: a program that brings
/// its own scheduler, or wants every core, links neither.
///
/// Requires `-DBLAKE3PP_SIZED_PARALLEL_SCHEDULER=ON`. Without it the target
/// does not exist and a call here fails to link.
///
/// @code
/// #include <blake3pp/parallel_backend.hpp>
/// #include <blake3pp/parallel.hpp>
///
/// blake3pp::size_parallel_scheduler(8);   // before anything uses it
/// auto d = blake3pp::hash(buf, blake3pp::get_parallel_scheduler());
/// @endcode
///
/// The scheduler-taking overloads accept any scheduler: a caller who
/// already owns an executor passes it and needs none of this.

namespace blake3pp {

/// Fixes the number of threads the process-wide parallel scheduler runs on.
///
/// Call it once, before the first use of `get_parallel_scheduler()` (and
/// before any hash overload that defaults to it), from one thread. The
/// size applies to the program; several differently sized pools are
/// schedulers the caller owns and passes explicitly.
///
/// @param threads  Worker threads; must be at least 1.
/// @throws std::invalid_argument if threads is 0.
/// @throws std::logic_error if the scheduler has already been created, or
///         if a different size was already fixed.
void size_parallel_scheduler(unsigned threads);

/// The size fixed by size_parallel_scheduler(), or 0 if none was.
[[nodiscard]] unsigned parallel_scheduler_threads() noexcept;

}  // namespace blake3pp
