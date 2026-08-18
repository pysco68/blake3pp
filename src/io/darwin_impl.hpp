#pragma once

// Shared Darwin plumbing for the direct-I/O reader and writer. macOS has
// no io_uring; the platform's async story IS libdispatch (GCD), and its
// page-cache bypass is fcntl(F_NOCACHE), per-fd rather than per-open,
// with no alignment contract: unaligned edges are silently served through
// the cache instead of being rejected, so the dual-fd tail trick the
// O_DIRECT and NO_BUFFERING backends need disappears here. The backend
// runs positional pread/pwrite loops on GCD's global concurrent pool,
// straight into the caller's buffer ring (dispatch_io was considered and
// rejected: it delivers dispatch_data_t chunks it allocated itself, an
// extra copy the zero-copy pipeline exists to avoid). A dispatch_group is
// the teardown drain and a mutex/condvar pair the completion queue.
// Internal to src/io/, never installed.

#if defined(__APPLE__)
#define BLAKE3PP_IO_GCD 1

#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>

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
// group exists for teardown: in-flight workers write into the buffer
// pool, so destroy() must wait them out before the pool is freed.
struct gcd_pump {
  dispatch_group_t group = nullptr;
  std::mutex m;
  std::condition_variable cv;

  bool init() noexcept {
    group = dispatch_group_create();
    return group != nullptr;
  }

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

}  // namespace blake3pp::detail::io_impl

#endif  // __APPLE__
