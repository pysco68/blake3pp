#include <blake3pp/core.hpp>

#include <algorithm>
#include <bit>

#include "core/core.hpp"
#include "core/subtree.hpp"
#include "kernel/kernel.hpp"

namespace blake3pp {

namespace {

// BLAKE3 keys enter as 32 little-endian bytes and live as 8 words.
std::array<std::uint32_t, 8> key_bytes_to_words(
    std::span<const std::byte, 32> key) noexcept {
  std::array<std::uint32_t, 8> words;
  for (std::size_t w = 0; w < 8; ++w) {
    const auto b = [&](std::size_t i) {
      return std::to_integer<std::uint32_t>(key[4 * w + i]);
    };
    words[w] = b(0) | (b(1) << 8) | (b(2) << 16) | (b(3) << 24);
  }
  return words;
}

}  // namespace

hasher::hasher(arch a) noexcept : hasher(detail::resolve(a)) {}

hasher::hasher(const kern::kernel_ops* custom_ops) noexcept
    : hasher(custom_ops, kern::iv, 0) {}

hasher::hasher(const kern::kernel_ops* ops,
               std::span<const std::uint32_t, 8> key,
               std::uint32_t base_flags) noexcept
    : ops_(ops), base_flags_(base_flags), cv_stack_len_(0) {
  std::ranges::copy(key, key_words_.begin());
  core::chunk_init(chunk_, key_words_, 0);
}

hasher hasher::keyed(std::span<const std::byte, 32> key, arch a) noexcept {
  return keyed(key, detail::resolve(a));
}

hasher hasher::keyed(std::span<const std::byte, 32> key,
                     const kern::kernel_ops* ops) noexcept {
  return hasher(ops, key_bytes_to_words(key), kern::flag_keyed_hash);
}

hasher hasher::derive_key(std::string_view context, arch a) noexcept {
  const kern::kernel_ops* const ops = detail::resolve(a);
  // Stage 1: hash the context string in DERIVE_KEY_CONTEXT mode...
  hasher ctx(ops, kern::iv, kern::flag_derive_key_context);
  ctx.update(context);
  const digest context_key = ctx.finalize();
  // ...stage 2: the returned hasher consumes key material keyed by it.
  return hasher(ops, key_bytes_to_words(context_key.bytes),
                kern::flag_derive_key_material);
}

void hasher::reset() noexcept {
  core::chunk_init(chunk_, key_words_, 0);
  cv_stack_len_ = 0;
}

arch hasher::selected_arch() const noexcept { return ops_->variant; }

std::uint64_t hasher::count() const noexcept {
  return chunk_.chunk_counter * kern::chunk_len + core::chunk_len(chunk_);
}

// Merge completed subtrees, then push. The pushed CV may itself be the root
// of a subtree_chunks-sized (power-of-2, aligned) subtree: counting in
// subtree units, each trailing zero bit of total_chunks/subtree_chunks is a
// full sibling subtree waiting on the stack (spec 5.1.2). The invariant
// afterwards is stack_len == popcount(total_chunks).
void hasher::push_cv(std::span<const std::uint32_t, 8> cv,
                     std::uint64_t total_chunks,
                     std::uint64_t subtree_chunks) noexcept {
  std::array<std::uint32_t, 8> new_cv;
  std::ranges::copy(cv, new_cv.begin());
  std::uint64_t chunks = total_chunks / subtree_chunks;
  while ((chunks & 1) == 0) {
    cv_stack_len_--;
    core::chaining_value(*ops_,
                         core::parent_output(cv_stack_[cv_stack_len_], new_cv,
                                             key_words_, base_flags_),
                         new_cv);
    chunks >>= 1;
  }
  cv_stack_[cv_stack_len_] = new_cv;
  cv_stack_len_++;
}

void hasher::update(std::span<const std::byte> input) noexcept {
  const auto* p = reinterpret_cast<const std::uint8_t*>(input.data());
  std::size_t len = input.size();

  while (len > 0) {
    // A full chunk is only closed out when more input arrives: the final
    // chunk of the message must stay open for possible ROOT finalization.
    if (core::chunk_len(chunk_) == kern::chunk_len) {
      std::array<std::uint32_t, 8> chunk_cv;
      core::chaining_value(*ops_, core::chunk_output(chunk_, base_flags_),
                           chunk_cv);
      const std::uint64_t total_chunks = chunk_.chunk_counter + 1;
      push_cv(chunk_cv, total_chunks, 1);
      core::chunk_init(chunk_, key_words_, total_chunks);
    }

    // Subtree fast path: aligned on a chunk boundary with more than one
    // whole chunk ahead, reduce the largest power-of-2, position-aligned
    // subtree in one wide pass (chunks AND parents lanes-parallel). The
    // strict > keeps at least one byte in reserve, so no offloaded chunk
    // can turn out to be the message's last (which would need
    // CHUNK_END-with-ROOT handling).
    if (core::chunk_len(chunk_) == 0 && len > kern::chunk_len) {
      const std::size_t safe_chunks = (len - 1) / kern::chunk_len;
      std::size_t subtree = std::bit_floor(safe_chunks);
      // A subtree merged as one CV must sit on a subtree-aligned position
      // in the overall tree.
      while (((subtree - 1) & chunk_.chunk_counter) != 0) {
        subtree /= 2;
      }
      if (subtree >= 2) {
        std::array<std::uint32_t, 8> cv;
        core::compress_subtree_to_cv(*ops_, p, subtree, chunk_.chunk_counter,
                                     key_words_, base_flags_, cv);
        push_cv(cv, chunk_.chunk_counter + subtree, subtree);
        chunk_.chunk_counter += subtree;
        p += subtree * kern::chunk_len;
        len -= subtree * kern::chunk_len;
        continue;
      }
      // A single (or misaligned) chunk falls through to the buffered path.
    }

    const std::size_t room = kern::chunk_len - core::chunk_len(chunk_);
    const std::size_t take = len < room ? len : room;
    core::chunk_update(*ops_, chunk_, p, take, base_flags_);
    p += take;
    len -= take;
  }
}

void hasher::update(std::string_view input) noexcept {
  update(std::as_bytes(std::span{input.data(), input.size()}));
}

output_reader hasher::finalize_xof() const noexcept {
  // Collapse the stack from the top down, then capture the ROOT node
  // instead of compressing it: the reader re-compresses it with varying
  // output counters. No member state is modified, so finalization can
  // happen at any point and hashing may continue afterwards.
  core::output o = core::chunk_output(chunk_, base_flags_);
  std::size_t parents = cv_stack_len_;
  while (parents > 0) {
    parents--;
    std::array<std::uint32_t, 8> right_cv;
    core::chaining_value(*ops_, o, right_cv);
    o = core::parent_output(cv_stack_[parents], right_cv, key_words_,
                            base_flags_);
  }

  output_reader r;
  r.ops_ = ops_;
  r.input_cv_ = o.input_cv;
  r.block_ = o.block;
  r.block_len_ = o.block_len;
  r.flags_ = o.flags | kern::flag_root;
  return r;
}

// The 32-byte digest is, by definition, the first 32 bytes of the output
// stream.
digest hasher::finalize() const noexcept {
  digest d;
  finalize_xof().fill(std::span<std::byte>{d.bytes});
  return d;
}

void hasher::finalize(std::span<std::byte> out) const noexcept {
  finalize_xof().fill(out);
}

void output_reader::fill(std::span<std::byte> out) noexcept {
  std::size_t done = 0;
  while (done < out.size()) {
    const std::uint64_t block_index = position_ / kern::block_len;
    const std::size_t in_block =
        static_cast<std::size_t>(position_ % kern::block_len);
    // Block-aligned bulk of the request goes lanes-wide: output counters
    // map to SIMD lanes, so long fills run at hash_many-class speed.
    if (in_block == 0 && out.size() - done >= kern::block_len) {
      const std::size_t nblocks = (out.size() - done) / kern::block_len;
      ops_->xof_many(input_cv_.data(), block_.data(), block_len_,
                     block_index, flags_,
                     reinterpret_cast<std::uint8_t*>(out.data() + done),
                     nblocks);
      done += nblocks * kern::block_len;
      position_ += nblocks * kern::block_len;
      continue;
    }
    if (!cache_valid_ || cached_block_ != block_index) {
      // uint8_t view of the byte cache at the flat kernel ABI boundary.
      ops_->compress_xof(input_cv_.data(), block_.data(), block_len_,
                         block_index, flags_,
                         reinterpret_cast<std::uint8_t*>(cache_.data()));
      cached_block_ = block_index;
      cache_valid_ = true;
    }
    const std::size_t take =
        std::min(out.size() - done, kern::block_len - in_block);
    std::copy_n(cache_.data() + in_block, take, out.data() + done);
    done += take;
    position_ += take;
  }
}

void hasher::push_subtree_cv(std::span<const std::uint32_t, 8> cv,
                             std::uint64_t subtree_chunks) noexcept {
  push_cv(cv, chunk_.chunk_counter + subtree_chunks, subtree_chunks);
  chunk_.chunk_counter += subtree_chunks;
}

namespace detail {
void compress_subtree_cv(const kern::kernel_ops* ops, const std::byte* data,
                         std::size_t num_chunks, std::uint64_t chunk_counter,
                         std::span<const std::uint32_t, 8> key,
                         std::uint32_t base_flags,
                         std::span<std::uint32_t, 8> out_cv) noexcept {
  core::compress_subtree_to_cv(*ops,
                               reinterpret_cast<const std::uint8_t*>(data),
                               num_chunks, chunk_counter, key, base_flags,
                               out_cv);
}
}  // namespace detail

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

digest keyed_hash(std::span<const std::byte, 32> key,
                  std::span<const std::byte> input) noexcept {
  hasher h = hasher::keyed(key);
  h.update(input);
  return h.finalize();
}

digest keyed_hash(std::span<const std::byte, 32> key,
                  std::string_view input) noexcept {
  return keyed_hash(key, std::as_bytes(std::span{input.data(), input.size()}));
}

digest derive_key(std::string_view context,
                  std::span<const std::byte> key_material) noexcept {
  hasher h = hasher::derive_key(context);
  h.update(key_material);
  return h.finalize();
}

digest derive_key(std::string_view context,
                  std::string_view key_material) noexcept {
  return derive_key(context, std::as_bytes(std::span{key_material.data(),
                                                     key_material.size()}));
}

std::optional<digest> digest::from_hex(std::string_view hex) noexcept {
  if (hex.size() != 64) {
    return std::nullopt;
  }
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  digest d;
  for (std::size_t i = 0; i < 32; ++i) {
    const int hi = nibble(hex[2 * i]);
    const int lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    d.bytes[i] = static_cast<std::byte>((hi << 4) | lo);
  }
  return d;
}

bool digest::matches(std::string_view hex) const noexcept {
  return from_hex(hex) == *this;  // optional's heterogeneous, CT inner ==
}

std::array<char, 65> digest::to_hex_chars() const noexcept {
  static constexpr char alphabet[] = "0123456789abcdef";
  std::array<char, 65> out;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const auto v = std::to_integer<unsigned>(bytes[i]);
    out[2 * i] = alphabet[v >> 4];
    out[2 * i + 1] = alphabet[v & 0xF];
  }
  out[64] = '\0';
  return out;
}

std::string digest::to_hex() const {
  const auto chars = to_hex_chars();
  return std::string{chars.data(), 64};
}

bool operator==(const digest& lhs, const digest& rhs) noexcept {
  // Accumulate the whole difference before deciding: no data-dependent
  // early exit, so comparison time is independent of where bytes differ.
  unsigned acc = 0;
  for (std::size_t i = 0; i < lhs.bytes.size(); ++i) {
    acc |= std::to_integer<unsigned>(lhs.bytes[i] ^ rhs.bytes[i]);
  }
  return acc == 0;
}

}  // namespace blake3pp
