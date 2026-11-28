#!/bin/bash
# Entry P9: workloads beyond GET-only / SET-only uniform. 2+2 split (server cpu2-3, kvload 2 threads on
# cpu0,1), N1 OS settings on (restored at the end), each server at its 2-core setting, fresh server per
# workload, keys preloaded, 3 runs x 4 s (median in the summary).
# usage: [WLF=regex] run_workloads.sh [server ...]   servers: kvpi5 kvpi6 redis valkey dragonfly garnet keydb
cd ~/kvpi
ulimit -n 65536 2>/dev/null || ulimit -n $(ulimit -Hn)
echo ondemand | sudo tee /sys/devices/system/cpu/cpufreq/policy0/scaling_governor >/dev/null
old_defer=$(cat /proc/sys/net/core/skb_defer_max)
sudo iptables -t raw -I PREROUTING -i lo -j NOTRACK; sudo iptables -t raw -I OUTPUT -o lo -j NOTRACK
sudo /usr/sbin/sysctl -q -w net.core.skb_defer_max=0
echo "## os: notrack=$(sudo iptables -t raw -S | grep -c NOTRACK) skb_defer_max=$(cat /proc/sys/net/core/skb_defer_max) nofile=$(ulimit -n)"
S=~/kvpi/src
cmd() {
  case $1 in
    kvpi5)     echo "./kvpi/build/v5/kvpi-server-uring -p 7777 -c 2,3" ;;
    kvpi6)     echo "./kvpi/build/v6/kvpi-server-uring -p 7777 -c 2,3" ;;
    redis)     echo "$S/redis-8.12.0/src/redis-server --port 7777 --save '' --appendonly no --protected-mode no --io-threads 2 --maxclients 10000" ;;
    valkey)    echo "$S/valkey-9.2.1/src/valkey-server --port 7777 --save '' --appendonly no --protected-mode no --io-threads 2 --maxclients 10000" ;;
    dragonfly) echo "$S/df/dragonfly-aarch64 --port 7777 --proactor_threads=2 --dbfilename= --logtostderr --maxclients=10000" ;;
    garnet)    echo "env DOTNET_ROOT=$HOME/.dotnet $S/garnet/net10.0/GarnetServer --port 7777 --bind 127.0.0.1" ;;
    keydb)     echo "$S/keydb/src/keydb-server --port 7777 --save '' --appendonly no --protected-mode no --server-threads 2 --maxclients 10000" ;;
  esac
}
# name keyspace load_keys value_bytes get_fraction zipf pipeline conns_per_thread set_ex
WL="mix90_P1 100000 100000 32 0.9 0 1 25 0
mix90_P16 100000 100000 32 0.9 0 16 25 0
mix50_P1 100000 100000 32 0.5 0 1 25 0
mix50_P16 100000 100000 32 0.5 0 16 25 0
zipf90_P1 100000 100000 32 0.9 0.99 1 25 0
zipf90_P16 100000 100000 32 0.9 0.99 16 25 0
zipf50_P1 100000 100000 32 0.5 0.99 1 25 0
zipf50_P16 100000 100000 32 0.5 0.99 16 25 0
val1k_P1 100000 100000 1024 0.9 0 1 25 0
val16k_P1 20000 20000 16384 0.9 0 1 25 0
ttl50_P1 100000 100000 32 0.5 0 1 25 100
conn1000_P1 100000 100000 32 0.9 0 1 500 0
keys10M_P1 10000000 10000000 32 1.0 0 1 25 0
keys10M_P16 10000000 10000000 32 1.0 0 16 25 0"
HZ=$(getconf CLK_TCK)
servers=${*:-"kvpi5 redis valkey dragonfly garnet keydb"}
for s in $servers; do
  echo "$WL" | grep -E "${WLF:-.}" | while read name ks load vs gf zf P cpt ex; do
    OUT=~/kvpi/results/wl_${s}_${name}.csv
    eval "taskset -c 2-3 $(cmd $s)" >/tmp/wl_srv.log 2>&1 &
    SP=$!
    for i in $(seq 100); do redis-cli -p 7777 ping 2>/dev/null | grep -q PONG && break; sleep 0.2; done
    sleep 1
    SP=$(ss -ltnpH "sport = :7777" | grep -o "pid=[0-9]*" | head -1 | cut -d= -f2)   # the process listening on :7777
    t0=$(date +%s)
    ./bench/kvload -p 7777 -t 1 -c 1 -D 1 -W 0 -r $ks -L $load -d $vs >/dev/null
    rss_kb=$(ps -o rss= -p $SP | tr -d ' ')
    echo "## $s $name  $(date +%T) $(vcgencmd measure_temp)  load ${load} keys in $(( $(date +%s) - t0 )) s, rss ${rss_kb} KiB"
    echo "server,workload,rep,ops,p50_us,p99_us,p999_us,client_cpu_pct,errors,server_cpu_pct,server_us_per_req,rss_kb" > $OUT
    for R in 1 2 3; do
      c0=$(awk '{print $14+$15}' /proc/$SP/stat); t1=$(date +%s.%N)
      line=$(./bench/kvload -p 7777 -t 2 -C 0,1 -c $cpt -P $P -g $gf -z $zf -e $ex -r $ks -d $vs -D 4 -W 1)
      t2=$(date +%s.%N); c1=$(awk '{print $14+$15}' /proc/$SP/stat)
      echo "$line" | awk -F, -v s=$s -v w=$name -v r=$R -v c0=$c0 -v c1=$c1 -v t1=$t1 -v t2=$t2 -v hz=$HZ -v rss=$rss_kb \
        '{u=(c1-c0)/hz/(t2-t1)*100; printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%.0f,%.2f,%s\n",s,w,r,$1,$2,$3,$4,$5,$6,u,(u/100)/($1>0?$1:1)*1e6,rss}' >> $OUT
    done
    python3 - "$OUT" <<'PY'
import csv,sys,statistics as st
v=list(csv.DictReader(open(sys.argv[1])))
m=lambda f: st.median(float(x[f]) for x in v)
print(f"   {v[0]['server']:10s} {v[0]['workload']:12s} {m('ops'):>10.0f} ops/s  p50 {m('p50_us'):7.1f} p99 {m('p99_us'):8.1f} us  srv {m('server_cpu_pct'):3.0f}% {m('server_us_per_req'):5.2f} us/req  cli {m('client_cpu_pct'):3.0f}%  err {int(m('errors'))}")
PY
    kill $SP 2>/dev/null; sleep 0.5; kill -9 $SP 2>/dev/null; wait 2>/dev/null
    while redis-cli -p 7777 ping >/dev/null 2>&1; do sleep 0.2; done
  done
done
sudo iptables -t raw -D PREROUTING -i lo -j NOTRACK; sudo iptables -t raw -D OUTPUT -o lo -j NOTRACK
sudo /usr/sbin/sysctl -q -w net.core.skb_defer_max=$old_defer
echo "## restored: notrack=$(sudo iptables -t raw -S | grep -c NOTRACK) skb_defer_max=$(cat /proc/sys/net/core/skb_defer_max)"
echo "## done $(date +%T) $(vcgencmd measure_temp) $(vcgencmd get_throttled)"
