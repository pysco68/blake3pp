// rux: run one command and report what it cost.
//
//   rux [--out <file>] [--label <name>] -- <command> [args...]
//
// The command runs with this process's standard handles, so `rux -- a |
// rux -- b` is still a pipe between a and b, created by the shell. One
// line goes to --out (or to stderr):
//
//   rux label=<name> exit=<code> wall_us=<n> user_us=<n> sys_us=<n> vcsw=<n> ivcsw=<n>
//
// and rux exits with the command's own exit code, so a pipeline of two
// rux processes reports both sides' status even where the shell keeps
// only the last. Integers throughout, so a script without floating point
// (cmd.exe's `set /a`) can use them.
//
// What each field is, per platform:
//   wall_us        the monotonic clock from spawn to exit, everywhere
//   user_us/sys_us POSIX: wait4()'s rusage for the child.
//                  Windows: GetProcessTimes for the child.
//   vcsw/ivcsw     POSIX: voluntary and involuntary context switches from
//                  the same rusage. Windows has no per-process counter
//                  that survives the process -- the per-thread counts in
//                  NtQuerySystemInformation vanish with the threads -- so
//                  both read `na` there.
// A command that cannot be started reports exit=127; on POSIX one killed
// by a signal reports 128 plus the signal number.
//
// What the command starts with, for fault tests:
//   --sigpipe default|ignore  POSIX: SIGPIPE's disposition in the child,
//                             which decides whether a write into a pipe
//                             with no reader kills it or fails with EPIPE.
//                             A shell, cmake and Python each leave it
//                             differently, so a test states it.
//   --signal-storm <us>       POSIX: send SIGUSR1 to the child every <us>
//                             microseconds while it runs. That interrupts
//                             a blocking read only if the child handles
//                             SIGUSR1 without SA_RESTART; readshim's
//                             READSHIM_EINTR installs exactly that.
//                             Windows has no signals: not available.
//   --nonblock-stdin          the child's standard input in non-blocking
//                             mode: O_NONBLOCK on POSIX (on the open file
//                             description, which the shell's pipe shares),
//                             PIPE_NOWAIT on a Windows pipe.
//   --close-stdin             the child starts with no standard input.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <thread>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

using clock_type = std::chrono::steady_clock;

struct start_options {
  std::string sigpipe;  // "", "default" or "ignore"
  std::int64_t storm_us = 0;
  bool nonblock_stdin = false;
  bool close_stdin = false;
};

struct cost {
  int exit_code = 127;
  std::int64_t wall_us = 0, user_us = 0, sys_us = 0;
  std::int64_t vcsw = -1, ivcsw = -1;  // -1: not available here
};

#if defined(_WIN32)
// One argument quoted the way CommandLineToArgvW and the C runtime read
// it back: backslashes are literal except before a quote, where they
// escape and must be doubled.
std::string quote_arg(const std::string& a) {
  if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) {
    return a;
  }
  std::string q = "\"";
  for (std::size_t i = 0;; ++i) {
    std::size_t backslashes = 0;
    while (i < a.size() && a[i] == '\\') {
      ++i;
      ++backslashes;
    }
    if (i == a.size()) {
      q.append(backslashes * 2, '\\');
      break;
    }
    if (a[i] == '"') {
      q.append(backslashes * 2 + 1, '\\');
    } else {
      q.append(backslashes, '\\');
    }
    q.push_back(a[i]);
  }
  q.push_back('"');
  return q;
}

std::int64_t filetime_us(const FILETIME& ft) {
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  return static_cast<std::int64_t>(u.QuadPart / 10);
}

cost run(const std::vector<std::string>& cmd, const start_options& so) {
  std::string line;
  for (const std::string& a : cmd) {
    if (!line.empty()) {
      line.push_back(' ');
    }
    line += quote_arg(a);
  }
  // The child takes this process's standard handles; a pipe handle the
  // shell gave us must be inheritable for that, and is made so.
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  for (HANDLE h : {in, out, err}) {
    if (h != nullptr && h != INVALID_HANDLE_VALUE) {
      SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
  }
  if (so.nonblock_stdin) {
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    if (SetNamedPipeHandleState(in, &mode, nullptr, nullptr) == 0) {
      std::fprintf(stderr, "rux: stdin is not a pipe; cannot make it non-blocking\n");
    }
  }
  STARTUPINFOA si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = so.close_stdin ? nullptr : in;
  si.hStdOutput = out;
  si.hStdError = err;
  PROCESS_INFORMATION pi{};
  cost c;
  const auto t0 = clock_type::now();
  if (CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, 0, nullptr,
                     nullptr, &si, &pi) == 0) {
    std::fprintf(stderr, "rux: cannot start '%s' (error %lu)\n", line.c_str(),
                 GetLastError());
    return c;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  c.wall_us = std::chrono::duration_cast<std::chrono::microseconds>(clock_type::now() - t0).count();
  FILETIME created, exited, kernel, user;
  GetProcessTimes(pi.hProcess, &created, &exited, &kernel, &user);
  c.user_us = filetime_us(user);
  c.sys_us = filetime_us(kernel);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  c.exit_code = static_cast<int>(code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return c;
}
#else
std::int64_t timeval_us(const timeval& tv) {
  return static_cast<std::int64_t>(tv.tv_sec) * 1000000 + static_cast<std::int64_t>(tv.tv_usec);
}

cost run(const std::vector<std::string>& cmd, const start_options& so) {
  std::vector<char*> argv;
  argv.reserve(cmd.size() + 1);
  for (const std::string& a : cmd) {
    argv.push_back(const_cast<char*>(a.c_str()));
  }
  argv.push_back(nullptr);
  cost c;
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  if (so.sigpipe == "default") {
    sigset_t def;
    sigemptyset(&def);
    sigaddset(&def, SIGPIPE);
    posix_spawnattr_setsigdefault(&attr, &def);
  } else if (so.sigpipe == "ignore") {
    std::signal(SIGPIPE, SIG_IGN);  // an ignored signal stays ignored across exec
  }
  short flags = 0;
  if (so.sigpipe == "default") flags |= POSIX_SPAWN_SETSIGDEF;
  if (so.storm_us > 0) {
    // The child starts with SIGUSR1 blocked, so a signal sent before it
    // has a handler stays pending instead of killing it; readshim
    // unblocks it once its handler is installed.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    posix_spawnattr_setsigmask(&attr, &mask);
    flags |= POSIX_SPAWN_SETSIGMASK;
  }
  posix_spawnattr_setflags(&attr, flags);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (so.close_stdin) {
    posix_spawn_file_actions_addclose(&actions, STDIN_FILENO);
  }
  if (so.nonblock_stdin) {
    const int fl = ::fcntl(STDIN_FILENO, F_GETFL);
    if (fl < 0 || ::fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK) != 0) {
      std::fprintf(stderr, "rux: cannot make stdin non-blocking\n");
    }
  }
  pid_t pid = 0;
  const auto t0 = clock_type::now();
  const int e = ::posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  if (e != 0) {
    std::fprintf(stderr, "rux: cannot start '%s' (errno %d)\n", argv[0], e);
    return c;
  }
  int status = 0;
  rusage ru{};
  if (so.storm_us > 0) {
    // Signal until the child has gone; wait4 without blocking reaps it.
    for (;;) {
      const pid_t w = ::wait4(pid, &status, WNOHANG, &ru);
      if (w == pid) break;
      if (w < 0 && errno != EINTR) {
        std::fprintf(stderr, "rux: wait4 failed (errno %d)\n", errno);
        return c;
      }
      ::kill(pid, SIGUSR1);
      std::this_thread::sleep_for(std::chrono::microseconds(so.storm_us));
    }
  } else {
    while (::wait4(pid, &status, 0, &ru) < 0) {
      if (errno != EINTR) {
        std::fprintf(stderr, "rux: wait4 failed (errno %d)\n", errno);
        return c;
      }
    }
  }
  c.wall_us = std::chrono::duration_cast<std::chrono::microseconds>(clock_type::now() - t0).count();
  c.user_us = timeval_us(ru.ru_utime);
  c.sys_us = timeval_us(ru.ru_stime);
  c.vcsw = static_cast<std::int64_t>(ru.ru_nvcsw);
  c.ivcsw = static_cast<std::int64_t>(ru.ru_nivcsw);
  if (WIFEXITED(status)) {
    c.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    c.exit_code = 128 + WTERMSIG(status);
  }
  return c;
}
#endif

std::string count(std::int64_t v) { return v < 0 ? std::string("na") : std::to_string(v); }

}  // namespace

int main(int argc, char** argv) {
  std::string out_path;
  std::string label = "-";
  std::vector<std::string> cmd;
  start_options so;

  CLI::App app{"rux: run a command and report its exit code, wall clock, CPU and context switches"};
  app.add_option("--out,-o", out_path, "write the report line to this file instead of stderr");
  app.add_option("--label,-l", label, "a name for the line");
  app.add_option("--sigpipe", so.sigpipe, "POSIX: SIGPIPE in the child")
      ->check(CLI::IsMember({"default", "ignore"}));
  app.add_option("--signal-storm", so.storm_us, "POSIX: SIGUSR1 to the child every this many microseconds")
      ->check(CLI::PositiveNumber);
  app.add_flag("--nonblock-stdin", so.nonblock_stdin, "start the child with non-blocking stdin");
  app.add_flag("--close-stdin", so.close_stdin, "start the child with no stdin");
  app.add_option("command", cmd, "the command and its arguments, after --")->required();
  app.positionals_at_end();
  CLI11_PARSE(app, argc, argv);
#if defined(_WIN32)
  if (so.storm_us > 0) {
    std::fprintf(stderr, "rux: --signal-storm needs POSIX signals; Windows has none\n");
    return 2;
  }
  if (!so.sigpipe.empty()) {
    std::fprintf(stderr, "rux: --sigpipe is POSIX; a Windows write into a closed pipe fails with ERROR_NO_DATA\n");
    return 2;
  }
#endif

  const cost c = run(cmd, so);
  const std::string line = "rux label=" + label + " exit=" + std::to_string(c.exit_code) +
                           " wall_us=" + std::to_string(c.wall_us) +
                           " user_us=" + std::to_string(c.user_us) +
                           " sys_us=" + std::to_string(c.sys_us) +
                           " vcsw=" + count(c.vcsw) + " ivcsw=" + count(c.ivcsw) + "\n";
  if (out_path.empty()) {
    std::fputs(line.c_str(), stderr);
  } else {
    std::FILE* f = std::fopen(out_path.c_str(), "wb");
    if (f == nullptr) {
      std::fprintf(stderr, "rux: cannot write %s\n", out_path.c_str());
      return c.exit_code == 0 ? 126 : c.exit_code;
    }
    std::fputs(line.c_str(), f);
    std::fclose(f);
  }
  return c.exit_code;
}
