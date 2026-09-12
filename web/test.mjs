// The module's node check: the empty-input vector, then one message
// whose digest every path must reproduce: the one-shots and the two
// hashers, sequential and pooled, streamed in uneven pieces, each
// compiled variant pinned; and the extended output round trip.
import { pathToFileURL } from 'node:url';

const modulePath = process.argv[2];
if (!modulePath) {
  console.error('usage: node test.mjs <blake3pp-web-*.mjs>');
  process.exit(2);
}
const { default: createBlake3pp } = await import(pathToFileURL(modulePath).href);
const b3pp = await createBlake3pp();

let failures = 0;
const check = (what, got, want) => {
  if (got !== want) {
    console.error(`FAIL ${what}: got ${got}, want ${want}`);
    failures++;
  }
};
const EMPTY = 'af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262';

console.log(`blake3pp ${b3pp.version()}, ${b3pp.simd_provider()}, ${b3pp.execution_provider()},`,
            `compiled ${b3pp.compiled_arches().join(' ')}, available ${b3pp.available_arches().join(' ')},`,
            `auto -> ${b3pp.best_available()}`);

check('empty input', b3pp.hash(0, 0, 'auto'), EMPTY);
check('empty text', (() => { const h = new b3pp.hasher('auto'); h.update(''); const d = h.finalize(); h.delete(); return d; })(), EMPTY);

// The message: 4 MiB + a partial chunk of extended output, written into
// the module's memory the way an embedder does.
const len = 4 * 1024 * 1024 + 777;
const buf = b3pp._malloc(len);
if (!buf) throw new Error('malloc failed');
{
  const seed = new b3pp.hasher('auto');
  seed.update('blake3pp web test');
  const r = seed.finalize_xof();
  r.fill(buf, len);
  check('reader position', r.position(), len);
  r.seek(0);
  const again = b3pp._malloc(64);
  r.fill(again, 64);
  check('seek reproduces the stream', b3pp.HEAPU8.subarray(again, again + 64).join(','),
        b3pp.HEAPU8.subarray(buf, buf + 64).join(','));
  b3pp._free(again);
  r.delete();
  seed.delete();
}
const want = b3pp.hash(buf, len, 'auto');
check('one-shot reproduces itself', b3pp.hash(buf, len, 'auto'), want);

const pool = new b3pp.thread_pool(Math.min(4, navigator.hardwareConcurrency));
const pieces = [1, 1023, 1025, 65536 + 3, 1 << 20, 3 << 20];
const stream = (h) => {
  let off = 0;
  for (const piece of pieces) {
    const n = Math.min(piece, len - off);
    h.update(buf + off, n);
    off += n;
  }
  h.update(buf + off, len - off);
  return h.finalize();
};
for (const variant of b3pp.compiled_arches()) {
  check(`${variant} one-shot`, b3pp.hash(buf, len, variant), want);
  check(`${variant} pooled one-shot`, b3pp.hash(buf, len, pool, variant), want);
  const h = new b3pp.hasher(variant);
  check(`${variant} hasher selects itself`, h.selected_arch(), variant);
  check(`${variant} hasher streamed`, stream(h), want);
  check(`${variant} hasher count`, h.count(), len);
  h.reset();
  check(`${variant} hasher reset`, h.finalize(), EMPTY);
  h.delete();
  const ph = new b3pp.parallel_hasher(pool, variant);
  check(`${variant} parallel_hasher streamed`, stream(ph), want);
  check(`${variant} parallel_hasher finalize is non-destructive`, ph.finalize(), want);
  check(`${variant} parallel_hasher count`, ph.count(), len);
  ph.reset();
  check(`${variant} parallel_hasher reset`, ph.finalize(), EMPTY);
  ph.delete();
}
pool.delete();
b3pp._free(buf);

if (failures) {
  console.error(`${failures} failure(s)`);
  process.exit(1);
}
console.log('ok');
