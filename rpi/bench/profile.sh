#!/bin/bash
# profile.sh <tag> <cpus> <server cmd...>: perf profile of the server during GET c=50 P=1, plus client CPU use
TAG=$1; SC=$2; shift 2
taskset -c $SC "$@" >/tmp/prof_srv.log 2>&1 & SP=$!; sleep 1
taskset -c 0-1 redis-benchmark -p 7777 -q -n 300000 -r 100000 -d 32 -t set >/dev/null 2>&1
sudo perf record -F 4999 -g -p $SP -o /tmp/perf_$TAG.data -- sleep 4 >/dev/null 2>&1 &
PP=$!; sleep 0.3
read cu0 cs0 < <(awk '{print $14,$15}' /proc/self/stat)
s0=$(date +%s.%N)
taskset -c 0-1 redis-benchmark -p 7777 -q -n 1500000 -r 100000 -d 32 -c 50 --threads 2 -t get 2>&1 | grep -v WARNING
s1=$(date +%s.%N)
wait $PP
echo "client wall $(echo "$s1-$s0" | bc) s"
echo "== by DSO"; sudo perf report -i /tmp/perf_$TAG.data --no-children --sort dso -q 2>/dev/null | head -6
echo "== top symbols"; sudo perf report -i /tmp/perf_$TAG.data --no-children --sort sym -q -g none 2>/dev/null | grep -v "^$" | head -30
kill $SP
