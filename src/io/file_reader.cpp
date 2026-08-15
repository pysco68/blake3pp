// file_reader implementation. On Linux this drives io_uring through raw
// syscalls (three of them: setup, enter, and mmap for the rings), with no
// liburing dependency, and every moving part visible. Everything degrades
// per-feature at runtime: O_DIRECT refused by the filesystem -> buffered
// reads; io_uring refused (seccomp, old kernel) -> synchronous pread;
// non-POSIX -> stdio.

#include <blake3pp/detail/file_reader.hpp>

#include <bit>
#include <cerrno>
#include <cstring>
#include <new>
#include <system_error>
#include <vector>

#include <blake3pp/blake3pp.hpp>  // chunk_size

#if defined(__unix__) || defined(__APPLE__)
#define BLAKE3PP_IO_POSIX 1
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#define BLAKE3PP_IO_URING 1
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include <atomic>
#endif

#if !defined(BLAKE3PP_IO_POSIX)
#include <cstdio>
#endif

namespace blake3pp::detail {
namespace {

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

constexpr std::size_t direct_align = 4096;

#if defined(BLAKE3PP_IO_URING)
int sys_io_uring_setup(unsigned entries, io_uring_params* p) noexcept {
  return static_cast<int>(::syscall(__NR_io_uring_setup, entries, p));
}

int sys_io_uring_enter(int ring_fd, unsigned to_submit, unsigned min_complete,
                       unsigned flags) noexcept {
  return static_cast<int>(::syscall(__NR_io_uring_enter, ring_fd, to_submit,
                                    min_complete, flags, nullptr, 0));
}

// The mapped rings, reduced to what reads need. Kernel-shared integers are
// accessed through atomic_ref with acquire/release, per the io_uring
// memory-ordering contract.
struct uring {
  int fd = -1;
  unsigned sq_entries = 0;
  unsigned cq_entries = 0;
  void* sq_ring = nullptr;
  std::size_t sq_ring_sz = 0;
  void* cq_ring = nullptr;
  std::size_t cq_ring_sz = 0;
  io_uring_sqe* sqes = nullptr;
  std::size_t sqes_sz = 0;
  unsigned* sq_tail = nullptr;
  unsigned* sq_mask = nullptr;
  unsigned* sq_array = nullptr;
  unsigned* cq_head = nullptr;
  unsigned* cq_tail = nullptr;
  unsigned* cq_mask = nullptr;
  io_uring_cqe* cqes = nullptr;

  bool init(unsigned entries) noexcept {
    io_uring_params p;
    std::memset(&p, 0, sizeof(p));
    fd = sys_io_uring_setup(entries, &p);
    if (fd < 0) {
      return false;
    }
    sq_entries = p.sq_entries;
    cq_entries = p.cq_entries;
    sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    cq_ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
      sq_ring_sz = cq_ring_sz = std::max(sq_ring_sz, cq_ring_sz);
    }
    sq_ring = ::mmap(nullptr, sq_ring_sz, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (sq_ring == MAP_FAILED) {
      return fail();
    }
    cq_ring = (p.features & IORING_FEAT_SINGLE_MMAP)
                  ? sq_ring
                  : ::mmap(nullptr, cq_ring_sz, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
    if (cq_ring == MAP_FAILED) {
      return fail();
    }
    sqes_sz = p.sq_entries * sizeof(io_uring_sqe);
    sqes = static_cast<io_uring_sqe*>(
        ::mmap(nullptr, sqes_sz, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES));
    if (sqes == MAP_FAILED) {
      sqes = nullptr;
      return fail();
    }
    auto* sqb = static_cast<unsigned char*>(sq_ring);
    auto* cqb = static_cast<unsigned char*>(cq_ring);
    sq_tail = reinterpret_cast<unsigned*>(sqb + p.sq_off.tail);
    sq_mask = reinterpret_cast<unsigned*>(sqb + p.sq_off.ring_mask);
    sq_array = reinterpret_cast<unsigned*>(sqb + p.sq_off.array);
    cq_head = reinterpret_cast<unsigned*>(cqb + p.cq_off.head);
    cq_tail = reinterpret_cast<unsigned*>(cqb + p.cq_off.tail);
    cq_mask = reinterpret_cast<unsigned*>(cqb + p.cq_off.ring_mask);
    cqes = reinterpret_cast<io_uring_cqe*>(cqb + p.cq_off.cqes);
    return true;
  }

  bool fail() noexcept {
    destroy();
    return false;
  }

  void destroy() noexcept {
    if (sqes != nullptr) {
      ::munmap(sqes, sqes_sz);
      sqes = nullptr;
    }
    if (cq_ring != nullptr && cq_ring != sq_ring) {
      ::munmap(cq_ring, cq_ring_sz);
    }
    cq_ring = nullptr;
    if (sq_ring != nullptr) {
      ::munmap(sq_ring, sq_ring_sz);
      sq_ring = nullptr;
    }
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }

  // Queues one READ; the sole submitter, so sq_tail needs no CAS.
  void submit_read(int file_fd, void* buf, unsigned len, std::uint64_t off,
                   std::uint64_t user_data) {
    const unsigned tail = *sq_tail;  // we are the only writer
    const unsigned idx = tail & *sq_mask;
    io_uring_sqe& sqe = sqes[idx];
    std::memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = IORING_OP_READ;
    sqe.fd = file_fd;
    sqe.addr = reinterpret_cast<std::uint64_t>(buf);
    sqe.len = len;
    sqe.off = off;
    sqe.user_data = user_data;
    sq_array[idx] = idx;
    std::atomic_ref<unsigned>(*sq_tail).store(tail + 1,
                                              std::memory_order_release);
    if (sys_io_uring_enter(fd, 1, 0, 0) < 0) {
      throw_errno("io_uring_enter(submit)");
    }
  }

  // Blocks for one completion and returns (user_data, result).
  std::pair<std::uint64_t, int> wait_one() {
    for (;;) {
      const unsigned head = *cq_head;  // we are the only consumer
      const unsigned tail =
          std::atomic_ref<unsigned>(*cq_tail).load(std::memory_order_acquire);
      if (head != tail) {
        const io_uring_cqe& cqe = cqes[head & *cq_mask];
        const std::pair<std::uint64_t, int> out{cqe.user_data, cqe.res};
        std::atomic_ref<unsigned>(*cq_head).store(head + 1,
                                                  std::memory_order_release);
        return out;
      }
      if (sys_io_uring_enter(fd, 0, 1, IORING_ENTER_GETEVENTS) < 0 &&
          errno != EINTR) {
        throw_errno("io_uring_enter(wait)");
      }
    }
  }
};
#endif  // BLAKE3PP_IO_URING

}  // namespace

struct file_reader::impl {
  std::uint64_t size = 0;
  std::size_t window = 0;
  unsigned qd = 0;
  std::uint64_t num_windows = 0;
  std::uint64_t next_submit = 0;   // next window index to assign to a slot
  std::uint64_t next_deliver = 0;  // next window index to hand out
  std::byte* pool = nullptr;
  std::size_t pool_sz = 0;
  const char* backend_name = "unknown";

  struct slot_state {
    std::uint64_t win = 0;   // window index assigned to this slot
    std::size_t target = 0;  // bytes this window must read
    std::size_t filled = 0;  // bytes completed so far (async)
    bool assigned = false;
    bool ready = false;  // data complete, awaiting delivery
    bool held = false;   // delivered, not yet released
  };
  std::vector<slot_state> slots;

#if defined(BLAKE3PP_IO_POSIX)
  int fd = -1;        // main data fd (O_DIRECT when engaged)
  int fd_plain = -1;  // always-buffered fd for the unaligned tail window
  bool direct = false;
#else
  std::FILE* stream = nullptr;
#endif
#if defined(BLAKE3PP_IO_URING)
  uring ring;
  bool use_uring = false;
#endif

  std::size_t window_len(std::uint64_t w) const noexcept {
    const std::uint64_t off = w * window;
    const std::uint64_t rest = size - off;
    return rest < window ? static_cast<std::size_t>(rest) : window;
  }

  std::byte* buf(unsigned slot) const noexcept {
    return pool + static_cast<std::size_t>(slot) * window;
  }

  // Whether this window can go through the O_DIRECT/io_uring path: only
  // fully-aligned lengths may; the tail is read with a plain pread.
  bool alignable(std::uint64_t w) const noexcept {
    return window_len(w) % direct_align == 0;
  }

  void assign(unsigned s) {
    slot_state& st = slots[s];
    st.win = next_submit++;
    st.target = window_len(st.win);
    st.filled = 0;
    st.assigned = true;
    st.ready = false;
    st.held = false;
#if defined(BLAKE3PP_IO_URING)
    if (use_uring && alignable(st.win)) {
      ring.submit_read(fd, buf(s), static_cast<unsigned>(st.target),
                       st.win * window, s);
      return;
    }
#endif
    // Synchronous backends read lazily at delivery time.
  }

  void read_sync(unsigned s) {
    slot_state& st = slots[s];
#if defined(BLAKE3PP_IO_POSIX)
    // The tail (or everything, in pread mode) goes through the buffered
    // fd: O_DIRECT rejects unaligned lengths.
    const int use_fd = alignable(st.win) && direct ? fd : fd_plain;
    std::size_t got = 0;
    while (got < st.target) {
      const ssize_t n =
          ::pread(use_fd, buf(s) + got, st.target - got,
                  static_cast<off_t>(st.win * window + got));
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw_errno("pread");
      }
      if (n == 0) {
        throw std::system_error(EIO, std::generic_category(),
                                "unexpected EOF");
      }
      got += static_cast<std::size_t>(n);
    }
#else
    if (std::fseek(stream, static_cast<long>(st.win * window), SEEK_SET) !=
        0) {
      throw_errno("fseek");
    }
    if (std::fread(buf(s), 1, st.target, stream) != st.target) {
      throw std::system_error(EIO, std::generic_category(), "fread");
    }
#endif
    st.filled = st.target;
    st.ready = true;
  }

#if defined(BLAKE3PP_IO_URING)
  // Reaps completions (issuing continuations for short reads) until the
  // slot owning `want_win` is fully read.
  void wait_async(std::uint64_t want_win) {
    for (;;) {
      for (const slot_state& st : slots) {
        if (st.assigned && st.win == want_win && st.ready) {
          return;
        }
      }
      const auto [ud, res] = ring.wait_one();
      const unsigned s = static_cast<unsigned>(ud);
      slot_state& st = slots[s];
      if (res < 0) {
        throw std::system_error(-res, std::generic_category(),
                                "io_uring read");
      }
      if (res == 0) {
        throw std::system_error(EIO, std::generic_category(),
                                "unexpected EOF (io_uring)");
      }
      st.filled += static_cast<std::size_t>(res);
      if (st.filled < st.target) {
        ring.submit_read(fd, buf(s) + st.filled,
                         static_cast<unsigned>(st.target - st.filled),
                         st.win * window + st.filled, s);
      } else {
        st.ready = true;
      }
    }
  }
#endif
};

file_reader::file_reader(const std::filesystem::path& fspath,
                         const file_reader_options& opts)
    : impl_(new impl) {
#if defined(BLAKE3PP_IO_POSIX)
  const char* const path = fspath.c_str();  // native() is char-based here
#endif
  impl& im = *impl_;
  // Window: power-of-2 multiple of the chunk size so every full window is
  // a subtree-aligned unit; >= 64 KiB keeps O_DIRECT alignment trivial.
  im.window = std::bit_floor(std::max<std::size_t>(opts.window_bytes,
                                                   64 * 1024));
  im.qd = std::min(32u, std::max(2u, opts.queue_depth));

#if defined(BLAKE3PP_IO_POSIX)
  im.fd_plain = ::open(path, O_RDONLY | O_CLOEXEC);
  if (im.fd_plain < 0) {
    delete impl_;
    throw_errno("open");
  }
  struct stat st;
  if (::fstat(im.fd_plain, &st) != 0) {
    const int e = errno;
    ::close(im.fd_plain);
    delete impl_;
    throw std::system_error(e, std::generic_category(), "fstat");
  }
  im.size = static_cast<std::uint64_t>(st.st_size);
  im.fd = im.fd_plain;
#if defined(O_DIRECT)
  if (opts.direct_io) {
    const int dfd = ::open(path, O_RDONLY | O_CLOEXEC | O_DIRECT);
    if (dfd >= 0) {
      im.fd = dfd;
      im.direct = true;
    }
  }
#endif
  im.backend_name = im.direct ? "pread+direct" : "pread";
#if defined(BLAKE3PP_IO_URING)
  if (opts.async && im.ring.init(2 * im.qd)) {
    im.use_uring = true;
    im.backend_name = im.direct ? "io_uring+direct" : "io_uring";
  }
#endif
#else
  // Non-POSIX stdio stub; a real Windows backend would use the path's
  // native wide string with CreateFileW + IOCP.
  im.stream = std::fopen(fspath.string().c_str(), "rb");
  if (im.stream == nullptr) {
    delete impl_;
    throw_errno("fopen");
  }
  std::fseek(im.stream, 0, SEEK_END);
  im.size = static_cast<std::uint64_t>(std::ftell(im.stream));
  im.backend_name = "stdio";
#endif

  im.num_windows = (im.size + im.window - 1) / im.window;
  im.slots.resize(im.qd);
  im.pool_sz = static_cast<std::size_t>(im.qd) * im.window;
  im.pool = static_cast<std::byte*>(
      ::operator new(im.pool_sz, std::align_val_t{direct_align}));
  const std::uint64_t initial = std::min<std::uint64_t>(im.qd, im.num_windows);
  for (unsigned s = 0; s < initial; ++s) {
    im.assign(s);
  }
}

file_reader::~file_reader() {
  impl& im = *impl_;
#if defined(BLAKE3PP_IO_URING)
  im.ring.destroy();
#endif
  if (im.pool != nullptr) {
    ::operator delete(im.pool, std::align_val_t{direct_align});
  }
#if defined(BLAKE3PP_IO_POSIX)
  if (im.direct && im.fd >= 0) {
    ::close(im.fd);
  }
  if (im.fd_plain >= 0) {
    ::close(im.fd_plain);
  }
#else
  if (im.stream != nullptr) {
    std::fclose(im.stream);
  }
#endif
  delete impl_;
}

std::uint64_t file_reader::file_size() const noexcept { return impl_->size; }

const char* file_reader::backend() const noexcept {
  return impl_->backend_name;
}

std::optional<file_reader::window> file_reader::next() {
  impl& im = *impl_;
  if (im.next_deliver >= im.num_windows) {
    return std::nullopt;
  }
  const std::uint64_t want = im.next_deliver;
  unsigned s = 0;
  for (; s < im.qd; ++s) {
    if (im.slots[s].assigned && im.slots[s].win == want) {
      break;
    }
  }
  impl::slot_state& st = im.slots[s];
  if (!st.ready) {
#if defined(BLAKE3PP_IO_URING)
    if (im.use_uring && im.alignable(want)) {
      im.wait_async(want);
    } else {
      im.read_sync(s);
    }
#else
    im.read_sync(s);
#endif
  }
  st.held = true;
  im.next_deliver++;
  return window{im.buf(s), st.target, want * im.window,
                want + 1 == im.num_windows, s};
}

void file_reader::release(const window& w) noexcept {
  impl& im = *impl_;
  impl::slot_state& st = im.slots[w.slot];
  st.assigned = false;
  st.held = false;
  if (im.next_submit < im.num_windows) {
    im.assign(w.slot);
  }
}

}  // namespace blake3pp::detail
