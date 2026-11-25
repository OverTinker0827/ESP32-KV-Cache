"""Network benchmark client for microredis and the C caches (identical for all).

Workloads (16 B keys "key:%012d", 32 B values, keyspace 1000 by default):
  PING, SET, GET  x  {1, 4, 8} connections  x  pipeline depth P
Each connection runs in its own process so the Python client is never the bottleneck
for a P=1 run; replies are fixed-size so the client just counts bytes.

Per run: requests/s, avg/p50/p95/p99 latency (per request for P=1, per batch for P>1),
and for the C cache, CPU utilisation per core from INFO idle counters.

Usage: python tools/bench.py --host 192.168.1.50 --label microredis --pipeline 1
"""
import argparse
import json
import multiprocessing as mp
import os
import random
import socket
import statistics
import sys
import time
import pathlib

KEY_FMT = b"key:%012d"
VAL = b"v" * 32


def resp(*args):
    out = [b"*%d\r\n" % len(args)]
    for a in args:
        out.append(b"$%d\r\n%s\r\n" % (len(a), a))
    return b"".join(out)


def expected_reply(cmd):
    if cmd == "PING":
        return b"+PONG\r\n"
    if cmd == "SET":
        return b"+OK\r\n"
    if cmd == "GET":
        return b"$32\r\n" + VAL + b"\r\n"
    raise ValueError(cmd)


def make_req(cmd, k):
    if cmd == "PING":
        return resp(b"PING")
    if cmd == "SET":
        return resp(b"SET", k, VAL)
    return resp(b"GET", k)


def connect(host, port):
    s = socket.create_connection((host, port), timeout=10)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    return s


def recv_exact(s, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = s.recv(max(65536, n - len(buf)))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return buf


def cmd_once(host, port, *args, timeout=10):
    """send one command, return raw reply (enough for simple/integer/bulk replies)"""
    s = connect(host, port)
    s.settimeout(timeout)
    s.sendall(resp(*[a if isinstance(a, bytes) else a.encode() for a in args]))
    data = b""
    t0 = time.time()
    while True:
        data += s.recv(65536)
        if data[:1] in (b"+", b"-", b":") and data.endswith(b"\r\n"):
            break
        if data[:1] == b"$":
            hdr, _, rest = data.partition(b"\r\n")
            n = int(hdr[1:])
            if n < 0 or len(rest) >= n + 2:
                break
        if time.time() - t0 > timeout:
            break
    s.close()
    return data


def info(host, port):
    raw = cmd_once(host, port, "INFO")
    d = {}
    for line in raw.decode(errors="replace").split("\r\n"):
        if ":" in line and not line.startswith("$"):
            k, v = line.split(":", 1)
            d[k] = v
    return d


def worker(args):
    host, port, cmd, P, duration, keyspace, seed, start_at, q = args
    rnd = random.Random(seed)
    s = connect(host, port)
    rep = expected_reply(cmd)
    # pre-build 64 batches of P requests (random keys) to keep the client cheap
    batches = []
    for _ in range(64):
        batches.append(b"".join(make_req(cmd, KEY_FMT % rnd.randrange(keyspace)) for _ in range(P)))
    rlen = len(rep) * P
    while time.time() < start_at:
        time.sleep(0.001)
    lat = []
    n = 0
    i = 0
    t_end = time.perf_counter() + duration
    pc = time.perf_counter_ns
    while True:
        t0 = pc()
        s.sendall(batches[i & 63])
        r = recv_exact(s, rlen)
        t1 = pc()
        if len(r) != rlen or r[: len(rep)] != rep:
            q.put({"error": "bad reply", "got": bytes(r[:60]).decode(errors="replace")})
            return
        lat.append(t1 - t0)
        n += P
        i += 1
        if t1 * 1e-9 >= t_end:
            break
    s.close()
    q.put({"n": n, "lat": lat, "elapsed": duration})


def run(host, port, cmd, conns, P, duration, keyspace, is_kvcache):
    q = mp.Manager().Queue()
    start_at = time.time() + 1.0
    if is_kvcache:
        i0 = info(host, port)
    t0 = time.time()
    with mp.Pool(conns) as pool:
        pool.map(worker, [(host, port, cmd, P, duration, keyspace, 1000 + c, start_at, q) for c in range(conns)])
    wall = time.time() - t0 - 1.0
    res = [q.get() for _ in range(conns)]
    errs = [r for r in res if "error" in r]
    if errs:
        return {"cmd": cmd, "conns": conns, "P": P, "error": errs[0]}
    total = sum(r["n"] for r in res)
    lat = sorted(x for r in res for x in r["lat"])
    out = {
        "cmd": cmd, "conns": conns, "P": P,
        "rps": round(total / duration),
        "lat_unit": "request" if P == 1 else "batch",
        "lat_avg_us": round(statistics.fmean(lat) / 1000, 1),
        "lat_p50_us": round(lat[len(lat) // 2] / 1000, 1),
        "lat_p95_us": round(lat[int(len(lat) * 0.95)] / 1000, 1),
        "lat_p99_us": round(lat[int(len(lat) * 0.99)] / 1000, 1),
        "samples": len(lat),
    }
    if is_kvcache:
        i1 = info(host, port)
        dt = int(i1["uptime_us"]) - int(i0["uptime_us"])
        out["cpu0_util"] = round(1 - (int(i1["idle0_us"]) - int(i0["idle0_us"])) / dt, 3)
        out["cpu1_util"] = round(1 - (int(i1["idle1_us"]) - int(i0["idle1_us"])) / dt, 3)
        dc = int(i1["cmds"]) - int(i0["cmds"])
        if dc > 0:
            out["server_exec_us_per_cmd"] = round((int(i1["exec_cycles"]) - int(i0["exec_cycles"])) / dc / 240, 2)
    return out


def preload(host, port, keyspace):
    s = connect(host, port)
    for i in range(keyspace):
        s.sendall(resp(b"SET", KEY_FMT % i, VAL))
        recv_exact(s, 5)  # P=1: microredis does not process pipelined input reliably
    s.close()


def max_keys(host, port, start=0, limit=200000):
    """Insert distinct keys (P=1) until the server returns an error / stops answering."""
    s = connect(host, port)
    s.settimeout(15)
    i = start
    err = None
    try:
        while i < limit:
            s.sendall(resp(b"SET", b"fill:%011d" % i, VAL))
            r = s.recv(256)
            if not r.startswith(b"+OK"):
                err = r[:80].decode(errors="replace")
                break
            i += 1
    except (socket.timeout, ConnectionError, OSError) as e:
        err = f"{type(e).__name__}: {e}"
    s.close()
    return {"max_keys_over_network": i, "stop_reason": err}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=6379)
    ap.add_argument("--label", required=True)
    ap.add_argument("--duration", type=float, default=10)
    ap.add_argument("--keyspace", type=int, default=1000)
    ap.add_argument("--conns", default="1,4,8")
    ap.add_argument("--pipeline", default="1")
    ap.add_argument("--cmds", default="PING,SET,GET")
    ap.add_argument("--fill", action="store_true", help="also measure max keys over the network (destructive)")
    a = ap.parse_args()

    is_cc = False
    try:
        is_cc = info(a.host, a.port).get("impl") == "kvcache"
    except Exception:
        pass
    print(f"[{a.label}] kvcache={is_cc}", flush=True)
    preload(a.host, a.port, a.keyspace)
    results = {"label": a.label, "host": a.host, "time": time.strftime("%Y-%m-%d %H:%M:%S"), "runs": []}
    if is_cc:
        results["info"] = info(a.host, a.port)
    for P in [int(x) for x in a.pipeline.split(",")]:
        for cmd in a.cmds.split(","):
            for c in [int(x) for x in a.conns.split(",")]:
                r = run(a.host, a.port, cmd, c, P, a.duration, a.keyspace, is_cc)
                print(json.dumps(r), flush=True)
                results["runs"].append(r)
    if a.fill:
        results["fill"] = max_keys(a.host, a.port)
        print(json.dumps(results["fill"]), flush=True)
    outdir = pathlib.Path(__file__).resolve().parents[1] / "results"
    outdir.mkdir(exist_ok=True)
    fn = outdir / f"{a.label}_{time.strftime('%Y%m%d_%H%M%S')}.json"
    fn.write_text(json.dumps(results, indent=1))
    print("saved", fn)


if __name__ == "__main__":
    main()
