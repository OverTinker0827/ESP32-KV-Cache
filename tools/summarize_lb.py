"""Markdown tables for the end-to-end loopback runs (results/<prefix>_lb_*.jsonl)."""
import json
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[1] / "results"
prefix = sys.argv[1] if len(sys.argv) > 1 else "entry5"
order = ["lb_basic", "lb_single", "lb_single0", "lb_shared_seq", "lb_sharded_seq", "lb_single_cl"]
data = {}
for n in order:
    f = root / f"{prefix}_{n}.jsonl"
    if f.exists():
        data[n] = [json.loads(l) for l in f.read_text(encoding="utf-8-sig").splitlines() if l.strip().startswith("{")]

print("### Raw TCP echo over loopback (no parser, no engine)\n")
print("| build | echo task on | core locking | RTT p50 / p99 / min µs |")
print("|---|---|---|---:|")
for n, rows in data.items():
    for r in rows:
        if r.get("part") == "echo_baseline":
            print(f"| {n} | core {r['echo_core']} | {'on' if r['core_locking'] else 'off'} | "
                  f"{r['rtt_p50_us']} / {r['rtt_p99_us']} / {r['rtt_min_us']} |")

for scen in ("quiet", "wifisim_core0"):
    print(f"\n### Real server over loopback, {scen}: ops/s (batch p50 / p99 µs)\n")
    cols = [("GET", 1, 1), ("GET", 4, 1), ("GET", 1, 16), ("GET", 4, 16), ("SET", 1, 1), ("SET", 4, 16)]
    print("| design | " + " | ".join(f"{c} c={k} P={p}" for c, k, p in cols) + " | server µs/cmd |")
    print("|---|" + "---:|" * (len(cols) + 1))
    for n, rows in data.items():
        cells = []
        srv = []
        for c, k, p in cols:
            r = next((x for x in rows if x.get("part") == "loopback" and x["scenario"] == scen and x["cmd"] == c
                      and x["conns"] == k and x["P"] == p), None)
            if not r:
                cells.append("-")
                continue
            bad = " ⚠" if r["errors"] else ""
            cells.append(f"{r['ops_per_s']:,} ({r['batch_p50_us']} / {r['batch_p99_us']}){bad}")
            srv.append(r["server_exec_us_per_cmd"])
        print(f"| {n} | " + " | ".join(cells) + f" | {sum(srv) / len(srv):.2f} |" if srv else f"| {n} | - |")
print("\n⚠ = at least one client connection failed in that run")
