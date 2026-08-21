#pragma once

// The Darwin backend. macOS has no io_uring; the platform's async story IS
// libdispatch (GCD), and its page-cache bypass is fcntl(F_NOCACHE),
// per-fd rather than per-open, with no alignment contract: unaligned edges
// are silently served through the cache instead of being rejected, so the
// dual-fd tail trick the O_DIRECT and NO_BUFFERING backends need
// disappears here. The backend runs positional pread/pwrite loops on GCD's
// global concurrent pool, straight into the engine's buffer ring
// (dispatch_io was considered and rejected: it delivers dispatch_data_t
// chunks it allocated itself, an extra copy the zero-copy pipeline exists
// to avoid). A dispatch_group is the teardown drain and a mutex/condvar
// pair the completion queue. Internal to src/io/, never installed.

#if defined(__APPLE__)

#include <dispatch/dispatch.h>
#include <string_view>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <system_error>
#include <vector>

#include "io/backend.hpp"
#include "io/posix_file.hpp"

namespace blake3pp::detail::io_impl {

// Turns the page cache off for this fd: Darwin's O_DIRECT analogue.
inline bool set_nocache(int fd) noexcept {
  return ::fcntl(fd, F_NOCACHE, 1) != -1;
}

// fallocate's Darwin twin: reserve the extents (contiguous if the volume
// can, scattered otherwise), then give the file its final logical size,
// so queued writes land as overwrites instead of size-extending appends.
inline bool preallocate(int fd, std::uint64_t len) noexcept {
  fstore_t st{};
  st.fst_flags = F_ALLOCATECONTIG;
  st.fst_posmode = F_PEOFPOSMODE;
  st.fst_offset = 0;
  st.fst_length = static_cast<off_t>(len);
  if (::fcntl(fd, F_PREALLOCATE, &st) == -1) {
    st.fst_flags = F_ALLOCATEALL;
    if (::fcntl(fd, F_PREALLOCATE, &st) == -1) {
      return false;
    }
  }
  return ::ftruncate(fd, static_cast<off_t>(len)) == 0;
}

// The submission/completion machinery, playing the role the uring and the
// completion port play elsewhere: submit() is fire-and-forget onto GCD's
// global pool, workers publish per-slot completion under m and signal cv,
// and the pipeline thread blocks on cv for the slot it needs next. The
// group exists for teardown: in-flight workers touch the engine's buffer
// pool, so destroy() must wait them out before the pool is freed.
struct gcd_pump {
  dispatch_group_t group = nullptr;
  std::mutex m;
  std::condition_variable cv;

  bool init() noexcept {
    group = dispatch_group_create();
    return group != nullptr;
  }

  // Owning the group means owning its release. The backends below also
  // call destroy() explicitly (it is idempotent), but they can only do so
  // once their constructor has COMPLETED: both allocate slot vectors after
  // init() succeeds, and a throw there destroys members without ever
  // running the backend destructor. This is the net under that window.
  ~gcd_pump() { destroy(); }
  gcd_pump() = default;
  gcd_pump(const gcd_pump&) = delete;
  gcd_pump& operator=(const gcd_pump&) = delete;

  void destroy() noexcept {
    if (group != nullptr) {
      dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
      dispatch_release(group);
      group = nullptr;
    }
  }

  void submit(void (*fn)(void*), void* ctx) noexcept {
    dispatch_group_async_f(
        group, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ctx,
        fn);
  }
};

class gcd_reader {
 public:
  gcd_reader(const std::filesystem::path& path,
             const file_reader_options& opts, unsigned nslots) {
    f_.open(path.c_str(), O_RDONLY | O_CLOEXEC);
    size_ = f_.stat_size();
    // Darwin's cache bypass is per-fd, not per-open, and tolerates any
    // alignment, so one fd serves every window, tail included.
    if (opts.direct_io && set_nocache(f_.fd_plain)) {
      f_.direct = true;
    }
    name_ = f_.direct ? "pread+nocache" : "pread";
    if (opts.async && pump_.init()) {
      use_gcd_ = true;
      slots_.resize(nslots);
      tasks_.resize(nslots);
      for (unsigned s = 0; s < nslots; ++s) {
        tasks_[s] = {this, s};
      }
      name_ = f_.direct ? "gcd+nocache" : "gcd";
    }
  }

  // In-flight workers write into the engine's buffer pool: wait them out
  // here, before the pool member (declared before this backend in the
  // engine) is freed. This is the GCD flavor of the IOCP cancel-and-drain
  // rule.
  ~gcd_reader() { pump_.destroy(); }
  gcd_reader(const gcd_reader&) = delete;
  gcd_reader& operator=(const gcd_reader&) = delete;

  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  // Every window, the unaligned tail included, takes the async path:
  // F_NOCACHE has no alignment contract, the kernel just serves unaligned
  // edges through the cache.
  [[nodiscard]] bool wants_async(std::uint64_t, std::size_t) const noexcept {
    return use_gcd_;
  }

  void start(unsigned s, std::uint64_t off, std::span<std::byte> buf) {
    slot& st = slots_[s];
    st.dst = buf.data();
    st.len = buf.size();
    st.off = off;
    st.filled = 0;
    st.error = 0;
    st.ready = false;
    pump_.submit(&gcd_reader::run_read, &tasks_[s]);
  }

  // Blocks until slot s completes; throws the worker's deferred errno.
  // ready/error are written under the pump lock, so even the "is it done
  // already" check lives here; an unlocked peek would be a data race.
  void wait(unsigned s) {
    slot& st = slots_[s];
    std::unique_lock<std::mutex> lk(pump_.m);
    pump_.cv.wait(lk, [&] { return st.ready; });
    if (st.error != 0) {
      throw std::system_error(st.error, std::generic_category(),
                              "gcd pread");
    }
  }

  void read_sync(std::uint64_t off, std::span<std::byte> buf) {
    f_.pread_all(f_.fd_plain, buf.data(), buf.size(), off);
  }

 private:
  struct slot {
    std::byte* dst = nullptr;
    std::size_t len = 0;
    std::uint64_t off = 0;
    std::size_t filled = 0;
    int error = 0;  // errno captured by the worker; thrown at wait()
    bool ready = false;
  };
  struct task {
    gcd_reader* self = nullptr;
    unsigned s = 0;
  };

  // Runs on a GCD worker: fills the slot's buffer with one positional
  // read loop, then publishes completion under the pump lock. noexcept:
  // errors travel through slot::error to the waiting thread.
  static void run_read(void* ctx) noexcept {
    const task t = *static_cast<task*>(ctx);
    gcd_reader& r = *t.self;
    slot& st = r.slots_[t.s];
    int err = 0;
    std::size_t got = 0;
    while (got < st.len) {
      const ssize_t n = ::pread(r.f_.fd_plain, st.dst + got, st.len - got,
                                static_cast<off_t>(st.off + got));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        err = errno;
        break;
      }
      if (n == 0) {
        err = EIO;  // unexpected EOF
        break;
      }
      got += static_cast<std::size_t>(n);
    }
    {
      const std::lock_guard<std::mutex> lk(r.pump_.m);
      st.filled = got;
      st.error = err;
      st.ready = true;
    }
    r.pump_.cv.notify_all();
  }

  posix_file f_;
  gcd_pump pump_;
  std::vector<slot> slots_;
  std::vector<task> tasks_;
  std::uint64_t size_ = 0;
  bool use_gcd_ = false;
  std::string_view name_ = "pread";
};

class gcd_writer {
 public:
  gcd_writer(const std::filesystem::path& path,
             const file_writer_options& opts, unsigned nslots) {
    f_.open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    // Same story as Linux's fallocate, via Darwin's spelling: queued
    // writes should land as overwrites, not size-extending appends.
    if (opts.preallocate_bytes > 0 &&
        preallocate(f_.fd_plain, opts.preallocate_bytes)) {
      prealloc_ = opts.preallocate_bytes;
    }
    if (opts.direct_io && set_nocache(f_.fd_plain)) {
      f_.direct = true;
    }
    name_ = f_.direct ? "pwrite+nocache" : "pwrite";
    if (opts.async && pump_.init()) {
      use_gcd_ = true;
      slots_.resize(nslots);
      tasks_.resize(nslots);
      for (unsigned s = 0; s < nslots; ++s) {
        tasks_[s] = {this, s};
      }
      name_ = f_.direct ? "gcd+nocache" : "gcd";
    }
  }

  // If finish() threw (or was skipped), workers may still be writing from
  // the engine's pool: wait them all out before it is freed.
  ~gcd_writer() { pump_.destroy(); }
  gcd_writer(const gcd_writer&) = delete;
  gcd_writer& operator=(const gcd_writer&) = delete;

  [[nodiscard]] std::string_view name() const noexcept { return name_; }

  [[nodiscard]] bool wants_async(std::size_t len) const noexcept {
    return use_gcd_ && len % direct_align == 0;
  }

  void start_write(unsigned s, std::uint64_t off,
                   std::span<const std::byte> buf) {
    slot& st = slots_[s];
    st.src = buf.data();
    st.len = buf.size();
    st.off = off;
    st.done = 0;
    st.busy = true;
    pump_.submit(&gcd_writer::run_write, &tasks_[s]);
  }

  // Blocks until slot s is idle; surfaces its deferred write error once
  // (cleared after the throw so the slot stays reusable).
  void wait_slot(unsigned s) {
    if (!use_gcd_) {
      return;
    }
    slot& st = slots_[s];
    std::unique_lock<std::mutex> lk(pump_.m);
    pump_.cv.wait(lk, [&] { return !st.busy; });
    if (st.error != 0) {
      const int e = st.error;
      st.error = 0;
      throw std::system_error(e, std::generic_category(), "gcd pwrite");
    }
  }

  void write_sync(std::uint64_t off, std::span<const std::byte> buf) {
    f_.pwrite_all(f_.fd_plain, buf.data(), buf.size(), off);
  }

  void finish(std::uint64_t written) {
    if (use_gcd_) {
      for (unsigned s = 0; s < slots_.size(); ++s) {
        wait_slot(s);
      }
    }
    // F_PREALLOCATE/ftruncate set the size up front; trim if less was
    // written.
    if (prealloc_ > written &&
        ::ftruncate(f_.fd_plain, static_cast<off_t>(written)) != 0) {
      throw_errno("ftruncate");
    }
    prealloc_ = 0;
  }

 private:
  struct slot {
    const std::byte* src = nullptr;
    std::size_t len = 0;
    std::uint64_t off = 0;
    std::size_t done = 0;
    int error = 0;  // errno captured by the worker; thrown at wait_slot()
    bool busy = false;
  };
  struct task {
    gcd_writer* self = nullptr;
    unsigned s = 0;
  };

  // Runs on a GCD worker: drains the slot's buffer with one positional
  // write loop, then publishes completion under the pump lock.
  static void run_write(void* ctx) noexcept {
    const task t = *static_cast<task*>(ctx);
    gcd_writer& w = *t.self;
    slot& st = w.slots_[t.s];
    int err = 0;
    std::size_t put = 0;
    while (put < st.len) {
      const ssize_t n = ::pwrite(w.f_.fd_plain, st.src + put, st.len - put,
                                 static_cast<off_t>(st.off + put));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        err = errno;
        break;
      }
      put += static_cast<std::size_t>(n);
    }
    {
      const std::lock_guard<std::mutex> lk(w.pump_.m);
      st.done = put;
      st.error = err;
      st.busy = false;
    }
    w.pump_.cv.notify_all();
  }

  posix_file f_;
  gcd_pump pump_;
  std::vector<slot> slots_;
  std::vector<task> tasks_;
  std::uint64_t prealloc_ = 0;
  bool use_gcd_ = false;
  std::string_view name_ = "pwrite";
};

// Definition-site conformance check (see uring_backend.hpp).
static_assert(reader_backend<gcd_reader>);
static_assert(writer_backend<gcd_writer>);

}  // namespace blake3pp::detail::io_impl

#endif  // __APPLE__
