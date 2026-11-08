#include <blake3pp/core.hpp>

#include <blake3pp/detail/contract.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "core/core.hpp"
#include "core/subtree.hpp"
#include "kernel/kernel.hpp"

namespace blake3pp {

namespace {

// BLAKE3 keys enter as 32 little-endian bytes and live as 8 words.
std::array<std::uint32_t, 8> key_bytes_to_words(
    std::span<const std::byte, 32> key) noexcept {
  std::array<std::uint32_t, 8> words;
  core::load_cv_le(kern::kernel_bytes(key.data()), words);
  return words;
}


}  // namespace

hasher::hasher(arch a) noexcept : hasher(detail::resolve(a)) {}

hasher::hasher(const kern::kernel_ops* custom_ops) noexcept
    : hasher(custom_ops, kern::iv, 0) {}

hasher::hasher(const kern::kernel_ops* ops,
               std::span<const std::uint32_t, 8> key,
               std::uint32_t base_flags) noexcept
    : ops_(ops), base_flags_(base_flags), cv_stack_len_(0),
      right_held_(false), right_chunks_(0) {
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
  return derive_key(context, detail::resolve(a));
}

hasher hasher::derive_key(std::string_view context,
                          const kern::kernel_ops* ops) noexcept {
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
  right_held_ = false;
  right_chunks_ = 0;
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
  // The arguments were checked where they entered, which is
  // push_subtree_cv() below: this one is also on update()'s path, where the
  // values are the hasher's own and the function must stay noexcept.
  // Violating them is not a graceful failure -- subtree_chunks == 0 divides
  // by zero here, and a misaligned or non-power-of-two size produces more
  // trailing zero bits than there are stacked siblings, wrapping the
  // uint8_t length to 255 and indexing a 54-entry array out of bounds.
  assert(subtree_chunks > 0 && std::has_single_bit(subtree_chunks));
  assert(total_chunks % subtree_chunks == 0);
  std::array<std::uint32_t, 8> new_cv;
  core::copy_cv(cv, new_cv);
  std::uint64_t chunks = total_chunks / subtree_chunks;
  while ((chunks & 1) == 0) {
    assert(cv_stack_len_ > 0);  // a sibling must be waiting for each merge
    cv_stack_len_--;
    core::chaining_value(*ops_,
                         core::parent_output(cv_stack_[cv_stack_len_], new_cv,
                                             key_words_, base_flags_),
                         new_cv);
    chunks >>= 1;
  }
  assert(cv_stack_len_ < cv_stack_.size());
  cv_stack_[cv_stack_len_] = new_cv;
  cv_stack_len_++;
}

// A full chunk is only closed out when more input arrives: the final
// chunk of the message must stay open for possible ROOT finalization. So
// "on a chunk boundary" has two shapes, an empty chunk state or a full
// one, and every path that appends after the boundary (update() and
// push_subtree_cv()) starts by collapsing the second into the first.
void hasher::close_full_chunk() noexcept {
  if (core::chunk_len(chunk_) == kern::chunk_len) {
    std::array<std::uint32_t, 8> chunk_cv;
    core::chaining_value(*ops_, core::chunk_output(chunk_, base_flags_),
                         chunk_cv);
    const std::uint64_t total_chunks = chunk_.chunk_counter + 1;
    push_cv(chunk_cv, total_chunks, 1);
    core::chunk_init(chunk_, key_words_, total_chunks);
  }
}

// update_last() left the message's rightmost subtree out of the stack so
// finalization could make it the root parent's right child without
// recompressing it. The held CV is a complete, aligned, left-complete
// subtree at its position -- BLAKE3 compresses a full chunk the same way
// whether or not the message continues -- so putting it on the stack
// restores exactly the state update() would have reached. A further update
// therefore costs the batching, not the digest.
void hasher::resume_held() noexcept {
  if (right_held_) {
    right_held_ = false;
    push_cv(right_cv_, chunk_.chunk_counter, right_chunks_);
  }
}

// Empty input returns at once, here and in update_last(): resume_held()
// and close_full_chunk() assume bytes follow, and either one run without
// them changes the tree finalize() sees.
void hasher::update(std::span<const std::byte> input) noexcept {
  if (input.empty()) {
    return;
  }
  resume_held();
  const auto* p = kern::kernel_bytes(input.data());
  std::size_t len = input.size();

  while (len > 0) {
    close_full_chunk();

    // Wide path: aligned on a chunk boundary with more than one whole chunk
    // ahead, every chunk but the one holding the last byte is reduced away.
    // The strict > keeps that byte in reserve, so no offloaded chunk can turn
    // out to be the message's last (which would need CHUNK_END-with-ROOT
    // handling). How the run is batched is compress_chunks' business.
    if (core::chunk_len(chunk_) == 0 && len > kern::chunk_len) {
      const std::size_t offload = (len - 1) / kern::chunk_len;
      core::compress_chunks(
          *ops_, p, offload, chunk_.chunk_counter, key_words_, base_flags_,
          [this](std::span<const std::uint32_t, 8> cv,
                 std::uint64_t subtree_chunks) noexcept {
            push_cv(cv, chunk_.chunk_counter + subtree_chunks, subtree_chunks);
            chunk_.chunk_counter += subtree_chunks;
          });
      p += offload * kern::chunk_len;
      len -= offload * kern::chunk_len;
      continue;
    }

    const std::size_t room = kern::chunk_len - core::chunk_len(chunk_);
    const std::size_t take = len < room ? len : room;
    core::chunk_update(*ops_, chunk_, p, take, base_flags_);
    p += take;
    len -= take;
  }
}

void hasher::update(std::string_view input) noexcept {
  update(detail::bytes_of(input));
}

void hasher::update_last(std::span<const std::byte> input) noexcept {
  if (input.empty()) {
    return;
  }
  resume_held();
  close_full_chunk();
  const std::size_t len = input.size();
  // Only a message ending on a chunk boundary has a final chunk that could
  // have been batched. Anything else ends inside a chunk, which is serial
  // in any implementation, and update() already batches every full chunk
  // ahead of it.
  if (core::chunk_len(chunk_) != 0 || len < 2 * kern::chunk_len ||
      len % kern::chunk_len != 0) {
    update(input);
    return;
  }

  const std::size_t num_chunks = len / kern::chunk_len;
  const std::uint64_t first = chunk_.chunk_counter;
  // Emission by emission, one behind: everything but the last subtree goes
  // on the stack, and the last stays in right_cv_ as the root parent's
  // right child. Capping an emission below the run guarantees there is a
  // last one to hold and a sibling for it.
  std::array<std::uint32_t, 8> held;
  std::uint64_t held_chunks = 0;
  std::uint64_t pushed = 0;
  core::compress_chunks(
      *ops_, kern::kernel_bytes(input.data()), num_chunks, first, key_words_,
      base_flags_,
      [&](std::span<const std::uint32_t, 8> cv,
          std::uint64_t subtree_chunks) noexcept {
        if (held_chunks != 0) {
          pushed += held_chunks;
          push_cv(held, first + pushed, held_chunks);
        }
        core::copy_cv(cv, held);
        held_chunks = subtree_chunks;
      },
      std::bit_floor(num_chunks - 1));
  assert(held_chunks != 0 && cv_stack_len_ > 0);
  core::copy_cv(held, right_cv_);
  right_held_ = true;
  right_chunks_ = held_chunks;
  // The chunk state is spent, but count() still owes every absorbed byte.
  chunk_.chunk_counter = first + num_chunks;
}

void hasher::update_last(std::string_view input) noexcept {
  update_last(detail::bytes_of(input));
}

output_reader hasher::finalize_xof() const noexcept {
  // Collapse the stack from the top down, then capture the ROOT node
  // instead of compressing it: the reader re-compresses it with varying
  // output counters. No member state is modified, so finalization can
  // happen at any point and hashing may continue afterwards.
  //
  // The rightmost node is the open chunk, or the subtree update_last() kept
  // off the stack once the message is known to have ended on a chunk
  // boundary. Both reach the same spine; what differs is which node the
  // message's last bytes sit in.
  std::size_t parents = cv_stack_len_;
  core::output o{};
  if (right_held_) {
    parents--;
    o = core::parent_output(cv_stack_[parents], right_cv_, key_words_,
                            base_flags_);
  } else {
    o = core::chunk_output(chunk_, base_flags_);
  }
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
    // Block-aligned bulk of the request goes lanes-wide: output counters
    // map to SIMD lanes, so long fills run at hash_many-class speed.
    if (block_offset_ == 0 && out.size() - done >= kern::block_len) {
      const std::size_t nblocks = (out.size() - done) / kern::block_len;
      ops_->xof_many(input_cv_.data(), block_.data(), block_len_,
                     block_counter_, flags_,
                     kern::kernel_bytes(out.data() + done),
                     nblocks);
      done += nblocks * kern::block_len;
      block_counter_ += nblocks;
      continue;
    }
    if (!cache_valid_ || cached_block_ != block_counter_) {
      // uint8_t view of the byte cache at the flat kernel ABI boundary.
      ops_->compress_xof(input_cv_.data(), block_.data(), block_len_,
                         block_counter_, flags_,
                         kern::kernel_bytes(cache_.data()));
      cached_block_ = block_counter_;
      cache_valid_ = true;
    }
    // Explicit std::size_t: kern::block_len is uint32_t, so on a 32-bit
    // target the two arguments deduce to different types and the call is
    // ambiguous.  Both values are bounded by out.size() and 64.
    const std::size_t take = std::min<std::size_t>(
        out.size() - done, kern::block_len - block_offset_);
    std::copy_n(cache_.data() + block_offset_, take, out.data() + done);
    done += take;
    advance(take);
  }
}

void hasher::push_subtree_cv(std::span<const std::uint32_t, 8> cv,
                             std::uint64_t subtree_chunks) {
  resume_held();  // the message did not end at update_last() after all
  // The expert entry point's arguments, checked in every build: push_cv()
  // below cannot do it, because update() reaches it too and is noexcept.
  detail::require<std::invalid_argument>(
      subtree_chunks > 0 && std::has_single_bit(subtree_chunks),
      "push_subtree_cv(): a subtree size that is not a power of two");
  close_full_chunk();
  // The documented chunk-boundary precondition, checked in every build for
  // the same reason as the two in push_cv(): close_full_chunk() ends a
  // chunk that is complete and leaves a partial one alone, so bytes left
  // in the chunk state here would be hashed as the head of a chunk past
  // the subtree -- a wrong digest with nothing to see.
  detail::require<std::invalid_argument>(
      core::chunk_len(chunk_) == 0,
      "push_subtree_cv(): a hasher part-way through a chunk");
  detail::require<std::invalid_argument>(
      (chunk_.chunk_counter + subtree_chunks) % subtree_chunks == 0,
      "push_subtree_cv(): a subtree at a position it cannot cover");
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
                               kern::kernel_bytes(data),
                               num_chunks, chunk_counter, key, base_flags,
                               out_cv);
}

std::size_t compress_subtree_cvs(const kern::kernel_ops* ops,
                                 const std::byte* data, std::size_t num_chunks,
                                 std::uint64_t chunk_counter,
                                 std::span<const std::uint32_t, 8> key,
                                 std::uint32_t base_flags,
                                 std::span<std::array<std::uint32_t, 8>> out_cvs) noexcept {
  return core::compress_subtree_to_cvs(*ops, kern::kernel_bytes(data),
                                       num_chunks, chunk_counter, key,
                                       base_flags, out_cvs);
}

std::size_t simd_degree(const kern::kernel_ops* ops) noexcept {
  return ops->simd_degree;
}

void fold_sibling_cvs(const kern::kernel_ops* ops,
                      std::span<std::array<std::uint32_t, 8>> cvs,
                      std::span<const std::uint32_t, 8> key,
                      std::uint32_t base_flags,
                      std::span<std::uint32_t, 8> out_cv) {
  core::fold_sibling_cvs_wide<>(*ops, cvs, key, base_flags, out_cv);
}

void parent_cv(const kern::kernel_ops* ops,
               std::span<const std::uint32_t, 8> left,
               std::span<const std::uint32_t, 8> right,
               std::span<const std::uint32_t, 8> key,
               std::uint32_t base_flags,
               std::span<std::uint32_t, 8> out_cv) noexcept {
  // The parent block carries copies of both children, so the compression
  // reads nothing through out_cv and writing it last makes aliasing with
  // either child safe.
  const core::output o = core::parent_output(left, right, key, base_flags);
  std::array<std::uint32_t, 8> cv;
  core::chaining_value(*ops, o, cv);
  core::copy_cv(cv, out_cv);
}
}  // namespace detail

digest hash(std::span<const std::byte> input) noexcept {
  hasher h;
  h.update_last(input);
  return h.finalize();
}

digest hash(std::string_view input) noexcept {
  return hash(detail::bytes_of(input));
}

digest keyed_hash(std::span<const std::byte, 32> key,
                  std::span<const std::byte> input) noexcept {
  hasher h = hasher::keyed(key);
  h.update_last(input);
  return h.finalize();
}

digest keyed_hash(std::span<const std::byte, 32> key,
                  std::string_view input) noexcept {
  return keyed_hash(key, detail::bytes_of(input));
}

digest derive_key(std::string_view context,
                  std::span<const std::byte> key_material) noexcept {
  hasher h = hasher::derive_key(context);
  h.update_last(key_material);
  return h.finalize();
}

digest derive_key(std::string_view context,
                  std::string_view key_material) noexcept {
  return derive_key(context, detail::bytes_of(key_material));
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

void to_hex(std::span<const std::byte> bytes, std::span<char> out) {
  static constexpr char alphabet[] = "0123456789abcdef";
  // The loop below writes two characters per byte whatever the span says,
  // so a span too small for them is a write past the end of a caller's
  // buffer -- with lengths that can come from a file or a wire. Checked in
  // every build.
  detail::require<std::length_error>(
      out.size() >= 2 * bytes.size(),
      "to_hex(): an output span smaller than two characters per byte");
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const auto v = std::to_integer<unsigned>(bytes[i]);
    out[2 * i] = alphabet[v >> 4];
    out[2 * i + 1] = alphabet[v & 0xF];
  }
}

std::string to_hex(std::span<const std::byte> bytes) {
  std::string s(2 * bytes.size(), '\0');
  to_hex(bytes, s);
  return s;
}

std::array<char, 65> digest::to_hex_chars() const noexcept {
  std::array<char, 65> out;
  blake3pp::to_hex(bytes, out);
  out[64] = '\0';
  return out;
}

std::string digest::to_hex() const { return blake3pp::to_hex(bytes); }

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
