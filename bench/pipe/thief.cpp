// thief: what an otherwise idle stdexec pool costs a busy task.
//
//   thief [--threads <N>] [--ms <D>]
//
// One task spins for D milliseconds on an exec::static_thread_pool of N
// threads; nothing else is submitted. The pool keeps an idle worker
// awake looking for work to steal. The measurement is the context
// switches the process takes per millisecond of the task's run,
// voluntary and involuntary, from getrusage(RUSAGE_SELF) around the run.
//
// This is the measurement behind sizing a stream's pool: a pool that is
// a percent busy pays this for every thread it holds.
//
// POSIX only. Windows has no per-process context-switch counter that
// can be read before and after a run (the per-thread figures from
// NtQuerySystemInformation have to be sampled while the threads live),
// so it is not built there. Needs the stdexec execution provider.

#include <chrono>
#include <cstdio>
#include <ratio>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <CLI/CLI.hpp>

#include <sys/resource.h>

namespace {

struct switches {
  long voluntary = 0, involuntary = 0;
};

switches now_switches() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return {ru.ru_nvcsw, ru.ru_nivcsw};
}

}  // namespace

int main(int argc, char** argv) {
  unsigned threads = 8;
  int ms = 1000;
  CLI::App app{"thief: context switches an idle stdexec pool costs one busy task"};
  app.add_option("--threads", threads, "pool threads")->check(CLI::Range(1u, 1024u));
  app.add_option("--ms", ms, "how long the task spins")->check(CLI::PositiveNumber);
  CLI11_PARSE(app, argc, argv);

  exec::static_thread_pool pool(threads);
  // Let the workers reach their idle state before counting.
  stdexec::sync_wait(stdexec::schedule(pool.get_scheduler()));

  const switches before = now_switches();
  const auto t0 = std::chrono::steady_clock::now();
  stdexec::sync_wait(stdexec::schedule(pool.get_scheduler()) | stdexec::then([ms] {
                       const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
                       while (std::chrono::steady_clock::now() < until) {
                       }
                     }));
  const double wall_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const switches after = now_switches();
  const long vol = after.voluntary - before.voluntary;
  const long inv = after.involuntary - before.involuntary;
  std::printf("thief threads=%u ms=%.1f voluntary=%ld involuntary=%ld | per ms: %.2f voluntary, %.2f involuntary\n",
              threads, wall_ms, vol, inv, static_cast<double>(vol) / wall_ms,
              static_cast<double>(inv) / wall_ms);
  return 0;
}
