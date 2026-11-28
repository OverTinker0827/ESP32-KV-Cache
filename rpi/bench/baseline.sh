#!/bin/bash
# Loopback benchmark (Entry P1 harness, v2: wall-clock rps).
# redis-benchmark's own rps is quantised to 0.25 s of elapsed time (500k requests -> 166,667 / 153,846 / ...,
# up to +-8 %), so rps_wall = n / (wall time of the redis-benchmark process), timed here with ns resolution.
# Server pinned to SCPU, redis-benchmark (2 threads) pinned to CCPU. Server CPU% from /proc/<pid>/stat.
# usage: baseline.sh <tag> <server-cmd...>   env: N, REPS, SCPU, CCPU
set -u
TAG=$1; shift
OUT=~/kvpi/results/baseline_${TAG}.csv
PORT=7777; SCPU=${SCPU:-3}; CCPU=${CCPU:-0-1}; REPS=${REPS:-3}  # CTHREADS: client threads (default 2)
HZ=$(getconf CLK_TCK)
taskset -c $SCPU "$@" >/tmp/srv_$TAG.log 2>&1 &
SP=$!; sleep 2
cpu() { awk '{print $14+$15}' /proc/$SP/stat; }
taskset -c $CCPU redis-benchmark -p $PORT -q -n 200000 -r 100000 -d 32 -c 50 -P 16 -t set >/dev/null 2>&1 # fill
echo "tag,test,clients,pipeline,rep,n,rps_tool,rps_wall,p50_ms,p99_ms,server_cpu_pct,cpu_us_per_req" > $OUT
for P in 1 16; do for C in 1 50; do for R in $(seq $REPS); do for T in set get; do
  n=${N:-4000000}; [ $P = 1 ] && n=$((n/4)); [ $C = 1 ] && n=$((n/8))
  c0=$(cpu); t0=$(date +%s.%N)
  line=$(taskset -c $CCPU redis-benchmark -p $PORT -q -n $n -r 100000 -d 32 -c $C -P $P --threads ${CTHREADS:-2} -t $T --csv 2>/dev/null | tail -1 | tr -d '"')
  t1=$(date +%s.%N); c1=$(cpu)
  echo "$line" | awk -F, -v t=$TAG -v c=$C -v p=$P -v r=$R -v n=$n -v c0=$c0 -v c1=$c1 -v t0=$t0 -v t1=$t1 -v hz=$HZ '
    {w=t1-t0; rps=n/w; u=(c1-c0)/hz/w*100; printf "%s,%s,%s,%s,%s,%s,%s,%.0f,%s,%s,%.0f,%.2f\n",t,$1,c,p,r,n,$2,rps,$5,$7,u,(u/100)/rps*1e6}' >> $OUT
done; done; done; done
kill $SP; wait $SP 2>/dev/null
python3 - "$OUT" <<'PY'
import csv,sys,statistics as st,collections
g=collections.defaultdict(list)
for r in csv.DictReader(open(sys.argv[1])):
    try: float(r["rps_wall"])
    except: continue
    g[(r['test'],r['clients'],r['pipeline'])].append(r)
for k,v in g.items():
    m=lambda f: st.median(float(x[f]) for x in v)
    print(f"{v[0]['tag']:16s} {k[0]:3s} c={k[1]:>2s} P={k[2]:>2s}  {m('rps_wall'):>10.0f} rps  p50 {m('p50_ms'):.3f}  p99 {m('p99_ms'):.3f} ms  srvCPU {m('server_cpu_pct'):3.0f}%  {m('cpu_us_per_req'):.2f} cpu-us/req")
PY
