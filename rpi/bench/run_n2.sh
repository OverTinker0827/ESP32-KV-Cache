#!/bin/bash
# Entry P6: network-path options on v4 (= v3d code + runtime flags), with the OS settings kept from N1:
#   loopback conntrack bypass (raw NOTRACK on lo) and net.core.skb_defer_max=0. Both are restored at the end.
cd ~/kvpi
echo ondemand | sudo tee /sys/devices/system/cpu/cpufreq/policy0/scaling_governor >/dev/null
old_defer=$(cat /proc/sys/net/core/skb_defer_max)
sudo iptables -t raw -I PREROUTING -i lo -j NOTRACK
sudo iptables -t raw -I OUTPUT -o lo -j NOTRACK
sudo /usr/sbin/sysctl -q -w net.core.skb_defer_max=0
echo "## os: notrack=$(sudo iptables -t raw -S | grep -c NOTRACK) skb_defer_max=$(cat /proc/sys/net/core/skb_defer_max)"
declare -A CPUS=([1]=3 [2]=2-3 [3]=1-3) CLI=([1]=0,1 [2]=0,1 [3]=0) CT=([1]=2 [2]=2 [3]=1) LIST=([1]=3 [2]=2,3 [3]=1,2,3)
B=./kvpi/build/v4/kvpi-server-uring
for v in "base:" "mshot:-M 1" "busy20:-b 20" "busy50:-b 50" "mshot_busy50:-M 1 -b 50"; do
  name=${v%%:*}; opt=${v#*:}
  for n in 1 2 3; do
    echo "## ${name}_${n}c $(date +%T) $(vcgencmd measure_temp)"
    SCPU=${CPUS[$n]} CCPU=${CLI[$n]} CT=${CT[$n]} ./kvbench.sh p6_${name}_${n}c $B -p 7777 -c ${LIST[$n]} $opt 2>&1 | tail -4
  done
done
# SQPOLL: one worker on cpu3 + its kernel SQ polling thread on cpu2 (2 cores in total) vs 2 normal workers
echo "## sqpoll_1w_2cores $(date +%T)"
SCPU=2-3 CCPU=0,1 CT=2 ./kvbench.sh p6_sqpoll_2c $B -p 7777 -c 3 -q 2 2>&1 | tail -4
echo "## sqpoll note: server CPU% above counts only the worker; the SQ thread is a kernel thread on cpu2"
sudo iptables -t raw -D PREROUTING -i lo -j NOTRACK
sudo iptables -t raw -D OUTPUT -o lo -j NOTRACK
sudo /usr/sbin/sysctl -q -w net.core.skb_defer_max=$old_defer
echo "## restored: notrack=$(sudo iptables -t raw -S | grep -c NOTRACK) skb_defer_max=$(cat /proc/sys/net/core/skb_defer_max)"
echo "## done $(date +%T) $(vcgencmd measure_temp) $(vcgencmd get_throttled)"
