"""Markdown tables for the Phase 1 multicore harness (results/<file>.jsonl)."""
import json
import pathlib
import sys

f = pathlib.Path(__file__).resolve().parents[1] / "results" / (sys.argv[1] if len(sys.argv) > 1 else "entry3_mc.jsonl")
rows = [json.loads(l) for l in f.read_text(encoding="utf-8-sig").splitlines() if l.strip().startswith("{")]
direct = [r for r in rows if r.get("part") == "direct"]
queue = [r for r in rows if r.get("part") == "queue"]
ctr = [r for r in rows if r.get("part") == "counters"]
wls = ["g90_uniform", "g50_uniform", "g90_zipf", "g50_zipf"]

base = {r["workload"]: r["ops_per_s"] for r in direct if r["design"] == "1c_nolock"}
designs = list(dict.fromkeys(r["design"] for r in direct))
print("### Direct designs: throughput (ops/s) and speed-up vs 1 core without lock\n")
print("| design | " + " | ".join(wls) + " |")
print("|---|" + "---:|" * len(wls))
for d in designs:
    cells = []
    for w in wls:
        r = next((x for x in direct if x["design"] == d and x["workload"] == w), None)
        cells.append(f"{r['ops_per_s']:,} ({r['ops_per_s'] / base[w]:.2f}×)" if r else "-")
    print(f"| {d} | " + " | ".join(cells) + " |")

print("\n### Direct designs: latency and contention (g90_uniform / g50_uniform / g90_zipf)\n")
print("| design | workload | p50 / p99 / p99.9 µs | contended acquisitions | spin cycles per contended | time spinning | seqlock retries |")
print("|---|---|---:|---:|---:|---:|---:|")
for d in designs:
    for w in ["g90_uniform", "g50_uniform", "g90_zipf"]:
        r = next((x for x in direct if x["design"] == d and x["workload"] == w), None)
        if not r:
            continue
        print(f"| {d} | {w} | {r['p50_ns']/1000:.2f} / {r['p99_ns']/1000:.2f} / {r['p999_ns']/1000:.2f} | "
              f"{r['lk_contended_pct']:.1f} % | {r['lk_spin_cyc_per_contended']:,} | {r['spin_pct']:.1f} % | {r['seq_retries']:,} |")

print("\n### Queue designs (receiver task on core 0)\n")
print("| design | workload | handoff | ops/s | vs 1c_nolock | worker0 / worker1 ops/s | latency p50 / p99 / p99.9 µs | CPU core0 / core1 |")
print("|---|---|---|---:|---:|---:|---:|---:|")
for r in queue:
    print(f"| {r['design']} | {r['workload']} | {r['handoff']} x{r['batch']} | {r['ops_per_s']:,} | "
          f"{r['ops_per_s'] / base[r['workload']]:.2f}× | {r['w0_ops_per_s']:,} / {r['w1_ops_per_s']:,} | "
          f"{r['lat_p50_us']} / {r['lat_p99_us']} / {r['lat_p999_us']} | {r['cpu0_pct']:.0f} % / {r['cpu1_pct']:.0f} % |")

print("\n### Statistics counters (2c_owned_nolock, g90_uniform)\n")
print("| counter design | ops/s | p50 / p99 µs | lost updates |")
print("|---|---:|---:|---:|")
for r in ctr:
    lost = r.get("lost_updates", 0)
    print(f"| {r['design']} | {r['ops_per_s']:,} | {r['p50_ns']/1000:.2f} / {r['p99_ns']/1000:.2f} | {lost:,} |")
