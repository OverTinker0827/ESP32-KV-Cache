"""Markdown tables for the Phase 2 harness (results/entry4_p2.jsonl).

Balancing runs with random hot-key placement are repeated (field "rep"); every design of a
repetition ran on the same placement and stream. Tables show the mean over repetitions,
with min-max in brackets when there is more than one.
"""
import json
import pathlib
import statistics
import sys

f = pathlib.Path(__file__).resolve().parents[1] / "results" / (sys.argv[1] if len(sys.argv) > 1 else "entry4_p2.jsonl")
rows = [json.loads(l) for l in f.read_text(encoding="utf-8-sig").splitlines() if l.strip().startswith("{")]
bal = [r for r in rows if r.get("part") == "balance"]
wls = list(dict.fromkeys(r["workload"] for r in bal))
designs = list(dict.fromkeys(r["design"] for r in bal))


def group(d, w):
    return [r for r in bal if r["design"] == d and r["workload"] == w]


def ratio_vs_static(d, w):
    """per-repetition ratio to static_hash on the same placement, then averaged"""
    out = []
    for r in group(d, w):
        s = next(x for x in bal if x["design"] == "static_hash" and x["workload"] == w and x.get("rep") == r.get("rep"))
        out.append(r["ops_per_s"] / s["ops_per_s"])
    return out


print("### Throughput (mean ops/s) and speed-up vs static_hash on the same placement\n")
print("| design | " + " | ".join(f"{w} (n={len(group('static_hash', w))})" for w in wls) + " |")
print("|---|" + "---:|" * len(wls))
for d in designs:
    cells = []
    for w in wls:
        g = group(d, w)
        m = statistics.fmean(r["ops_per_s"] for r in g)
        rt = ratio_vs_static(d, w)
        rng = f" [{min(rt):.2f}–{max(rt):.2f}]" if len(rt) > 1 else ""
        cells.append(f"{m:,.0f} ({statistics.fmean(rt):.2f}×{rng})")
    print(f"| {d} | " + " | ".join(cells) + " |")

print("\n### Per-worker load and balancing activity (mean over repetitions)\n")
print("| design | workload | offered to w0 (static map) | w0 / w1 ops/s | spills | migrations | replica hits / fallbacks | lock contended |")
print("|---|---|---:|---:|---:|---:|---:|---:|")
for w in wls:
    for d in designs:
        g = group(d, w)
        m = lambda k: statistics.fmean(r[k] for r in g)
        print(f"| {d} | {w} | {m('offered_w0_pct'):.0f} % | {m('w0_ops_per_s'):,.0f} / {m('w1_ops_per_s'):,.0f} | "
              f"{m('spills'):,.0f} | {m('migrations'):.0f} | {m('rep_hits'):,.0f} / {m('rep_fallbacks'):,.0f} | "
              f"{m('lk_contended_pct'):.1f} % |")

print("\n### Phase 1 designs: quiet vs WiFi-like load on core 0 (40 % duty)\n")
print("| design | workload | quiet ops/s | wifisim ops/s | change | p99.9 core0 µs quiet → sim | p99.9 core1 µs quiet → sim | sim lock contended |")
print("|---|---|---:|---:|---:|---:|---:|---:|")
other = [r for r in rows if r.get("part") in ("direct", "queue")]
for r in other:
    if r["scenario"] != "quiet":
        continue
    s = next((x for x in other if x["scenario"] == "wifisim_core0" and x["design"] == r["design"]
              and x["workload"] == r["workload"]), None)
    if not s:
        continue
    if "p999_core1_ns" in r:
        c0 = "–" if r["ntasks"] == 1 else f"{r['p999_core0_ns'] / 1000:.1f} → {s['p999_core0_ns'] / 1000:.1f}"
        c1 = f"{r['p999_core1_ns'] / 1000:.1f} → {s['p999_core1_ns'] / 1000:.1f}"
    else:
        c0 = c1 = f"(queue) {r.get('lat_p999_us')} → {s.get('lat_p999_us')}"
    print(f"| {r['design']} | {r['workload']} | {r['ops_per_s']:,} | {s['ops_per_s']:,} | "
          f"{100 * (s['ops_per_s'] / r['ops_per_s'] - 1):+.0f} % | {c0} | {c1} | {s.get('lk_contended_pct', 0):.1f} % |")
