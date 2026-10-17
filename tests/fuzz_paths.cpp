// libFuzzer harness: every way of feeding the same bytes must agree.
//
// The test vectors pin the algorithm at a few hundred lengths. What they
// cannot enumerate is the cross product this library actually has: where a
// caller splits its updates, whether the last piece went through update()
// or update_last(), whether a window-aligned run became a subtree or fell
// back, which mode the hasher is in, and whether extended output was taken
// in one call or seeked to. Each of those is a different path through the
// same tree, so the fuzzer's job here is not to find a wrong digest from
// nowhere -- it is to find two paths that disagree.
//
// The input steers the comparison and then becomes the message:
//
//   byte 0   the mode (plain, keyed, derive_key)
//   byte 1   how many bytes of extended output to cross-check
//   byte 2   the seek offset for the XOF comparison, in blocks
//   byte 3+  split sizes, taken until one runs past the end
//   rest     the message
//
// A mismatch aborts; so does a sanitizer finding, which is the other half
// of what this covers (the pipeline's staging buffers and the kernels'
// transposes under ASan and UBSan, at lengths nobody wrote a case for).
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#include <blake3pp/blake3pp.hpp>

namespace {

using bytes = std::span<const std::byte>;

[[noreturn]] void disagree(const char* what, std::size_t len) {
  std::fprintf(stderr, "blake3pp fuzz: %s disagrees at %zu bytes\n", what, len);
  std::abort();
}

void same(const char* what, bytes a, bytes b, std::size_t len) {
  if (a.size() != b.size() ||
      !std::equal(a.begin(), a.end(), b.begin())) {
    disagree(what, len);
  }
}

constexpr std::string_view fuzz_context = "blake3pp fuzz context v1";

// A key from the message itself, so the fuzzer's bytes reach the key
// schedule too; a message shorter than a key leaves the rest zero.
[[nodiscard]] std::array<std::byte, blake3pp::key_size> key_from(bytes msg) {
  std::array<std::byte, blake3pp::key_size> key{};
  for (std::size_t i = 0; i < key.size() && i < msg.size(); ++i) {
    key[i] = msg[i];
  }
  return key;
}

// A fresh hasher in this run's mode: every path below needs its own.
[[nodiscard]] blake3pp::hasher make(std::uint8_t mode, bytes msg) {
  switch (mode % 3) {
    case 1:
      return blake3pp::hasher::keyed(key_from(msg));
    case 2:
      return blake3pp::hasher::derive_key(fuzz_context);
    default:
      return blake3pp::hasher{};
  }
}

// The same mode's one-shot free function, which is a path of its own: it
// reaches update_last() through hash() rather than through a hasher the
// caller drives.
[[nodiscard]] blake3pp::digest one_shot(std::uint8_t mode, bytes msg) {
  switch (mode % 3) {
    case 1:
      return blake3pp::keyed_hash(key_from(msg), msg);
    case 2:
      return blake3pp::derive_key(fuzz_context, msg);
    default:
      return blake3pp::hash(msg);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  if (size < 4) {
    return 0;
  }
  const std::uint8_t mode = data[0];
  const std::size_t xof_len = 1 + (data[1] % 96);
  const std::uint64_t xof_seek =
      static_cast<std::uint64_t>(data[2]) * blake3pp::block_size;

  // The split sizes come out of the input, which is what makes the fuzzer
  // able to steer them: a run of small splits exercises the chunk-at-a-time
  // path, one large split the wide one.
  std::vector<std::size_t> splits;
  std::size_t at = 3;
  std::size_t planned = 0;
  while (at < size && planned < size - at) {
    const std::size_t piece = 1 + data[at];
    splits.push_back(piece);
    planned += piece;
    ++at;
    if (splits.size() == 16) {
      break;
    }
  }
  const bytes msg{reinterpret_cast<const std::byte*>(data + at), size - at};

  // 1. The reference for this input: one update, one finalize.
  blake3pp::hasher whole_hasher = make(mode, msg);
  whole_hasher.update(msg);
  const blake3pp::digest one = whole_hasher.finalize();

  // 2. The mode's one-shot, which takes the deep API's own short cut.
  same("one-shot", one.bytes, one_shot(mode, msg).bytes, msg.size());

  // 3. The splits the input described.
  blake3pp::hasher pieces = make(mode, msg);
  std::size_t done = 0;
  for (const std::size_t piece : splits) {
    const std::size_t take = std::min(piece, msg.size() - done);
    pieces.update(msg.subspan(done, take));
    done += take;
    if (done == msg.size()) {
      break;
    }
  }
  if (done < msg.size()) {
    pieces.update(msg.subspan(done));
  }
  const blake3pp::digest split = pieces.finalize();
  same("split updates", one.bytes, split.bytes, msg.size());

  // 4. update_last() on the final piece: the caller promising the message
  // ends there must not change the digest.
  blake3pp::hasher last = make(mode, msg);
  if (msg.size() > 64) {
    last.update(msg.first(msg.size() - 64));
    last.update_last(msg.last(64));
  } else {
    last.update_last(msg);
  }
  same("update_last", one.bytes, last.finalize().bytes, msg.size());

  // 5. Extended output: one call against a seeked reader.
  blake3pp::hasher xof = make(mode, msg);
  xof.update(msg);
  std::vector<std::byte> whole(xof_seek + xof_len);
  auto reader = xof.finalize_xof();
  reader.fill(whole);
  std::vector<std::byte> seeked(xof_len);
  auto jumped = xof.finalize_xof();
  jumped.seek(xof_seek);
  jumped.fill(seeked);
  same("seeked extended output",
       bytes{whole}.subspan(static_cast<std::size_t>(xof_seek), xof_len),
       seeked, msg.size());

  return 0;
}
