// The preconditions the library checks in EVERY build, each in its own
// process, because what they do is end the process (see
// include/blake3pp/detail/contract.hpp).
//
// A check that cannot be shown to fire is a comment. doctest cannot catch
// an abort, so each case is a ctest entry whose PASS_REGULAR_EXPRESSION is
// the message that check promises to print; the exit code is deliberately
// not the thing being tested.
//
// Every case here is a deliberate misuse of an internal interface. That is
// the point: these are the contracts whose violation would otherwise be a
// wrong digest or a write past the end of a span, with nothing reported.
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

#include <blake3pp/blake3pp.hpp>
#include <blake3pp/detail/tree_reducer.hpp>

namespace {

blake3pp::detail::tree_reducer::cv_type some_cv(std::uint32_t seed) {
  blake3pp::detail::tree_reducer::cv_type cv{};
  for (std::size_t i = 0; i < cv.size(); ++i) {
    cv[i] = seed * 2654435761u + static_cast<std::uint32_t>(i);
  }
  return cv;
}

// Absorbing the same chunks twice: the digest would cover a message that
// does not exist, and verify against nothing.
void reducer_double_insert() {
  blake3pp::hasher h;
  std::array<blake3pp::detail::tree_reducer::node, 64> storage{};
  blake3pp::detail::tree_reducer r(blake3pp::detail::resolve(h.selected_arch()),
                                  h.key_words(), h.mode_flags(), storage);
  const auto cv = some_cv(1);
  (void)r.insert(0, 4, cv);
  (void)r.insert(4, 4, cv);
  // 0..4 again, which the pending scan cannot see: it merged into 0..8.
  (void)r.insert(0, 4, cv);
  std::puts("not reached");
}

// A hole in the run: every node after it lands at the wrong position.
void reducer_hole() {
  blake3pp::hasher h;
  std::array<blake3pp::detail::tree_reducer::node, 64> storage{};
  blake3pp::detail::tree_reducer r(blake3pp::detail::resolve(h.selected_arch()),
                                  h.key_words(), h.mode_flags(), storage);
  (void)r.insert(0, 1, some_cv(1));
  (void)r.insert(2, 1, some_cv(2));  // chunk 1 is missing
  r.drain_into(h);
  std::puts("not reached");
}

// A hasher part-way through a chunk cannot take a subtree: the bytes it
// holds would be hashed as the head of a chunk past that subtree.
void push_subtree_mid_chunk() {
  blake3pp::hasher h;
  const std::vector<std::byte> partial(100, std::byte{0x11});
  h.update(partial);
  h.push_subtree_cv(some_cv(3), 2);
  std::puts("not reached");
}

// to_hex() is noexcept and writes 2 characters per byte: a span that does
// not hold them would be written past its end.
void to_hex_short_output() {
  const blake3pp::digest d = blake3pp::hash("abc");
  std::array<char, 16> too_small{};
  blake3pp::to_hex(d.bytes, too_small);
  std::puts("not reached");
}

// Folding a count that is not a power of two drops the chaining value
// that had no sibling, so the digest covers fewer subtrees than it was
// given.
void fold_odd_count() {
  blake3pp::hasher h;
  std::array<blake3pp::detail::tree_reducer::cv_type, 3> three{
      some_cv(1), some_cv(2), some_cv(3)};
  blake3pp::detail::tree_reducer::cv_type out{};
  blake3pp::detail::fold_sibling_cvs(
      blake3pp::detail::resolve(h.selected_arch()), three, h.key_words(),
      h.mode_flags(), out);
  std::puts("not reached");
}

// Two threads inside one context: the second would walk the operation list
// the first is mutating. Reported rather than aborted, because run() can
// throw -- so this case checks the message of the exception it threw.
void two_runners() {
  blake3pp::async::io_context ctx({.window_bytes = 64 * 1024});
  std::atomic<bool> running{false};
  std::thread runner([&] {
    running.store(true, std::memory_order_release);
    try {
      ctx.run();
    } catch (...) {
    }
  });
  while (!running.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  // Give the first runner time to be inside drive().
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  int exit_code = 1;
  try {
    ctx.run_until_idle();
    std::puts("not reached");
  } catch (const std::logic_error& e) {
    std::fprintf(stderr, "%s\n", e.what());  // already named
    exit_code = 0;
  }
  ctx.request_stop();
  runner.join();
  std::_Exit(exit_code);
}

struct case_entry {
  std::string_view name;
  void (*run)();
};

constexpr case_entry cases[] = {
    {"reducer-double-insert", &reducer_double_insert},
    {"reducer-hole", &reducer_hole},
    {"push-subtree-mid-chunk", &push_subtree_mid_chunk},
    {"to-hex-short-output", &to_hex_short_output},
    {"fold-odd-count", &fold_odd_count},
    {"two-runners", &two_runners},
};

}  // namespace

// The check under test ends the process with abort(), and ctest fails an
// abnormally terminated test whatever its output says. So the abort is
// turned into a normal exit here and the MESSAGE is what the ctest entry
// matches: a case whose check does not fire prints nothing of the sort and
// returns 1 instead.
extern "C" void on_abort(int) {
  std::fflush(stderr);
  std::_Exit(0);
}

int main(int argc, char** argv) {
#if defined(_WIN32)
  // No "abort() has been called" dialog and no WER report: this is an
  // expected abort.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ASSERT, 0);
#endif
  std::signal(SIGABRT, &on_abort);
  if (argc != 2) {
    std::fputs("usage: blake3pp_contract_violations <case>\ncases:", stderr);
    for (const auto& c : cases) {
      std::fprintf(stderr, " %.*s", static_cast<int>(c.name.size()),
                   c.name.data());
    }
    std::fputc('\n', stderr);
    return 2;
  }
  for (const auto& c : cases) {
    if (c.name == argv[1]) {
      c.run();
      return 1;  // the case returned, so the check did not fire
    }
  }
  std::fprintf(stderr, "unknown case %s\n", argv[1]);
  return 2;
}
