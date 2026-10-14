#!/usr/bin/env python3
"""Render the documentation's charts from benchmark reports.

The numbers live in a data file (docs/data/charts.json) and the charts are
SVGs committed beside the documents, so a reader of the repository sees the
same figures the site shows and a reviewer sees what changed. Refreshing a
series means re-running the bench on that machine and feeding its report
back through --extract.

    tools/bench-charts.py                      # render every chart
    tools/bench-charts.py --extract REPORT     # the rows a report holds
    tools/bench-charts.py --check              # fail if a chart is stale

A report is `blake3pp_bench`'s own output, or a thread sweep's: --extract
reads the single-thread end-to-end rows, the 8 KiB rows and the sweep's
summary table, and prints them as the JSON shape the data file expects.
"""
import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DATA = ROOT / "docs" / "data" / "charts.json"
OUT = ROOT / "docs" / "img"

# ---------------------------------------------------------------- extract

# "  blake3pp       9.01 GiB/s   (4db7...)  [cpu 11]"
ROW = re.compile(r"^\s{2,}(\S+)\s+([0-9.]+)\s+(GiB|MiB|KiB)/s(.*)$")
# The sweep's summary: "      1     1.89   1.0x      1.61   1.0x   ..."
SWEEP = re.compile(r"^\s*(\d+)\s+([0-9.]+)\s+[0-9.]+x\s+([0-9.]+)\s+[0-9.]+x"
                   r"\s+([0-9.]+)\s+[0-9.]+x")


def gibs(value: str, unit: str) -> float:
    scale = {"GiB": 1.0, "MiB": 1 / 1024, "KiB": 1 / 1048576}[unit]
    return round(float(value) * scale, 3)


def extract(text: str) -> dict:
    """The rows a chart can be built from, by name."""
    rows, sweep = {}, []
    for line in text.split("\n"):
        m = SWEEP.match(line)
        if m:
            sweep.append({"threads": int(m.group(1)),
                          "pool": float(m.group(2)),
                          "reference_pool": float(m.group(3)),
                          "rayon": float(m.group(4))})
            continue
        m = ROW.match(line)
        if m:
            name, tail = m.group(1), m.group(4)
            # The same row name appears for our kernels and for upstream's
            # assembly; the tail is what tells them apart.
            if "hand-written asm" in tail or "hand-tuned" in tail:
                name += " [asm]"
            rows[name] = gibs(m.group(2), m.group(3))
    out = {"rows": rows}
    if sweep:
        out["sweep"] = sweep
    return out


# ----------------------------------------------------------------- render

# One palette for both themes, chosen so the bars stay distinguishable in
# either: the text and axes follow the viewer's colour scheme, the bars do
# not. SVGs are embedded as images, so the page's CSS cannot reach them and
# the media query inside each file is what makes them theme-aware.
STYLE = """
  text { font-family: system-ui, -apple-system, "Segoe UI", sans-serif;
         fill: #24292f; }
  .axis { stroke: #8b949e; stroke-width: 1; }
  .grid { stroke: #d0d7de; stroke-width: 1; stroke-dasharray: 2 3; }
  .muted { fill: #57606a; }
  @media (prefers-color-scheme: dark) {
    text { fill: #e6edf3; }
    .axis { stroke: #6e7681; }
    .grid { stroke: #30363d; }
    .muted { fill: #9198a1; }
  }
"""
SERIES_COLOURS = ["#e8590c", "#1971c2", "#2f9e44", "#9c36b5"]


def esc(text: str) -> str:
    return (str(text).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;"))


def svg_open(width: int, height: int, title: str, subtitle: str) -> list:
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}"'
        f' width="{width}" height="{height}" role="img"'
        f' aria-label="{esc(title)}">',
        f"<style>{STYLE}</style>",
        f'<text x="0" y="18" font-size="15" font-weight="600">{esc(title)}</text>',
        f'<text x="0" y="36" font-size="12" class="muted">{esc(subtitle)}</text>',
    ]


def legend(entries, x: int, y: int) -> list:
    out, at = [], x
    for i, name in enumerate(entries):
        colour = SERIES_COLOURS[i % len(SERIES_COLOURS)]
        out.append(f'<rect x="{at}" y="{y - 9}" width="10" height="10"'
                   f' fill="{colour}" rx="2"/>')
        out.append(f'<text x="{at + 15}" y="{y}" font-size="12">{esc(name)}</text>')
        at += 20 + int(7.2 * len(name)) + 14
    return out


def ticks(top: float) -> list:
    """Round grid values up to a readable ceiling."""
    for step in (0.5, 1, 2, 2.5, 5, 10, 20, 25, 50):
        if top / step <= 6:
            end = step * (int(top / step) + 1)
            return [i * step for i in range(int(end / step) + 1)]
    return [0, top]


def grouped_bars(chart: dict) -> str:
    """One group of bars per machine, one bar per implementation."""
    groups = chart["groups"]
    series = chart["series"]
    bar, gap, pad = 18, 14, 210
    width, top_pad = 860, 70
    rows = len(groups) * (len(series) * bar + gap)
    notes = [g["source"] for g in groups if g.get("source")]
    height = top_pad + rows + 46 + 14 * len(notes)
    scale_top = max(v for g in groups for v in g["values"])
    grid = ticks(scale_top)
    plot = width - pad - 60

    out = svg_open(width, height, chart["title"], chart["subtitle"])
    out += legend([s for s in series], pad, 56)
    for value in grid:
        x = pad + plot * value / grid[-1]
        out.append(f'<line class="grid" x1="{x:.1f}" y1="{top_pad}"'
                   f' x2="{x:.1f}" y2="{top_pad + rows}"/>')
        out.append(f'<text x="{x:.1f}" y="{top_pad + rows + 16}" font-size="11"'
                   f' class="muted" text-anchor="middle">{value:g}</text>')
    out.append(f'<line class="axis" x1="{pad}" y1="{top_pad}" x2="{pad}"'
               f' y2="{top_pad + rows}"/>')

    y = top_pad + gap / 2
    for group in groups:
        label_y = y + len(series) * bar / 2 + 4
        out.append(f'<text x="{pad - 8}" y="{label_y:.1f}" font-size="12"'
                   f' text-anchor="end">{esc(group["name"])}</text>')
        for i, value in enumerate(group["values"]):
            colour = SERIES_COLOURS[i % len(SERIES_COLOURS)]
            w = plot * value / grid[-1]
            out.append(f'<rect x="{pad}" y="{y:.1f}" width="{w:.1f}"'
                       f' height="{bar - 3}" fill="{colour}" rx="2"/>')
            out.append(f'<text x="{pad + w + 6:.1f}" y="{y + bar - 7:.1f}"'
                       f' font-size="11" class="muted">{value:g}</text>')
            y += bar
        y += gap
    out.append(f'<text x="0" y="{top_pad + rows + 40}" font-size="11"'
               f' class="muted">{esc(chart["unit"])}</text>')
    at = top_pad + rows + 58
    for group, note in zip(groups, notes):
        out.append(f'<text x="0" y="{at}" font-size="10" class="muted">'
                   f'{esc(group["name"])}: {esc(note)}</text>')
        at += 14
    out.append("</svg>")
    return "\n".join(out)


def lines(chart: dict) -> str:
    """Throughput against thread count, one line per implementation."""
    series = chart["series"]
    points = chart["points"]
    width, height, pad_l, pad_b, top_pad = 860, 420, 60, 46, 70
    xs = [p["x"] for p in points]
    grid = ticks(max(max(p["values"]) for p in points))
    plot_w = width - pad_l - 30
    plot_h = height - top_pad - pad_b

    def px(x):  # the thread axis is logarithmic: counts double
        import math
        lo, hi = math.log2(min(xs)), math.log2(max(xs))
        return pad_l + plot_w * (math.log2(x) - lo) / (hi - lo)

    def py(v):
        return top_pad + plot_h - plot_h * v / grid[-1]

    out = svg_open(width, height, chart["title"], chart["subtitle"])
    out += legend(series, pad_l, 56)
    for value in grid:
        y = py(value)
        out.append(f'<line class="grid" x1="{pad_l}" y1="{y:.1f}"'
                   f' x2="{pad_l + plot_w}" y2="{y:.1f}"/>')
        out.append(f'<text x="{pad_l - 8}" y="{y + 4:.1f}" font-size="11"'
                   f' class="muted" text-anchor="end">{value:g}</text>')
    for x in xs:
        out.append(f'<text x="{px(x):.1f}" y="{height - pad_b + 20}"'
                   f' font-size="11" class="muted" text-anchor="middle">{x}</text>')
    out.append(f'<line class="axis" x1="{pad_l}" y1="{top_pad}" x2="{pad_l}"'
               f' y2="{top_pad + plot_h}"/>')
    out.append(f'<line class="axis" x1="{pad_l}" y1="{top_pad + plot_h}"'
               f' x2="{pad_l + plot_w}" y2="{top_pad + plot_h}"/>')
    for i, name in enumerate(series):
        colour = SERIES_COLOURS[i % len(SERIES_COLOURS)]
        path = " ".join(
            f"{'M' if n == 0 else 'L'}{px(p['x']):.1f},{py(p['values'][i]):.1f}"
            for n, p in enumerate(points))
        out.append(f'<path d="{path}" fill="none" stroke="{colour}"'
                   f' stroke-width="2.5"/>')
        for p in points:
            out.append(f'<circle cx="{px(p["x"]):.1f}" cy="{py(p["values"][i]):.1f}"'
                       f' r="3" fill="{colour}"/>')
        last = points[-1]
        out.append(f'<text x="{px(last["x"]) - 4:.1f}"'
                   f' y="{py(last["values"][i]) - 8:.1f}" font-size="11"'
                   f' fill="{colour}" text-anchor="end">{last["values"][i]:g}</text>')
    out.append(f'<text x="{pad_l}" y="{height - 6}" font-size="11" class="muted">'
               f'{esc(chart["unit"])}</text>')
    out.append("</svg>")
    return "\n".join(out)


KINDS = {"grouped-bars": grouped_bars, "lines": lines}


def render(data: dict) -> dict:
    return {name: KINDS[chart["kind"]](chart)
            for name, chart in data["charts"].items()}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", metavar="REPORT",
                    help="print the rows a bench report holds, as JSON")
    ap.add_argument("--data", type=pathlib.Path, default=DATA)
    ap.add_argument("--out", type=pathlib.Path, default=OUT)
    ap.add_argument("--check", action="store_true",
                    help="report stale charts instead of writing them")
    args = ap.parse_args()

    if args.extract:
        text = pathlib.Path(args.extract).read_text(errors="replace")
        json.dump(extract(text), sys.stdout, indent=2)
        print()
        return 0

    data = json.loads(args.data.read_text())
    charts = render(data)
    stale = []
    for name, svg in charts.items():
        path = args.out / f"{name}.svg"
        if args.check:
            if not path.exists() or path.read_text() != svg:
                stale.append(path.relative_to(ROOT))
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(svg)
        print(f"wrote {path.relative_to(ROOT)}")
    if stale:
        print("stale charts (run tools/bench-charts.py): "
              + ", ".join(str(p) for p in stale), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
