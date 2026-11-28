#!/bin/bash
# profile2.sh <tag> <server cpus> <server cmd...>: perf profile of the server under kvload GET P=1 (2 client threads)
TAG=$1; SC=$2; shift 2
cd ~/kvpi
taskset -c $SC "$@" >/tmp/p2_srv.log 2>&1 & SP=$!; sleep 1
./bench/kvload -p 7777 -t 1 -c 1 -D 1 -W 0 -L 100000 >/dev/null
./bench/kvload -p 7777 -t 2 -C 0,1 -c 25 -P 1 -g 1 -D 7 -W 1 > /tmp/p2_load.txt &
LP=$!; sleep 2
sudo perf record -F 4999 -g -p $SP -o /tmp/perf_$TAG.data -- sleep 4 >/dev/null 2>&1
wait $LP; echo "load: $(cat /tmp/p2_load.txt)"
echo "== user vs kernel"; sudo perf report -i /tmp/perf_$TAG.data --no-children --sort dso -q -g none 2>/dev/null | head -5
echo "== top symbols"; sudo perf report -i /tmp/perf_$TAG.data --no-children --sort sym -q -g none 2>/dev/null | grep -v "^$" | head -40
kill $SP
