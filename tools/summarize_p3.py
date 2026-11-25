"""Markdown tables for Phase 3 (results/<prefix>_p3_*.jsonl): capacity sweep, mixed sizes,
speed regression (Entry 2 benchmark in the same firmware), adaptive reserve."""
import json
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[1] / "results"
prefix = sys.argv[1] if len(sys.argv) > 1 else "entry6"
rows = []
for n in ("p3_basic", "p3_opt0", "p3_opt1", "p3_opt2"):
    f = root / f"{prefix}_{n}.jsonl"
    if f.exists():
        rows += [json.loads(l) for l in f.read_text(encoding="utf-8-sig").splitlines() if l.strip().startswith("{")]

sweep = [r for r in rows if r.get("part") == "sweep"]
for keys in ("seq", "iot"):
    kl = 16 if keys == "seq" else 24
    print(f"\n### Capacity: max keys ({kl} B keys `{'key:%012d' if keys == 'seq' else 'sensor:temp:roomNN:NNNNN'}`), "
          "store bytes per key in brackets\n")
    sizes = sorted({r["val_len"] for r in sweep})
    print("| engine | " + " | ".join(f"{v} B" for v in sizes) + " |")
    print("|---|" + "---:|" * len(sizes))
    for eng, pfx in (("basic", 0), ("opt0", 0), ("opt1", 0), ("opt2", 0), ("opt2", 1)):
        cells = []
        for v in sizes:
            r = next((x for x in sweep if x["engine"] == eng and x["keys"] == keys and x["val_len"] == v
                      and x["prefix"] == pfx), None)
            cells.append(f"{r['max_keys']:,} ({r['bytes_per_key']:.0f})" if r else "-")
        label = eng + (" + prefix" if pfx else "")
        print(f"| {label} | " + " | ".join(cells) + " |")

print("\n### Mixed value sizes (16-512 B, IoT keys): keys stored, payload / store bytes\n")
print("| engine | keys stored | rejected SETs | store KB | payload efficiency |")
print("|---|---:|---:|---:|---:|")
for r in rows:
    if r.get("part") == "mixed":
        label = r["engine"] + (" + prefix" if r["prefix"] else "")
        print(f"| {label} | {r['max_keys']:,} | {r.get('rejected', '-')} | {r['store_bytes'] / 1024:.0f} | "
              f"{r['payload_eff']:.2f} |")

print("\n### Speed regression check (Entry 2 benchmark, same firmware): ops/s, GET p50/p99 µs\n")
print("| engine | scenario | SET/s | GET/s | DEL/s | GET p50 / p99 µs |")
print("|---|---|---:|---:|---:|---:|")
for r in rows:
    if "set_ops" in r and r.get("scenario") in ("quiet", "sim_other_core", "sim_same_core"):
        print(f"| {r['engine']} | {r['scenario']} | {r['set_ops']:,} | {r['get_ops']:,} | {r['del_ops']:,} | "
              f"{r['get_p50_ns'] / 1000:.2f} / {r['get_p99_ns'] / 1000:.2f} |")

print("\n### Adaptive reserve under packet-buffer bursts (v2 engine, 6 s each)\n")
print("| mode | buffer allocs failed / attempted | cache hit rate | arena pages avg (min-max) | pages released / regrown | lowest free heap |")
print("|---|---:|---:|---:|---:|---:|")
for r in rows:
    if r.get("part") == "adaptive":
        print(f"| {r['mode']} | {r['buf_fails']:,} / {r['buf_attempts']:,} | {r['hit_rate']:.3f} | "
              f"{r['pages_avg']:.1f} ({r['pages_min']}-{r['pages_max']}) | {r['pages_released']} / {r['pages_grown']} | "
              f"{r['heap_min'] / 1024:.1f} KB |")
