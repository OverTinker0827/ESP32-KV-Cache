#!/bin/bash
# Entry P8 matrix: P4 matrix + the N1 OS settings (loopback conntrack bypass, skb_defer_max=0) for every server;
# kvpi v5 (features, multishot default) and v4 -M 1 (no features) side by side.
#   1c: server cpu3,   client cpu0,1 (2 threads)     2c: server cpu2-3, client cpu0,1 (2 threads)
#   3c: server cpu1-3, client cpu0   (1 thread)
cd ~/kvpi
echo ondemand | sudo tee /sys/devices/system/cpu/cpufreq/policy0/scaling_governor >/dev/null
old_defer=$(cat /proc/sys/net/core/skb_defer_max)
sudo iptables -t raw -I PREROUTING -i lo -j NOTRACK; sudo iptables -t raw -I OUTPUT -o lo -j NOTRACK
sudo /usr/sbin/sysctl -q -w net.core.skb_defer_max=0
echo "## os: notrack=$(sudo iptables -t raw -S | grep -c NOTRACK) skb_defer_max=$(cat /proc/sys/net/core/skb_defer_max)"
S=~/kvpi/src
declare -A CPUS=([1]=3 [2]=2-3 [3]=1-3) CLI=([1]=0,1 [2]=0,1 [3]=0) CT=([1]=2 [2]=2 [3]=1) LIST=([1]=3 [2]=2,3 [3]=1,2,3)
cmd() {
  case $1 in
    redis)     echo "$S/redis-8.12.0/src/redis-server --port 7777 --save '' --appendonly no --protected-mode no --io-threads $2" ;;
    valkey)    echo "$S/valkey-9.2.1/src/valkey-server --port 7777 --save '' --appendonly no --protected-mode no --io-threads $2" ;;
    dragonfly) echo "$S/df/dragonfly-aarch64 --port 7777 --proactor_threads=$2 --dbfilename= --logtostderr" ;;
    keydb)     echo "$S/keydb/src/keydb-server --port 7777 --save '' --appendonly no --protected-mode no --server-threads $2" ;;
    garnet)    echo "env DOTNET_ROOT=$HOME/.dotnet $S/garnet/net10.0/GarnetServer --port 7777 --bind 127.0.0.1" ;;
    kvpi5)     echo "./kvpi/build/v5/kvpi-server-uring -p 7777 -c ${LIST[$2]}" ;;
    kvpi4)     echo "./kvpi/build/v4/kvpi-server-uring -p 7777 -c ${LIST[$2]} -M 1" ;;
  esac
}
servers=${*:-"kvpi5 kvpi4 redis valkey dragonfly keydb garnet"}
for s in $servers; do for n in 1 2 3; do
  c=$(cmd $s $n)
  echo "## ${s}_${n}c  $(date +%T)  $(vcgencmd measure_temp)  : $c"
  eval "SCPU=${CPUS[$n]} CCPU=${CLI[$n]} CT=${CT[$n]} ./kvbench.sh p8_${s}_${n}c $c" 2>&1 | tail -4
done; done
sudo iptables -t raw -D PREROUTING -i lo -j NOTRACK; sudo iptables -t raw -D OUTPUT -o lo -j NOTRACK
sudo /usr/sbin/sysctl -q -w net.core.skb_defer_max=$old_defer
echo "## restored: notrack=$(sudo iptables -t raw -S | grep -c NOTRACK) skb_defer_max=$(cat /proc/sys/net/core/skb_defer_max)"
echo "## done $(date +%T) $(vcgencmd measure_temp) $(vcgencmd get_throttled)"
