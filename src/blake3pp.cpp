#include <blake3pp/blake3pp.hpp>

#include <cstring>

#include "core/core.hpp"
#include "kernel/kernel.hpp"

namespace blake3pp {

hasher::hasher(arch a) noexcept : ops_(detail::resolve(a)), cv_stack_len_(0) {
  core::chunk_init(chunk_, kern::iv, 0);
}

void hasher::reset() noexcept {
  core::chunk_init(chunk_, kern::iv, 0);
  cv_stack_len_ = 0;
}

arch hasher::selected_arch() const noexcept {
  // The table's name string doubles as identity; avoids storing the enum.
  const char* n = ops_->name;
  if (std::strcmp(n, "sse42") == 0) return arch::sse42;
  if (std::strcmp(n, "avx2") == 0) return arch::avx2;
  if (std::strcmp(n, "avx512") == 0) return arch::avx512;
  if (std::strcmp(n, "neon") == 0) return arch::neon;
  return arch::scalar;
}

// Merge completed subtrees, then push: each trailing zero bit of
// total_chunks is a full binary subtree whose sibling is on the stack
// (spec 5.1.2).
void hasher::push_chunk_cv(const std::uint32_t cv[8],
                           std::uint64_t total_chunks) noexcept {
  std::uint32_t new_cv[8];
  std::memcpy(new_cv, cv, sizeof(new_cv));
  std::uint64_t chunks = total_chunks;
  while ((chunks & 1) == 0) {
    cv_stack_len_--;
    core::chaining_value(
        *ops_, core::parent_output(cv_stack_[cv_stack_len_], new_cv, kern::iv),
        new_cv);
    chunks >>= 1;
  }
  std::memcpy(cv_stack_[cv_stack_len_], new_cv, sizeof(new_cv));
  cv_stack_len_++;
}

void hasher::update(std::span<const std::byte> input) noexcept {
  const auto* p = reinterpret_cast<const std::uint8_t*>(input.data());
  std::size_t len = input.size();

  while (len > 0) {
    // A full chunk is only closed out when more input arrives: the final
    // chunk of the message must stay open for possible ROOT finalization.
    if (core::chunk_len(chunk_) == kern::chunk_len) {
      std::uint32_t chunk_cv[8];
      core::chaining_value(*ops_, core::chunk_output(chunk_), chunk_cv);
      const std::uint64_t total_chunks = chunk_.chunk_counter + 1;
      push_chunk_cv(chunk_cv, total_chunks);
      core::chunk_init(chunk_, kern::iv, total_chunks);
    }

    // SIMD fast path: aligned on a chunk boundary with more than one whole
    // chunk ahead, hand hash_many a batch of chunks. The strict > keeps at
    // least one byte out of the batch, so no batched chunk can turn out to
    // be the message's last (which would need CHUNK_END-with-ROOT handling).
    if (core::chunk_len(chunk_) == 0 && len > kern::chunk_len) {
      const std::size_t safe_chunks = (len - 1) / kern::chunk_len;
      const std::size_t n = safe_chunks < ops_->simd_degree
                                ? safe_chunks
                                : ops_->simd_degree;
      const std::uint8_t* ptrs[kern::max_simd_degree];
      std::uint8_t cvs[kern::max_simd_degree * kern::out_len];
      for (std::size_t j = 0; j < n; ++j) {
        ptrs[j] = p + j * kern::chunk_len;
      }
      ops_->hash_many(ptrs, n, kern::chunk_len / kern::block_len, kern::iv,
                      chunk_.chunk_counter, /*increment_counter=*/true, 0,
                      kern::flag_chunk_start, kern::flag_chunk_end, cvs);
      for (std::size_t j = 0; j < n; ++j) {
        std::uint32_t cv[8];
        for (std::size_t w = 0; w < 8; ++w) {
          const std::uint8_t* b = cvs + j * kern::out_len + 4 * w;
          cv[w] = static_cast<std::uint32_t>(b[0]) |
                  (static_cast<std::uint32_t>(b[1]) << 8) |
                  (static_cast<std::uint32_t>(b[2]) << 16) |
                  (static_cast<std::uint32_t>(b[3]) << 24);
        }
        push_chunk_cv(cv, chunk_.chunk_counter + j + 1);
      }
      chunk_.chunk_counter += n;
      p += n * kern::chunk_len;
      len -= n * kern::chunk_len;
      continue;
    }

    const std::size_t room = kern::chunk_len - core::chunk_len(chunk_);
    const std::size_t take = len < room ? len : room;
    core::chunk_update(*ops_, chunk_, p, take);
    p += take;
    len -= take;
  }
}

void hasher::update(std::string_view input) noexcept {
  update(std::as_bytes(std::span{input.data(), input.size()}));
}

digest hasher::finalize() const noexcept {
  // Collapse the stack from the top down; the last combination happens with
  // the ROOT flag. No member state is modified; finalize can be called at
  // any point and hashing may continue afterwards.
  core::output o = core::chunk_output(chunk_);
  std::size_t parents = cv_stack_len_;
  while (parents > 0) {
    parents--;
    std::uint32_t right_cv[8];
    core::chaining_value(*ops_, o, right_cv);
    o = core::parent_output(cv_stack_[parents], right_cv, kern::iv);
  }

  o.flags |= kern::flag_root;
  std::uint32_t root_cv[8];
  core::chaining_value(*ops_, o, root_cv);

  digest d;
  for (std::size_t w = 0; w < 8; ++w) {
    for (std::size_t b = 0; b < 4; ++b) {
      d.bytes[4 * w + b] = static_cast<std::byte>(root_cv[w] >> (8 * b));
    }
  }
  return d;
}

digest hash(std::span<const std::byte> input) noexcept {
  hasher h;
  h.update(input);
  return h.finalize();
}

digest hash(std::string_view input) noexcept {
  hasher h;
  h.update(input);
  return h.finalize();
}

std::string digest::to_hex() const {
  static constexpr char alphabet[] = "0123456789abcdef";
  std::string s;
  s.resize(bytes.size() * 2);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const auto v = static_cast<std::uint8_t>(bytes[i]);
    s[2 * i] = alphabet[v >> 4];
    s[2 * i + 1] = alphabet[v & 0xF];
  }
  return s;
}

}  // namespace blake3pp
