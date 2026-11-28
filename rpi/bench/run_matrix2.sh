#!/bin/bash
# Entry P4 matrix with kvload: every server at 1/2/3 server cores.
#   1c: server cpu3,   client cpu0,1 (2 threads)     2c: server cpu2-3, client cpu0,1 (2 threads)
#   3c: server cpu1-3, client cpu0   (1 thread)
cd ~/kvpi
echo ondemand | sudo tee /sys/devices/system/cpu/cpufreq/policy0/scaling_governor >/dev/null
S=~/kvpi/src
declare -A CPUS=([1]=3 [2]=2-3 [3]=1-3) CLI=([1]=0,1 [2]=0,1 [3]=0) CT=([1]=2 [2]=2 [3]=1) LIST=([1]=3 [2]=2,3 [3]=1,2,3)
cmd() {
  case $1 in
    redis)     echo "$S/redis-8.12.0/src/redis-server --port 7777 --save '' --appendonly no --protected-mode no --io-threads $2" ;;
    valkey)    echo "$S/valkey-9.2.1/src/valkey-server --port 7777 --save '' --appendonly no --protected-mode no --io-threads $2" ;;
    dragonfly) echo "$S/df/dragonfly-aarch64 --port 7777 --proactor_threads=$2 --dbfilename= --logtostderr" ;;
    keydb)     echo "$S/keydb/src/keydb-server --port 7777 --save '' --appendonly no --protected-mode no --server-threads $2" ;;
    garnet)    echo "env DOTNET_ROOT=$HOME/.dotnet $S/garnet/net10.0/GarnetServer --port 7777 --bind 127.0.0.1" ;;
    kvpi)      echo "./kvpi/build/v1/kvpi-server-mt -p 7777 -c ${LIST[$2]}" ;;
    kvpiu)     echo "./kvpi/build/v2/kvpi-server-uring -p 7777 -c ${LIST[$2]}" ;;
  esac
}
servers=${*:-"kvpi kvpiu redis valkey dragonfly keydb garnet"}
for s in $servers; do for n in 1 2 3; do
  c=$(cmd $s $n)
  echo "## ${s}_${n}c  $(date +%T)  $(vcgencmd measure_temp)  : $c"
  eval "SCPU=${CPUS[$n]} CCPU=${CLI[$n]} CT=${CT[$n]} ./kvbench.sh p4_${s}_${n}c $c" 2>&1 | tail -4
done; done
echo "## done $(date +%T) $(vcgencmd measure_temp) $(vcgencmd get_throttled)"
