"""Turn results/<prefix>_*.jsonl (on-device engine RESULT lines) into markdown tables.

Usage: python tools/summarize.py entry2 [microredis_jsonl]
"""
import json
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[1] / "results"
prefix = sys.argv[1]
rows = {}
for f in sorted(root.glob(f"{prefix}_*.jsonl")):
    name = f.stem[len(prefix) + 1:]
    for line in f.read_text(encoding="utf-8-sig").splitlines():
        if line.strip():
            d = json.loads(line)
            rows.setdefault(name, {})[d.get("scenario", "?") + ("_" + d["keys"] if "keys" in d else "")] = d


def us(ns):
    return f"{ns / 1000:.2f}"


order = [n for n in ["microredis", "basic", "basic_fastitoa", "opt_noiram", "opt_forcelock", "opt"] if n in rows]
print("| variant | scenario | SET/s | GET/s | DEL/s | SET p50/p99/p99.9/max µs | GET p50/p99/p99.9/max µs | rand GET/s |")
print("|---|---|---:|---:|---:|---:|---:|---:|")
for n in order:
    for sc, d in rows[n].items():
        if sc.startswith("fill"):
            continue
        def q(op):
            ks = [f"{op}_p50_ns", f"{op}_p99_ns", f"{op}_p999_ns", f"{op}_max_ns"]
            return " / ".join(us(d[k]) if k in d else "-" for k in ks)
        print(f"| {n} | {sc} | {d['set_ops']:,} | {d['get_ops']:,} | {d['del_ops']:,} | "
              f"{q('set')} | {q('get')} | {d.get('rand_get_ops', 0):,} |")
print()
print("| variant | max keys | bytes/key |")
print("|---|---:|---:|")
for n in order:
    f = next((d for sc, d in rows[n].items() if sc.startswith("fill")), None)
    if f:
        print(f"| {n} | {f['max_keys']:,} | {f.get('bytes_per_key', f.get('bytes_per_key_alloc', '-'))} |")
