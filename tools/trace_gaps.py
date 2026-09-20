#!/usr/bin/env python3
"""Where the pool's idle time goes, from one pipeline trace.

The bench reports how busy the pool was; this says what the rest of the
time was. Idle pool time has several causes that want different fixes,
and they are indistinguishable in a single percentage:

  * ramp      -- the pool has not all started yet
  * tail      -- the last window's compute is finished and the file is not
  * gaps      -- a cpu sat between two of its own pieces of work
  * residual  -- capacity no agent record accounts for, which is either a
                 thread that never ran or work the records cannot see

Only the gaps are what a persistent agent runtime would attack, so
separating them is the difference between a decision and a guess.

    tools/trace_gaps.py trace.json
    tools/trace_gaps.py trace.json --json out.json
    tools/trace_gaps.py before.json after.json   # one table each

Input is the JSON that `blake3pp_bench_file --trace --trace-agents`
writes for a pipeline row. Agent records are required: without them the
pool is invisible and only the wall split is printed.
"""
import argparse
import json
import statistics
import sys

# A gap shorter than this is what a scheduler does between two runnable
# pieces of work; a longer one means the thread went to sleep and had to
# be woken. The split is what says whether the idleness is reachable.
NOISE_GAP_US = 20.0


def percentile(values, q):
    """The q-th percentile (0..1) of values, nearest-rank, or 0 for none."""
    if not values:
        return 0.0
    ordered = sorted(values)
    i = min(len(ordered) - 1, max(0, int(round(q * (len(ordered) - 1)))))
    return ordered[i]


class Trace:
    """One run's events, sorted into what the pipeline calls them."""

    def __init__(self, path):
        with open(path) as fh:
            doc = json.load(fh)
        self.path = path
        self.threads = doc.get("pool_threads") or 0
        events = doc["traceEvents"]
        self.names = {
            e["tid"]: e["args"]["name"]
            for e in events
            if e.get("name") == "thread_name"
        }
        self.agents = [e for e in events if e.get("name") == "compress"]
        self.driver = next(
            (e for e in events if e.get("name") == "driver" and e["ph"] == "X"),
            None,
        )
        # A window is four phase events on one slot track; collect them by
        # index rather than by track, since a slot carries many windows.
        phases = {}
        for e in events:
            if e.get("name") in ("wait", "hash", "absorb", "release"):
                phases.setdefault(e["args"]["window"], {})[e["name"]] = e
        self.windows = []
        for index in sorted(phases):
            p = phases[index]
            if len(p) != 4:
                continue
            args = p["wait"]["args"]
            self.windows.append(
                {
                    "index": index,
                    "slot": args.get("slot", p["wait"]["tid"] - 10),
                    "bytes": args.get("bytes", 0),
                    "last": bool(args.get("last", 0)),
                    "parallel": bool(args.get("parallel", 0)),
                    "t_wait_begin": p["wait"]["ts"],
                    "t_ready": p["hash"]["ts"],
                    "t_joined": p["absorb"]["ts"],
                    "t_absorbed": p["release"]["ts"],
                    "t_released": p["release"]["ts"] + p["release"]["dur"],
                }
            )
        if not self.windows:
            raise SystemExit(f"{path}: no window records in this trace")
        # Older traces carry no `last` flag; the highest index is it.
        if not any(w["last"] for w in self.windows):
            self.windows[-1]["last"] = True
        self.begin = min(w["t_wait_begin"] for w in self.windows)
        self.end = max(
            [w["t_released"] for w in self.windows]
            + [a["ts"] + a["dur"] for a in self.agents]
        )
        if self.threads == 0:
            # Fall back to what the trace shows rather than refusing.
            self.threads = len({a["tid"] for a in self.agents}) or 1

    # -- the three spans -------------------------------------------------

    def ramp_end(self):
        """When every cpu that ever ran has run: the pool is fully up."""
        first = {}
        for a in self.agents:
            first[a["tid"]] = min(first.get(a["tid"], a["ts"]), a["ts"])
        return max(first.values()) if first else self.begin

    def tail_begin(self):
        """The last full window's CV: after it, no pool work is left."""
        full = [w for w in self.windows if not w["last"]]
        return max(w["t_joined"] for w in full) if full else self.end

    # -- the pool's time -------------------------------------------------

    def pool_split(self):
        wall = self.end - self.begin
        capacity = wall * self.threads
        ramp_end = self.ramp_end()
        tail_begin = self.tail_begin()

        def overlap(a0, a1, b0, b1):
            return max(0.0, min(a1, b1) - max(a0, b0))

        busy = sum(a["dur"] for a in self.agents)
        busy_ramp = sum(
            overlap(a["ts"], a["ts"] + a["dur"], self.begin, ramp_end)
            for a in self.agents
        )
        busy_tail = sum(
            overlap(a["ts"], a["ts"] + a["dur"], tail_begin, self.end)
            for a in self.agents
        )
        idle_ramp = max(0.0, (ramp_end - self.begin) * self.threads - busy_ramp)
        idle_tail = max(0.0, (self.end - tail_begin) * self.threads - busy_tail)
        gaps = self.steady_gaps(ramp_end, tail_begin)
        idle_between = sum(sum(g) for g in gaps.values())
        residual = max(0.0, capacity - busy - idle_ramp - idle_tail - idle_between)
        return {
            "wall_us": wall,
            "threads": self.threads,
            "capacity_us": capacity,
            "ramp_us": ramp_end - self.begin,
            "steady_us": max(0.0, tail_begin - ramp_end),
            "tail_us": self.end - tail_begin,
            "compressing_us": busy,
            "idle_ramp_us": idle_ramp,
            "idle_tail_us": idle_tail,
            "idle_between_us": idle_between,
            "residual_us": residual,
        }

    def steady_gaps(self, ramp_end, tail_begin):
        """Per cpu, the holes between its own agent records in steady state."""
        by_cpu = {}
        for a in self.agents:
            by_cpu.setdefault(a["tid"], []).append(a)
        out = {}
        for tid, records in by_cpu.items():
            records.sort(key=lambda e: e["ts"])
            holes = []
            for prev, cur in zip(records, records[1:]):
                g0 = max(prev["ts"] + prev["dur"], ramp_end)
                g1 = min(cur["ts"], tail_begin)
                if g1 > g0:
                    holes.append(g1 - g0)
            out[tid] = holes
        return out

    # -- per window ------------------------------------------------------

    def window_latencies(self):
        """The four hand-offs a window goes through, in microseconds."""
        agents_by_window = {}
        for a in self.agents:
            agents_by_window.setdefault(a["args"]["window"], []).append(a)
        read_to_agent, agent_to_cv, cv_to_reduced, release_to_submit = [], [], [], []
        for w in self.windows:
            found = agents_by_window.get(w["index"])
            if found and not w["last"]:
                read_to_agent.append(min(a["ts"] for a in found) - w["t_ready"])
                agent_to_cv.append(
                    w["t_joined"] - max(a["ts"] + a["dur"] for a in found)
                )
            cv_to_reduced.append(w["t_absorbed"] - w["t_joined"])
        # A slot's next window cannot start until this one is released.
        by_slot = {}
        for w in self.windows:
            by_slot.setdefault(w["slot"], []).append(w)
        for windows in by_slot.values():
            windows.sort(key=lambda w: w["index"])
            for prev, cur in zip(windows, windows[1:]):
                release_to_submit.append(cur["t_wait_begin"] - prev["t_released"])
        return {
            "read_to_agent": read_to_agent,
            "agent_to_cv": agent_to_cv,
            "cv_to_reduced": cv_to_reduced,
            "release_to_submit": release_to_submit,
        }


def report(trace, out_json=None):
    split = trace.pool_split()
    wall = split["wall_us"]
    cap = split["capacity_us"]
    full = sum(1 for w in trace.windows if not w["last"])
    print(f"\ntrace: {trace.path}")
    print(
        f"  wall {wall / 1000:.1f} ms, {trace.threads} threads, "
        f"{len({a['tid'] for a in trace.agents})} cpus seen, "
        f"{len(trace.windows)} windows ({full} full), "
        f"{len(trace.agents)} agent records"
    )

    print("\n  wall split")
    for name, key in (("ramp", "ramp_us"), ("steady", "steady_us"), ("tail", "tail_us")):
        print(
            f"    {name:<8} {split[key] / 1000:9.2f} ms  {100.0 * split[key] / wall:5.1f}%"
        )

    print(f"\n  pool time (share of {trace.threads} x {wall / 1000:.1f} ms)")
    rows = (
        ("compressing", "compressing_us"),
        ("idle in ramp", "idle_ramp_us"),
        ("idle in tail", "idle_tail_us"),
        ("idle between", "idle_between_us"),
        ("residual", "residual_us"),
    )
    for name, key in rows:
        print(
            f"    {name:<13} {split[key] / 1000:9.2f} ms  {100.0 * split[key] / cap:5.1f}%"
        )

    gaps = trace.steady_gaps(trace.ramp_end(), trace.tail_begin())
    flat = [g for holes in gaps.values() for g in holes]
    print("\n  steady-state idle gaps per cpu")
    print(f"    {'cpu':>5} {'count':>7} {'p50 us':>9} {'p90 us':>9} {'max us':>9}")
    for tid in sorted(gaps):
        holes = gaps[tid]
        if not holes:
            continue
        print(
            f"    {trace.names.get(tid, str(tid)).replace('cpu ', ''):>5} "
            f"{len(holes):>7} {percentile(holes, 0.5):>9.1f} "
            f"{percentile(holes, 0.9):>9.1f} {max(holes):>9.1f}"
        )
    if flat:
        short = [g for g in flat if g < NOISE_GAP_US]
        short_time = sum(short)
        print(
            f"    all: {len(flat)} gaps, {len(short)} under {NOISE_GAP_US:.0f} us "
            f"({100.0 * len(short) / len(flat):.1f}% of gaps, "
            f"{100.0 * short_time / sum(flat):.1f}% of the idle time)"
        )

    lat = trace.window_latencies()
    print("\n  per-window latencies (us)")
    print(f"    {'':<18} {'p50':>10} {'p90':>10}")
    for name, key in (
        ("read -> agent", "read_to_agent"),
        ("agent -> cv", "agent_to_cv"),
        ("cv -> reduced", "cv_to_reduced"),
        ("release -> submit", "release_to_submit"),
    ):
        values = lat[key]
        print(
            f"    {name:<18} {percentile(values, 0.5):>10.1f} "
            f"{percentile(values, 0.9):>10.1f}"
        )

    idle = {
        "idle in ramp": split["idle_ramp_us"],
        "idle in tail": split["idle_tail_us"],
        "idle between agent records": split["idle_between_us"],
        "residual": split["residual_us"],
    }
    worst = max(idle, key=idle.get)
    print(
        f"\n  verdict: {worst} is the largest idle contributor, "
        f"{100.0 * idle[worst] / cap:.1f}% of pool time "
        f"({100.0 * split['compressing_us'] / cap:.1f}% compressing)"
    )

    if out_json:
        payload = dict(split)
        payload["path"] = trace.path
        payload["gaps"] = {
            trace.names.get(tid, str(tid)): holes for tid, holes in gaps.items()
        }
        payload["latencies"] = lat
        payload["verdict"] = {"largest_idle": worst, "share": idle[worst] / cap}
        with open(out_json, "w") as fh:
            json.dump(payload, fh, indent=2)
        print(f"  json written to {out_json}")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("traces", nargs="+", help="trace JSON from --trace --trace-agents")
    ap.add_argument("--json", dest="out_json", help="also write the numbers here")
    args = ap.parse_args()
    if args.out_json and len(args.traces) > 1:
        print("--json takes one trace at a time", file=sys.stderr)
        return 2
    for path in args.traces:
        report(Trace(path), args.out_json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
