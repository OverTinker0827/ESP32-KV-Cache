"""Entry 7 tables: repeated runs (mean, std dev, min-max) + correctness, no-network and network."""
import json
import pathlib
import statistics as st
import sys

root = pathlib.Path(__file__).resolve().parents[1] / "results"


def load(name):
    f = root / name
    if not f.exists():
        return []
    return [json.loads(l) for l in f.read_text(encoding="utf-8-sig").splitlines() if l.strip().startswith("{")]


def agg(vals):
    m = st.fmean(vals)
    sd = st.stdev(vals) if len(vals) > 1 else 0.0
    return m, sd, min(vals), max(vals)


which = sys.argv[1] if len(sys.argv) > 1 else "nonet"
if which == "nonet":
    rows = load("entry7_nonet.jsonl") + load("entry7_microredis_engine.jsonl")
    perf = [r for r in rows if r.get("part") == "final"]
    chk = [r for r in rows if r.get("part") == "final_check"]
    designs = list(dict.fromkeys(r["design"] for r in perf))
    for wl in ("g90_uniform", "g50_uniform"):
        base = st.fmean(r["ops_per_s"] for r in perf if r["design"] == "single_core" and r["workload"] == wl)
        n = len([r for r in perf if r["design"] == "single_core" and r["workload"] == wl])
        print(f"\n### No network, {wl} ({n} runs x {perf[0]['run_ms'] / 1000:.0f} s each)\n")
        print("| design | ops/s mean | std dev | min - max | vs single core | p50 / p99 / p99.9 µs | CPU core0 / core1 |")
        print("|---|---:|---:|---:|---:|---:|---:|")
        for d in designs:
            g = [r for r in perf if r["design"] == d and r["workload"] == wl]
            m, sd, lo, hi = agg([r["ops_per_s"] for r in g])
            lat = ("–" if g[0].get("p50_ns", 0) == 0 else
                   f"{st.fmean(r['p50_ns'] for r in g) / 1000:.2f} / {st.fmean(r['p99_ns'] for r in g) / 1000:.2f} / "
                   f"{st.fmean(r['p999_ns'] for r in g) / 1000:.2f}")
            cpu = (f"{st.fmean(r['cpu0_pct'] for r in g):.0f} % / {st.fmean(r['cpu1_pct'] for r in g):.0f} %"
                   if "cpu0_pct" in g[0] else "one core (MicroPython)")
            print(f"| {d} | {m:,.0f} | {sd:,.0f} ({100 * sd / m:.1f} %) | {lo:,} - {hi:,} | {m / base:.2f}× | {lat} | {cpu} |")
    print("\n### No network, correctness (one checked 3 s run per design and workload)\n")
    print("| design | workload | GETs checked | SETs | torn | unpublished version | went backwards | misses | final state wrong | verdict |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|---|")
    for r in chk:
        if r["design"] == "microredis":  # value check only (single-threaded), 512 keys
            print(f"| microredis ({r['keys']} keys) | {r['workload']} | {r['gets_checked']:,} | {r['sets']:,} | – | – | – | "
                  f"{r['wrong_value']} wrong | {r['final_mismatch']} | {'PASS' if r['pass'] else '**FAIL**'} |")
            continue
        print(f"| {r['design']} | {r['workload']} | {r['gets_checked']:,} | {r['sets']:,} | {r['torn']} | {r['future']} | "
              f"{r['backwards']} | {r['misses']} | {r['final_mismatch'] + r['final_unreadable']} | "
              f"{'PASS' if r['pass'] else '**FAIL**'} |")
else:
    names = ["lbf_single", "lbf_shared", "lbf_sharded", "lbf_owner", "lbf_dispatch", "lbf_basic", "lbf_microredis"]
    data = {n: load(f"entry7_{n}.jsonl") for n in names}
    cols = [("GET", 1, 1), ("GET", 4, 1), ("MIX90", 4, 1), ("GET", 4, 16), ("SET", 4, 16), ("MIX90", 4, 16)]
    print("\n### Network (real server over loopback TCP), 5 runs x 2 s each: ops/s mean (std dev %) [min - max]\n")
    print("| design | " + " | ".join(f"{c} {k}×{p}" for c, k, p in cols) + " |")
    print("|---|" + "---:|" * len(cols))
    for n, rows in data.items():
        if not rows:
            continue
        cells = []
        for c, k, p in cols:
            g = [r for r in rows if r.get("part") == "loopback" and r["cmd"] == c and r["conns"] == k and r["P"] == p]
            if not g:
                cells.append("-")
                continue
            m, sd, lo, hi = agg([r["ops_per_s"] for r in g])
            err = " ⚠" if any(r["errors"] for r in g) else ""
            cells.append(f"{m:,.0f} ({100 * sd / m if m else 0:.1f} %) [{lo:,} - {hi:,}]{err}")
        print(f"| {n[4:]} | " + " | ".join(cells) + " |")
    print("\n### Network, latency (batch round trip, mean over runs): p50 / p99 µs\n")
    print("| design | " + " | ".join(f"{c} {k}×{p}" for c, k, p in cols) + " |")
    print("|---|" + "---:|" * len(cols))
    for n, rows in data.items():
        if not rows:
            continue
        cells = []
        for c, k, p in cols:
            g = [r for r in rows if r.get("part") == "loopback" and r["cmd"] == c and r["conns"] == k and r["P"] == p]
            cells.append(f"{st.fmean(r['batch_p50_us'] for r in g):,.0f} / {st.fmean(r['batch_p99_us'] for r in g):,.0f}" if g and "batch_p50_us" in g[0] else "–"
                         )
        print(f"| {n[4:]} | " + " | ".join(cells) + " |")
    print("\n### Network, correctness\n")
    print("| design | functional suite | consistency: GETs checked | SETs | torn | unpublished | backwards | final wrong | io errors | verdict |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|---|")
    for n, rows in data.items():
        c = next((r for r in rows if r.get("part") == "lb_consistency"), None)
        fs = next((r for r in rows if r.get("part") in ("functional", "mr_functional")), None)
        fstr = f"{fs['pass']}/{fs['pass'] + fs['fail']}" if fs else "see log"
        if c:
            print(f"| {n[4:]} | {fstr} | {c['gets_checked']:,} | {c['sets']:,} | {c['torn']} | {c['future']} | "
                  f"{c['backwards']} | {c['final_mismatch'] + c['final_unreadable']} | {c['io_errors']} | "
                  f"{'PASS' if c['pass'] else '**FAIL**'} |")
        elif fs:
            print(f"| {n[4:]} | {fstr} | not run | | | | | | | |")
