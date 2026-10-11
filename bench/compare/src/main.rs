// The reference implementation as the external baseline: the same
// algorithm on the same machine, with its own execution model. The blake3
// crate picks its SIMD backend at runtime and drives the tree with rayon,
// which is what blake3pp's own rows are compared against.
//
// Two measurements, because they answer different questions.
//
//   --size <MiB>   an in-memory buffer, hashed single-threaded and then
//                  through rayon. Isolates the execution model: no
//                  storage, no page cache, nothing but scheduling. The
//                  same bytes also go through as 8 KiB messages, which
//                  is the short-message path on its own: no tree to
//                  spread over cores, no long run to amortise over.
//   --file <path>  a file, hashed the way the crate's own documentation
//                  recommends: update_mmap_rayon. That path goes through
//                  the page cache by design, and it is measured as it
//                  ships rather than bent to match a direct-I/O
//                  pipeline, since that difference is part of what the
//                  comparison shows. The page
//                  cache is dropped before every timed rep (root only),
//                  so each rep reads the device; --warm leaves it, as
//                  the control that shows the drop is what makes a
//                  difference.
//
// Reported as best-of-N GiB/s, the same convention blake3pp_bench uses.
use std::time::Instant;

// Best-effort page-cache drop before a timed rep. Needs root, or an
// elevated process on Windows; false when the platform refuses.
//
// Linux: vm.drop_caches=3, after flushing the file's own dirty pages (a
// freshly written file would otherwise survive the drop).
#[cfg(target_os = "linux")]
const DROP_CACHES: &str = "vm.drop_caches=3";
#[cfg(target_os = "linux")]
fn drop_caches(path: &str) -> bool {
    if let Ok(f) = std::fs::File::open(path) {
        let _ = f.sync_all();
    }
    std::fs::write("/proc/sys/vm/drop_caches", "3").is_ok()
}

// macOS: purge(8).
#[cfg(target_os = "macos")]
const DROP_CACHES: &str = "purge";
#[cfg(target_os = "macos")]
fn drop_caches(_path: &str) -> bool {
    std::process::Command::new("/usr/sbin/purge")
        .status()
        .is_ok_and(|s| s.success())
}

// Windows: what RAMMap's "empty" menu does, in its order. Emptying the
// working sets moves cached file pages to the standby and modified lists,
// flushing writes the modified list out, and purging the standby list
// discards what is left. The memory-list calls need
// SeProfileSingleProcessPrivilege, which only an elevated token holds.
#[cfg(windows)]
const DROP_CACHES: &str = "working sets emptied and standby list purged";
#[cfg(windows)]
fn drop_caches(_path: &str) -> bool {
    #[repr(C)]
    struct TokenPrivileges {
        count: u32,
        luid_low: u32,
        luid_high: i32,
        attributes: u32,
    }
    #[link(name = "advapi32")]
    extern "system" {
        fn OpenProcessToken(process: isize, access: u32, token: *mut isize) -> i32;
        fn LookupPrivilegeValueW(system: *const u16, name: *const u16, luid: *mut u32) -> i32;
        fn AdjustTokenPrivileges(token: isize, disable_all: i32, new: *const TokenPrivileges,
                                 len: u32, prev: *mut TokenPrivileges, ret: *mut u32) -> i32;
    }
    #[link(name = "kernel32")]
    extern "system" {
        fn GetCurrentProcess() -> isize;
        fn GetLastError() -> u32;
        fn CloseHandle(handle: isize) -> i32;
    }
    #[link(name = "ntdll")]
    extern "system" {
        fn NtSetSystemInformation(class: u32, info: *const u32, len: u32) -> i32;
    }
    const TOKEN_ADJUST_PRIVILEGES: u32 = 0x20;
    const TOKEN_QUERY: u32 = 0x08;
    const SE_PRIVILEGE_ENABLED: u32 = 0x02;
    const SYSTEM_MEMORY_LIST_INFORMATION: u32 = 80;
    const MEMORY_EMPTY_WORKING_SETS: u32 = 2;
    const MEMORY_FLUSH_MODIFIED_LIST: u32 = 3;
    const MEMORY_PURGE_STANDBY_LIST: u32 = 4;

    let name: Vec<u16> = "SeProfileSingleProcessPrivilege\0".encode_utf16().collect();
    let mut tp = TokenPrivileges { count: 1, luid_low: 0, luid_high: 0,
                                   attributes: SE_PRIVILEGE_ENABLED };
    unsafe {
        let mut token = 0isize;
        if OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                            &mut token) == 0 {
            return false;
        }
        // AdjustTokenPrivileges succeeds even when the token lacks the
        // privilege, and reports that through ERROR_NOT_ALL_ASSIGNED.
        let enabled = LookupPrivilegeValueW(std::ptr::null(), name.as_ptr(),
                                            &mut tp.luid_low) != 0
            && AdjustTokenPrivileges(token, 0, &tp, 0, std::ptr::null_mut(),
                                     std::ptr::null_mut()) != 0
            && GetLastError() == 0;
        CloseHandle(token);
        enabled
            && [MEMORY_EMPTY_WORKING_SETS, MEMORY_FLUSH_MODIFIED_LIST, MEMORY_PURGE_STANDBY_LIST]
                .iter()
                .all(|cmd| NtSetSystemInformation(SYSTEM_MEMORY_LIST_INFORMATION, cmd, 4) >= 0)
    }
}

#[cfg(not(any(target_os = "linux", target_os = "macos", windows)))]
const DROP_CACHES: &str = "";
#[cfg(not(any(target_os = "linux", target_os = "macos", windows)))]
fn drop_caches(_path: &str) -> bool {
    false
}

fn gib_per_s(bytes: u64, secs: f64) -> f64 {
    (bytes as f64) / secs / (1024.0 * 1024.0 * 1024.0)
}

fn best_of<F: FnMut() -> f64>(reps: u32, mut run: F) -> f64 {
    let mut best = 0.0f64;
    for _ in 0..reps {
        let r = run();
        if r > best {
            best = r;
        }
    }
    best
}

fn main() {
    let mut size_mib: u64 = 512;
    let mut reps: u32 = 5;
    let mut file: Option<String> = None;
    let mut threads: usize = 0;
    let mut warm = false;
    let mut pin: Option<usize> = None;

    let args: Vec<String> = std::env::args().collect();
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--size" => { size_mib = args[i + 1].parse().unwrap(); i += 2; }
            "--reps" => { reps = args[i + 1].parse().unwrap(); i += 2; }
            "--file" => { file = Some(args[i + 1].clone()); i += 2; }
            "--threads" => { threads = args[i + 1].parse().unwrap(); i += 2; }
            "--warm" => { warm = true; i += 1; }
            "--pin" => { pin = Some(args[i + 1].parse().unwrap()); i += 2; }
            other => { eprintln!("unknown argument: {other}"); std::process::exit(2); }
        }
    }

    if threads > 0 {
        rayon::ThreadPoolBuilder::new().num_threads(threads).build_global().unwrap();
    }
    // Which backend this binary can reach. The crate caps itself at
    // compile time, so the label comes from the features it was built
    // with; what it actually selects is that cap or the best the CPU
    // offers, whichever is lower.
    // `pure` drops only the crate's C and assembly: on x86 its Rust
    // SSE2/SSE4.1/AVX2 intrinsics remain (AVX-512 exists only as C and
    // assembly), while on aarch64 it drops NEON too.
    let cap = if cfg!(feature = "portable") && cfg!(target_arch = "x86_64") {
        "pure Rust (the crate's Rust sse2/sse41/avx2, no avx512)"
    } else if cfg!(feature = "portable") && cfg!(target_arch = "aarch64") {
        "portable (pure Rust, neon off)"
    } else if cfg!(feature = "portable") {
        "portable (pure Rust)"
    } else if cfg!(feature = "cap_sse2") {
        "sse2"
    } else if cfg!(feature = "cap_sse41") {
        "sse41"
    } else if cfg!(feature = "cap_avx2") {
        "avx2"
    } else if cfg!(feature = "cap_no_neon") {
        "scalar (neon off)"
    } else if cfg!(target_arch = "x86_64") {
        "unrestricted (avx512 where the CPU has it)"
    } else if cfg!(target_arch = "aarch64") {
        "unrestricted (neon)"
    } else {
        "unrestricted (no SIMD backend on this architecture: portable)"
    };
    println!("blake3 crate, backend cap: {cap}, rayon threads: {}",
             rayon::current_num_threads());

    // Pinned only now that the pool exists (current_num_threads() above
    // created it), so rayon's workers keep the full affinity mask and
    // only this thread, which runs the single-thread rows, is confined.
    // On a machine with mixed core classes an unpinned single-thread row
    // lands on whichever class the scheduler picks.
    let tag = match pin {
        None => String::new(),
        Some(cpu) if cfg!(target_os = "macos") => {
            let _ = cpu;
            "  [--pin ignored: macOS has no thread affinity]".to_string()
        }
        Some(cpu) => {
            if core_affinity::set_for_current(core_affinity::CoreId { id: cpu }) {
                format!("  [cpu {cpu}]")
            } else {
                format!("  [pin to cpu {cpu} FAILED, unpinned]")
            }
        }
    };

    if let Some(path) = file {
        let bytes = std::fs::metadata(&path).unwrap().len();
        let cold = !warm && drop_caches(&path);
        if cold {
            println!("  [cold: {DROP_CACHES} before every rep]");
        } else if warm {
            println!("  [warm by request (--warm): the page cache is left as it is]");
        } else {
            println!("  [WARM: cannot drop caches (needs root or elevation), so these \
                      rows time the page cache, not the device]");
        }
        let mut digest = String::new();
        let seq = best_of(reps, || {
            if cold {
                drop_caches(&path);
            }
            let t = Instant::now();
            let mut h = blake3::Hasher::new();
            h.update_mmap(&path).unwrap();
            let d = h.finalize();
            let r = gib_per_s(bytes, t.elapsed().as_secs_f64());
            digest = d.to_hex()[..16].to_string();
            r
        });
        let par = best_of(reps, || {
            if cold {
                drop_caches(&path);
            }
            let t = Instant::now();
            let mut h = blake3::Hasher::new();
            h.update_mmap_rayon(&path).unwrap();
            let d = h.finalize();
            std::hint::black_box(d);
            gib_per_s(bytes, t.elapsed().as_secs_f64())
        });
        println!("  digest              {digest}");
        println!("  mmap                {seq:7.2} GiB/s{tag}");
        println!("  mmap+rayon          {par:7.2} GiB/s");
        return;
    }

    let bytes = size_mib * 1024 * 1024;
    let input = vec![0xa5u8; bytes as usize];
    let seq = best_of(reps, || {
        let t = Instant::now();
        let d = blake3::hash(&input);
        std::hint::black_box(d);
        gib_per_s(bytes, t.elapsed().as_secs_f64())
    });
    let par = best_of(reps, || {
        let t = Instant::now();
        let mut h = blake3::Hasher::new();
        h.update_rayon(&input);
        let d = h.finalize();
        std::hint::black_box(d);
        gib_per_s(bytes, t.elapsed().as_secs_f64())
    });
    // The same bytes as 8 KiB messages, one hasher each, their digests
    // folded. A message of eight chunks has no tree worth spreading over
    // cores and no long run to amortise a batch over, so this row is each
    // implementation's short-message path and nothing else. The fold is
    // 1/256 of the bytes and is in the C++ harness's row too.
    const MSG: usize = 8 * 1024;
    let mut small_fold = String::new();
    let small = best_of(reps, || {
        let t = Instant::now();
        let mut fold = blake3::Hasher::new();
        let mut at = 0;
        while at + MSG <= input.len() {
            fold.update(blake3::hash(&input[at..at + MSG]).as_bytes());
            at += MSG;
        }
        let d = fold.finalize();
        let r = gib_per_s((input.len() / MSG * MSG) as u64,
                         t.elapsed().as_secs_f64());
        small_fold = d.to_hex()[..16].to_string();
        r
    });
    // Extended output: the crate's OutputReader, filled in the same
    // reader-sized pieces blake3pp_bench uses, reported as bytes of output.
    // The crate reaches a wide xof kernel only where its AVX-512 assembly
    // is available; elsewhere it compresses one output block at a time, and
    // this row is where that shows.
    let mut xof_fold = String::new();
    let xof = best_of(reps, || {
        let piece = (8 << 20).min(input.len());
        let mut out = vec![0u8; piece];
        let t = Instant::now();
        let mut r = blake3::Hasher::new().update(&input[..1]).finalize_xof();
        let mut acc = [0u8; 32];
        let mut at = 0;
        while at < input.len() {
            let n = piece.min(input.len() - at);
            r.fill(&mut out[..n]);
            let mut b = 0;
            while b * 64 < n {
                for k in 0..32 {
                    acc[k] ^= out[b * 64 + k];
                }
                b += 64;
            }
            at += n;
        }
        let rate = gib_per_s(input.len() as u64, t.elapsed().as_secs_f64());
        xof_fold = acc.iter().take(8).map(|b| format!("{b:02x}")).collect();
        rate
    });
    // A benchmark that does not check its answer measures the wrong
    // thing eventually. The buffer is deterministic, so every cap must
    // produce the same digest; sweep.sh compares them across binaries.
    // The fold below is reported as `fold` rather than `digest` because
    // the sweep scripts read the one digest line per binary.
    println!("in-memory, {size_mib} MiB, best of {reps}, digest {}",
             &blake3::hash(&input).to_hex()[..16]);
    println!("  single thread       {seq:7.2} GiB/s{tag}");
    println!("  rayon               {par:7.2} GiB/s  ({:.2}x)", par / seq);
    println!("  8 KiB messages      {small:7.2} GiB/s  (fold {small_fold})");
    println!("  extended output     {xof:7.2} GiB/s  (fold {xof_fold})");
}
