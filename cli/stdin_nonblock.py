"""blake3ppsum reading a standard input in O_NONBLOCK mode.

    stdin_nonblock.py <work dir> <tool> [runner...]

A producer writes through a pipe whose read end is non-blocking, pausing
between writes so the tool's reads find the pipe empty and answer EAGAIN.
The digest must equal the one of the same bytes read from a file, on the
sequential path (--threads 1) and the parallel one; a --keyed key read
from such a stdin in two writes must be the whole key. The runner (node for
the wasm build, an emulator for a cross build) goes in front of the tool.
"""
import fcntl
import os
import subprocess
import sys
import time

work, tool, runner = sys.argv[1], sys.argv[2], sys.argv[3:]
cmd = [*runner, tool]
os.makedirs(work, exist_ok=True)
data = bytes((i * 7 + 3) % 251 for i in range(1024 * 1024 + 17))
path = os.path.join(work, "input.bin")
with open(path, "wb") as f:
    f.write(data)
want = subprocess.run([*cmd, path], capture_output=True, check=True,
                      text=True).stdout.split()[0]

failed = False
for threads in ("1", "4"):
    r, w = os.pipe()
    fcntl.fcntl(r, fcntl.F_SETFL, fcntl.fcntl(r, fcntl.F_GETFL) | os.O_NONBLOCK)
    # stderr to a file: node writes a pipe asynchronously and loses what
    # is still queued when the process exits.
    err_path = os.path.join(work, f"stderr-{threads}.txt")
    with open(err_path, "wb") as err:
        child = subprocess.Popen([*cmd, "--threads", threads, "-"], stdin=r,
                                 stdout=subprocess.PIPE, stderr=err)
    os.close(r)
    try:
        step = 64 * 1024
        for off in range(0, len(data), step):
            os.write(w, data[off:off + step])
            time.sleep(0.02)
    except BrokenPipeError:
        pass
    os.close(w)
    try:
        out, _ = child.communicate(timeout=120)
    except subprocess.TimeoutExpired:
        child.kill()
        out, _ = child.communicate()
    got = out.decode().split()[0] if out.strip() else ""
    if child.returncode != 0 or got != want:
        failed = True
        with open(err_path, "rb") as f:
            lines = [l for l in f.read().decode(errors="replace").splitlines()
                     if len(l) < 400]
        print(f"--threads {threads}: exit {child.returncode}, digest "
              f"{got or '(none)'}, want {want}")
        print("\n".join(lines[-20:]))
# A key on such a stdin, written in two parts: the key is read to the end
# of the stream, not to the first read that comes back short.
key = "3031323334353637383961626364656630313233343536373839616263646566"
key_path = os.path.join(work, "hexkey")
with open(key_path, "w") as f:
    f.write(key)
want_keyed = subprocess.run([*cmd, "--keyed", key_path, path],
                            capture_output=True, check=True,
                            text=True).stdout.split()[0]
r, w = os.pipe()
fcntl.fcntl(r, fcntl.F_SETFL, fcntl.fcntl(r, fcntl.F_GETFL) | os.O_NONBLOCK)
child = subprocess.Popen([*cmd, "--keyed", "-", path], stdin=r,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
os.close(r)
os.write(w, key[:32].encode())
time.sleep(0.2)
os.write(w, (key[32:] + "\n").encode())
os.close(w)
out, err = child.communicate(timeout=120)
got = out.decode().split()[0] if out.strip() else ""
if child.returncode != 0 or got != want_keyed:
    failed = True
    print(f"--keyed - in two writes: exit {child.returncode}, digest "
          f"{got or '(none)'}, want {want_keyed}: {err.decode(errors='replace')[-400:]}")
sys.exit(1 if failed else 0)
