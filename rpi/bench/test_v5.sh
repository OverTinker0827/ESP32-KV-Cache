#!/bin/bash
# v5 correctness: differential test vs Redis 8.12 (several seeds), concurrency stress, mixed pipeline,
# eviction through the server, and expiry through the server.
cd ~/kvpi/kvpi
R=~/kvpi/src/redis-8.12.0/src/redis-server
$R --port 7790 --save '' --appendonly no --protected-mode no >/tmp/r790.log 2>&1 &
RP=$!
taskset -c 2,3 ./build/v5/kvpi-server-uring -p 7791 -c 2,3 -s 8 >/tmp/k791.log 2>&1 &
KP=$!
sleep 1
echo "== $(head -1 /tmp/k791.log)"
for seed in 41; do
  echo "-- difftest seed $seed: $(python3 difftest.py 7790 7791 20000 $seed 2>&1 | tail -3 | tr '\n' ' ')"
done
echo "-- stress: $(python3 stress_mt.py 7791 8 10 2>&1 | tail -1)"
redis-cli -p 7791 flushall >/dev/null
printf '*3\r\n$3\r\nSET\r\n$1\r\nx\r\n$2\r\n42\r\n*2\r\n$3\r\nGET\r\n$1\r\nx\r\n*3\r\n$5\r\nLPUSH\r\n$1\r\nl\r\n$1\r\na\r\n*2\r\n$3\r\nGET\r\n$1\r\nl\r\n*2\r\n$4\r\nINCR\r\n$1\r\nx\r\n*2\r\n$3\r\nGET\r\n$1\r\nx\r\n' > /tmp/pipe5.bin
echo "-- pipeline: $(timeout 1 nc -q1 127.0.0.1 7791 < /tmp/pipe5.bin | tr '\r\n' '||')"
kill $KP; wait $KP 2>/dev/null
# expiry through the server: 2000 keys with a 1 s TTL vanish (sweep + lazy), persistent ones stay
taskset -c 3 ./build/v5/kvpi-server-uring -p 7792 -c 3 >/tmp/k792.log 2>&1 &
KP=$!
sleep 0.5
for i in $(seq 1 2000); do printf 'SET t%d v EX 1\r\nSET p%d v\r\n' $i $i; done | redis-cli -p 7792 --pipe >/dev/null 2>&1
echo "-- expiry: dbsize before $(redis-cli -p 7792 dbsize)"
sleep 2.5
echo "-- expiry: dbsize after 2.5 s $(redis-cli -p 7792 dbsize), get t5 '$(redis-cli -p 7792 get t5)', get p5 '$(redis-cli -p 7792 get p5)', $(redis-cli -p 7792 info | grep expired_keys | tr -d '\r')"
kill $KP; wait $KP 2>/dev/null
# eviction through the server: 32 MiB limit, write 200 MiB of 1 KiB values
taskset -c 3 ./build/v5/kvpi-server-uring -p 7793 -c 3 -x 32 -e lru >/tmp/k793.log 2>&1 &
KP=$!
sleep 0.5
redis-benchmark -p 7793 -q -n 200000 -r 10000000 -d 1024 -t set -P 16 2>&1 | tr '\r' '\n' | grep "per second"
echo "-- eviction: $(redis-cli -p 7793 info | grep -E 'keys:|slab_bytes|limit_bytes|evicted' | tr -d '\r' | tr '\n' ' ')"
echo "-- rss $(ps -o rss= -p $KP) KiB"
kill $KP $RP; wait 2>/dev/null
echo "== done"
