#!/bin/bash
# perf profile of a server (1 worker, cpu3) under the 16 KiB-value 90/10 workload (kvload 2 threads cpu0,1)
# usage: profile_big.sh <tag> <server cmd...>
TAG=$1; shift
cd ~/kvpi
old_defer=$(cat /proc/sys/net/core/skb_defer_max)
sudo iptables -t raw -I PREROUTING -i lo -j NOTRACK; sudo iptables -t raw -I OUTPUT -o lo -j NOTRACK
sudo /usr/sbin/sysctl -q -w net.core.skb_defer_max=0
taskset -c 3 "$@" >/tmp/pb_srv.log 2>&1 &
sleep 1
SP=$(ss -ltnpH "sport = :7777" | grep -o "pid=[0-9]*" | head -1 | cut -d= -f2)
./bench/kvload -p 7777 -t 1 -c 1 -D 1 -W 0 -r 20000 -L 20000 -d 16384 >/dev/null
./bench/kvload -p 7777 -t 2 -C 0,1 -c 25 -P 1 -g 0.9 -r 20000 -d 16384 -D 7 -W 1 > /tmp/pb_load.txt &
LP=$!
sleep 2
sudo perf record -F 4999 -g -p $SP -o /tmp/perf_$TAG.data -- sleep 4 >/dev/null 2>&1
wait $LP
echo "load: $(cat /tmp/pb_load.txt)"
echo "== by DSO"; sudo perf report -i /tmp/perf_$TAG.data --no-children --sort dso -q -g none 2>/dev/null | head -4
echo "== top symbols"; sudo perf report -i /tmp/perf_$TAG.data --no-children --sort sym -q -g none 2>/dev/null | grep -v "^$" | head -25
kill $SP
sudo iptables -t raw -D PREROUTING -i lo -j NOTRACK; sudo iptables -t raw -D OUTPUT -o lo -j NOTRACK
sudo /usr/sbin/sysctl -q -w net.core.skb_defer_max=$old_defer
echo "restored: notrack=$(sudo iptables -t raw -S | grep -c NOTRACK) skb_defer_max=$(cat /proc/sys/net/core/skb_defer_max)"
