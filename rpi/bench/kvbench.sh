#!/bin/bash
# kvbench.sh <tag> <server cmd...>   (Entry P4 harness: kvload instead of redis-benchmark)
# env: SCPU (server cpus for taskset), CCPU (client cpu list "0,1"), CT (client threads), REPS, DUR
# 50 connections total, 100k keys preloaded, 16 B keys, 32 B values; GET-only and SET-only; P=1 and P=16.
set -u
TAG=$1; shift
OUT=~/kvpi/results/kv_${TAG}.csv
SCPU=${SCPU:-3}; CCPU=${CCPU:-0,1}; CT=${CT:-2}; REPS=${REPS:-3}; DUR=${DUR:-4}
HZ=$(getconf CLK_TCK); CPT=$((50 / CT))
taskset -c $SCPU "$@" >/tmp/srv_$TAG.log 2>&1 & SP=$!
for i in $(seq 50); do redis-cli -p 7777 ping >/dev/null 2>&1 && break; sleep 0.2; done
sleep 1
./bench/kvload -p 7777 -t 1 -c 1 -D 1 -W 0 -L 100000 >/dev/null   # fill
echo "tag,test,pipeline,rep,ops,p50_us,p99_us,p999_us,client_cpu_pct,errors,server_cpu_pct,server_us_per_req" > $OUT
for P in 1 16; do for R in $(seq $REPS); do for T in get set; do
  g=$([ $T = get ] && echo 1 || echo 0)
  c0=$(awk '{print $14+$15}' /proc/$SP/stat); t0=$(date +%s.%N)
  line=$(./bench/kvload -p 7777 -t $CT -C $CCPU -c $CPT -P $P -g $g -D $DUR -W 1)
  t1=$(date +%s.%N); c1=$(awk '{print $14+$15}' /proc/$SP/stat)
  # server CPU over the whole kvload run (warm-up included) -> utilisation; per-request uses measured ops
  echo "$line" | awk -F, -v tg=$TAG -v t=$T -v p=$P -v r=$R -v c0=$c0 -v c1=$c1 -v t0=$t0 -v t1=$t1 -v hz=$HZ \
    '{u=(c1-c0)/hz/(t1-t0)*100; printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%.0f,%.2f\n",tg,t,p,r,$1,$2,$3,$4,$5,$6,u,(u/100)/$1*1e6}' >> $OUT
done; done; done
kill $SP; wait $SP 2>/dev/null
python3 - "$OUT" <<'PY'
import csv,sys,statistics as st,collections
g=collections.defaultdict(list)
for r in csv.DictReader(open(sys.argv[1])): g[(r['test'],r['pipeline'])].append(r)
for (t,p),v in g.items():
    m=lambda f: st.median(float(x[f]) for x in v)
    sp=max(float(x['ops']) for x in v)/max(min(float(x['ops']) for x in v),1)-1
    print(f"{v[0]['tag']:18s} {t.upper()} P={p:>2s} {m('ops'):>10.0f} ops/s (spread {sp*100:4.1f}%)  p50 {m('p50_us'):7.1f} p99 {m('p99_us'):7.1f} us  srv {m('server_cpu_pct'):3.0f}% {m('server_us_per_req'):5.2f} us/req  cli {m('client_cpu_pct'):3.0f}%  err {int(m('errors'))}")
PY
