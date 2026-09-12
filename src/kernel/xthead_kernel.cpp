// T-Head XTheadVector kernel: the draft-RVV-0.7.1 encoding shipped in
// C906/C910 silicon (Allwinner D1, SG2042, TH1520...). Opt-in via
// -DBLAKE3PP_XTHEAD_KERNEL=ON; compiled with -march=rv64gc_xtheadvector
// through blake3pp_add_kernel's SOURCE argument.
//
// This is deliberately NOT an instantiation of kernel.cpp: XTheadVector
// types are sizeless in GCC's model (no fixed-vlen attribute exists for
// them, unlike RVV 1.0's riscv_rvv_vector_bits), so they cannot back the
// facade's u32v struct member, cannot form arrays, and cannot cross any
// aggregate boundary. What remains is the classic hand-written intrinsics
// style, sixteen named vector variables and a memory-staged message,
// faithfully the shape 0.7.1-era kernels had to take. GCC 14+ maps the
// standard __riscv_v* intrinsic names onto th.-prefixed encodings under
// this -march; only loads/stores need the __riscv_th_* spellings
// (riscv_th_vector.h).
//
// Width is fixed at 4: every 0.7.1 part is VLEN=128, and m1 u32 gives four
// lanes. Rotates are the 3-op shift-or; 0.7.1 has no vector rotate.
// Scalar entry points forward to the always-present scalar kernel.

#include "kernel/kernel.hpp"

#include <riscv_th_vector.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace blake3pp::kern::xthead {
namespace {

constexpr std::size_t W = 4;       // VLEN=128, u32 m1
constexpr std::size_t vl = W;

inline std::uint32_t load32(const std::uint8_t* p) noexcept {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof v);
  return v;  // RISC-V is little-endian; big-endian RV has no 0.7.1 parts
}

inline void store32(std::uint8_t* p, std::uint32_t v) noexcept {
  std::memcpy(p, &v, sizeof v);
}

inline vuint32m1_t ld(const std::uint32_t* p) noexcept {
  return __riscv_th_vlwu_v_u32m1(p, vl);
}
inline void st(std::uint32_t* p, vuint32m1_t v) noexcept {
  __riscv_th_vsw_v_u32m1(p, v, vl);
}
inline vuint32m1_t bcast(std::uint32_t x) noexcept {
  return __riscv_vmv_v_x_u32m1(x, vl);
}
inline vuint32m1_t vadd(vuint32m1_t a, vuint32m1_t b) noexcept {
  return __riscv_vadd_vv_u32m1(a, b, vl);
}
inline vuint32m1_t vxor(vuint32m1_t a, vuint32m1_t b) noexcept {
  return __riscv_vxor_vv_u32m1(a, b, vl);
}

// rot<N>(x ^ y): eor + srl + sll + or; no rotate instruction exists in
// 0.7.1 (Zvbb is an RVV-1.0-era extension).
template <int N>
inline vuint32m1_t xor_rot(vuint32m1_t x, vuint32m1_t y) noexcept {
  const vuint32m1_t e = vxor(x, y);
  return __riscv_vor_vv_u32m1(__riscv_vsrl_vx_u32m1(e, N, vl),
                              __riscv_vsll_vx_u32m1(e, 32 - N, vl), vl);
}

inline void g(vuint32m1_t& a, vuint32m1_t& b, vuint32m1_t& c, vuint32m1_t& d,
              vuint32m1_t mx, vuint32m1_t my) noexcept {
  a = vadd(vadd(a, b), mx);
  d = xor_rot<16>(d, a);
  c = vadd(c, d);
  b = xor_rot<12>(b, c);
  a = vadd(vadd(a, b), my);
  d = xor_rot<8>(d, a);
  c = vadd(c, d);
  b = xor_rot<7>(b, c);
}

// Same accumulated-permutation trick as kernel.cpp: msg_schedule[r][i] is
// the original message word round r reads at position i.
constexpr std::uint8_t msg_perm[16] = {2, 6,  3,  10, 7, 0,  4,  13,
                                       1, 11, 12, 5,  9, 14, 15, 8};

consteval std::array<std::array<std::uint8_t, 16>, 7> make_msg_schedule() {
  std::array<std::array<std::uint8_t, 16>, 7> s{};
  for (std::uint8_t i = 0; i < 16; ++i) {
    s[0][i] = i;
  }
  for (std::size_t r = 1; r < 7; ++r) {
    for (std::size_t i = 0; i < 16; ++i) {
      s[r][i] = s[r - 1][msg_perm[i]];
    }
  }
  return s;
}

constexpr auto msg_schedule = make_msg_schedule();

// Hashes exactly W inputs, one per lane. The message lives transposed in a
// memory staging array (sizeless types cannot form the m[16] register array
// kernel.cpp uses), so each G operand is one th.vlwu from staging; the
// 16 state words stay in registers across the whole block.
void hash_batch(const std::uint8_t* const* inputs, std::size_t blocks,
                const std::uint32_t key[8], std::uint64_t counter,
                bool increment_counter, std::uint32_t flags,
                std::uint32_t flags_start, std::uint32_t flags_end,
                std::uint8_t* out) noexcept {
  vuint32m1_t cv0 = bcast(key[0]);
  vuint32m1_t cv1 = bcast(key[1]);
  vuint32m1_t cv2 = bcast(key[2]);
  vuint32m1_t cv3 = bcast(key[3]);
  vuint32m1_t cv4 = bcast(key[4]);
  vuint32m1_t cv5 = bcast(key[5]);
  vuint32m1_t cv6 = bcast(key[6]);
  vuint32m1_t cv7 = bcast(key[7]);

  alignas(16) std::uint32_t ctr[2][W];
  for (std::size_t lane = 0; lane < W; ++lane) {
    const std::uint64_t c = counter + (increment_counter ? lane : 0);
    ctr[0][lane] = static_cast<std::uint32_t>(c);
    ctr[1][lane] = static_cast<std::uint32_t>(c >> 32);
  }
  const vuint32m1_t ctr_lo = ld(ctr[0]);
  const vuint32m1_t ctr_hi = ld(ctr[1]);

  alignas(16) std::uint32_t ms[16][W];

  for (std::size_t b = 0; b < blocks; ++b) {
    std::uint32_t block_flags = flags;
    if (b == 0) {
      block_flags |= flags_start;
    }
    if (b == blocks - 1) {
      block_flags |= flags_end;
    }

    for (std::size_t j = 0; j < 16; ++j) {
      for (std::size_t lane = 0; lane < W; ++lane) {
        ms[j][lane] = load32(inputs[lane] + b * block_len + 4 * j);
      }
    }

    vuint32m1_t v0 = cv0, v1 = cv1, v2 = cv2, v3 = cv3;
    vuint32m1_t v4 = cv4, v5 = cv5, v6 = cv6, v7 = cv7;
    vuint32m1_t v8 = bcast(iv[0]);
    vuint32m1_t v9 = bcast(iv[1]);
    vuint32m1_t v10 = bcast(iv[2]);
    vuint32m1_t v11 = bcast(iv[3]);
    vuint32m1_t v12 = ctr_lo;
    vuint32m1_t v13 = ctr_hi;
    vuint32m1_t v14 = bcast(block_len);
    vuint32m1_t v15 = bcast(block_flags);

    for (std::size_t r = 0; r < 7; ++r) {
      const auto& s = msg_schedule[r];
      // Columns.
      g(v0, v4, v8, v12, ld(ms[s[0]]), ld(ms[s[1]]));
      g(v1, v5, v9, v13, ld(ms[s[2]]), ld(ms[s[3]]));
      g(v2, v6, v10, v14, ld(ms[s[4]]), ld(ms[s[5]]));
      g(v3, v7, v11, v15, ld(ms[s[6]]), ld(ms[s[7]]));
      // Diagonals.
      g(v0, v5, v10, v15, ld(ms[s[8]]), ld(ms[s[9]]));
      g(v1, v6, v11, v12, ld(ms[s[10]]), ld(ms[s[11]]));
      g(v2, v7, v8, v13, ld(ms[s[12]]), ld(ms[s[13]]));
      g(v3, v4, v9, v14, ld(ms[s[14]]), ld(ms[s[15]]));
    }

    cv0 = vxor(v0, v8);
    cv1 = vxor(v1, v9);
    cv2 = vxor(v2, v10);
    cv3 = vxor(v3, v11);
    cv4 = vxor(v4, v12);
    cv5 = vxor(v5, v13);
    cv6 = vxor(v6, v14);
    cv7 = vxor(v7, v15);
  }

  alignas(16) std::uint32_t lanes[W];
  const auto emit = [&](std::size_t j, vuint32m1_t cv) {
    st(lanes, cv);
    for (std::size_t lane = 0; lane < W; ++lane) {
      store32(out + lane * out_len + 4 * j, lanes[lane]);
    }
  };
  emit(0, cv0);
  emit(1, cv1);
  emit(2, cv2);
  emit(3, cv3);
  emit(4, cv4);
  emit(5, cv5);
  emit(6, cv6);
  emit(7, cv7);
}

void hash_many(const std::uint8_t* const* inputs, std::size_t num_inputs,
               std::size_t blocks, const std::uint32_t key[8],
               std::uint64_t counter, bool increment_counter,
               std::uint32_t flags, std::uint32_t flags_start,
               std::uint32_t flags_end, std::uint8_t* out) noexcept {
  std::size_t i = 0;
  for (; i + W <= num_inputs; i += W) {
    hash_batch(inputs + i, blocks, key,
               counter + (increment_counter ? i : 0), increment_counter,
               flags, flags_start, flags_end, out + i * out_len);
  }
  if (i < num_inputs) {
    // Serial remainder (at most W-1 inputs): the scalar oracle has
    // identical semantics.
    scalar::ops.hash_many(inputs + i, num_inputs - i, blocks, key,
                          counter + (increment_counter ? i : 0),
                          increment_counter, flags, flags_start, flags_end,
                          out + i * out_len);
  }
}

// Fills W consecutive XOF output blocks: cv and message are broadcast,
// only the per-lane counter differs; the mirror image of hash_batch.
void xof_wide(const std::uint32_t cv[8], const std::uint8_t block[block_len],
              std::uint32_t len, std::uint64_t counter, std::uint32_t flags,
              std::uint8_t* out) noexcept {
  alignas(16) std::uint32_t ms[16][W];
  for (std::size_t j = 0; j < 16; ++j) {
    const std::uint32_t w = load32(block + 4 * j);
    for (std::size_t lane = 0; lane < W; ++lane) {
      ms[j][lane] = w;
    }
  }

  alignas(16) std::uint32_t ctr[2][W];
  for (std::size_t lane = 0; lane < W; ++lane) {
    const std::uint64_t c = counter + lane;
    ctr[0][lane] = static_cast<std::uint32_t>(c);
    ctr[1][lane] = static_cast<std::uint32_t>(c >> 32);
  }

  vuint32m1_t v0 = bcast(cv[0]);
  vuint32m1_t v1 = bcast(cv[1]);
  vuint32m1_t v2 = bcast(cv[2]);
  vuint32m1_t v3 = bcast(cv[3]);
  vuint32m1_t v4 = bcast(cv[4]);
  vuint32m1_t v5 = bcast(cv[5]);
  vuint32m1_t v6 = bcast(cv[6]);
  vuint32m1_t v7 = bcast(cv[7]);
  vuint32m1_t v8 = bcast(iv[0]);
  vuint32m1_t v9 = bcast(iv[1]);
  vuint32m1_t v10 = bcast(iv[2]);
  vuint32m1_t v11 = bcast(iv[3]);
  vuint32m1_t v12 = ld(ctr[0]);
  vuint32m1_t v13 = ld(ctr[1]);
  vuint32m1_t v14 = bcast(len);
  vuint32m1_t v15 = bcast(flags);

  for (std::size_t r = 0; r < 7; ++r) {
    const auto& s = msg_schedule[r];
    g(v0, v4, v8, v12, ld(ms[s[0]]), ld(ms[s[1]]));
    g(v1, v5, v9, v13, ld(ms[s[2]]), ld(ms[s[3]]));
    g(v2, v6, v10, v14, ld(ms[s[4]]), ld(ms[s[5]]));
    g(v3, v7, v11, v15, ld(ms[s[6]]), ld(ms[s[7]]));
    g(v0, v5, v10, v15, ld(ms[s[8]]), ld(ms[s[9]]));
    g(v1, v6, v11, v12, ld(ms[s[10]]), ld(ms[s[11]]));
    g(v2, v7, v8, v13, ld(ms[s[12]]), ld(ms[s[13]]));
    g(v3, v4, v9, v14, ld(ms[s[14]]), ld(ms[s[15]]));
  }

  alignas(16) std::uint32_t lanes[W];
  const auto emit = [&](std::size_t j, vuint32m1_t w) {
    st(lanes, w);
    for (std::size_t lane = 0; lane < W; ++lane) {
      store32(out + lane * 64 + 4 * j, lanes[lane]);
    }
  };
  emit(0, vxor(v0, v8));
  emit(1, vxor(v1, v9));
  emit(2, vxor(v2, v10));
  emit(3, vxor(v3, v11));
  emit(4, vxor(v4, v12));
  emit(5, vxor(v5, v13));
  emit(6, vxor(v6, v14));
  emit(7, vxor(v7, v15));
  emit(8, vxor(v8, bcast(cv[0])));
  emit(9, vxor(v9, bcast(cv[1])));
  emit(10, vxor(v10, bcast(cv[2])));
  emit(11, vxor(v11, bcast(cv[3])));
  emit(12, vxor(v12, bcast(cv[4])));
  emit(13, vxor(v13, bcast(cv[5])));
  emit(14, vxor(v14, bcast(cv[6])));
  emit(15, vxor(v15, bcast(cv[7])));
}

void xof_many(const std::uint32_t cv[8], const std::uint8_t block[block_len],
              std::uint32_t len, std::uint64_t counter, std::uint32_t flags,
              std::uint8_t* out, std::size_t num_blocks) noexcept {
  std::size_t i = 0;
  for (; i + W <= num_blocks; i += W) {
    xof_wide(cv, block, len, counter + i, flags, out + i * 64);
  }
  if (i < num_blocks) {
    scalar::ops.xof_many(cv, block, len, counter + i, flags, out + i * 64,
                         num_blocks - i);
  }
}

// The scalar single-block entry points have identical semantics to the
// scalar oracle, so forward: one extra predicted call on paths that are
// not the wide workhorse anyway.
void compress_in_place(std::uint32_t cv[8],
                       const std::uint8_t block[block_len], std::uint32_t len,
                       std::uint64_t counter, std::uint32_t flags) noexcept {
  scalar::ops.compress_in_place(cv, block, len, counter, flags);
}

void compress_xof(const std::uint32_t cv[8],
                  const std::uint8_t block[block_len], std::uint32_t len,
                  std::uint64_t counter, std::uint32_t flags,
                  std::uint8_t out[64]) noexcept {
  scalar::ops.compress_xof(cv, block, len, counter, flags, out);
}

}  // namespace

// As in kernel.cpp: the staging buffers this build sizes from
// BLAKE3PP_MAX_SIMD_DEGREE must hold this variant's batches.
static_assert(W <= max_simd_degree);

extern const kernel_ops ops;
const kernel_ops ops = {
    arch::xthead,
    /*simd_degree=*/W,
    &compress_in_place,
    &compress_xof,
    &xof_many,
    &hash_many,
};

}  // namespace blake3pp::kern::xthead
