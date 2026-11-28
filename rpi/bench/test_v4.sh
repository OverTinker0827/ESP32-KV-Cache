#!/bin/bash
# Correctness for v4 network-path options: stress (self-checking values, pipelines, 0-9 KB values),
# a mixed pipelined batch, and 3 KB values pipelined through redis-benchmark.
cd ~/kvpi/kvpi
printf '*3\r\n$3\r\nSET\r\n$1\r\nx\r\n$2\r\n42\r\n*2\r\n$3\r\nGET\r\n$1\r\nx\r\n*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nDEL\r\n$1\r\nx\r\n*2\r\n$3\r\nget\r\n$1\r\nx\r\n*2\r\n$6\r\nEXISTS\r\n$1\r\nx\r\nPING\r\n' > /tmp/pipe.bin
for opt in "-M 1" "-b 50" "-M 1 -b 50"; do
  taskset -c 2,3 ./build/v4/kvpi-server-uring -p 7785 -c 2,3 -s 4 $opt >/tmp/u4.log 2>&1 &
  SP=$!
  sleep 0.5
  echo "== $opt: $(head -1 /tmp/u4.log)"
  python3 stress_mt.py 7785 8 10 2>&1 | tail -1
  echo "pipe: $(timeout 1 nc -q1 127.0.0.1 7785 < /tmp/pipe.bin | tr '\r\n' '||')"
  redis-benchmark -p 7785 -q -n 100000 -c 50 -P 16 -t set,get -d 3000 2>&1 | tr '\r' '\n' | grep "per second"
  kill $SP; wait $SP 2>/dev/null
  echo "server alive until kill: $(grep -ci error /tmp/u4.log) errors in log"
done
