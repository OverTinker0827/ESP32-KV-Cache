#!/bin/bash
# memtier_run.sh <tag> <server cpus> <server cmd...>: GET-only and SET-only, 2 client threads x 25 conns on cpu0-1,
# 10 s each after a fill; reports ops/s, p50/p99 and server CPU%
TAG=$1; SC=$2; shift 2
OUT=~/kvpi/results/memtier_${TAG}.csv
HZ=$(getconf CLK_TCK)
taskset -c $SC "$@" >/tmp/mt_srv.log 2>&1 & SP=$!; sleep 1.5
memtier_benchmark -s 127.0.0.1 -p 7777 -t 2 -c 4 --ratio 1:0 --key-pattern P:P --key-maximum 100000 -d 32 -n allkeys --hide-histogram >/dev/null 2>&1
echo "tag,test,pipeline,ops,p50_ms,p99_ms,server_cpu_pct" > $OUT
for P in 1 16; do for R in 0:1 1:0; do
  c0=$(awk '{print $14+$15}' /proc/$SP/stat); t0=$(date +%s.%N)
  taskset -c 0-1 memtier_benchmark -s 127.0.0.1 -p 7777 -t 2 -c 25 --pipeline $P --ratio $R --key-pattern R:R --key-maximum 100000 -d 32 \
     --test-time 10 --hide-histogram --print-percentiles 50,99 2>/dev/null > /tmp/mt_out.txt
  t1=$(date +%s.%N); c1=$(awk '{print $14+$15}' /proc/$SP/stat)
  u=$(echo "($c1-$c0)/$HZ/($t1-$t0)*100" | bc -l)
  name=$([ $R = 0:1 ] && echo GET || echo SET)
  awk -v t=$TAG -v n=$name -v p=$P -v u=$u '/^Totals/{printf "%s,%s,%s,%.0f,%s,%s,%.0f\n",t,n,p,$2,$6,$7,u}' /tmp/mt_out.txt >> $OUT
done; done
kill $SP; wait $SP 2>/dev/null
column -s, -t $OUT
