#!/bin/bash
# Entry P5: v3 steps. Engine bench (performance governor, as in P2) then servers (ondemand, as in P4).
cd ~/kvpi
echo performance | sudo tee /sys/devices/system/cpu/cpufreq/policy0/scaling_governor >/dev/null
for V in v0 v3a v3b; do echo "## engine $V"; ./kvpi/build/$V/bench_engine; done
echo ondemand | sudo tee /sys/devices/system/cpu/cpufreq/policy0/scaling_governor >/dev/null
echo "## governor $(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor)"
declare -A CPUS=([1]=3 [2]=2-3 [3]=1-3) CLI=([1]=0,1 [2]=0,1 [3]=0) CT=([1]=2 [2]=2 [3]=1) LIST=([1]=3 [2]=2,3 [3]=1,2,3)
for V in v2 v3a v3b v3c v3d; do for n in 1 2 3; do
  echo "## ${V}_${n}c $(date +%T) $(vcgencmd measure_temp)"
  SCPU=${CPUS[$n]} CCPU=${CLI[$n]} CT=${CT[$n]} ./kvbench.sh p5_${V}_${n}c ./kvpi/build/$V/kvpi-server-uring -p 7777 -c ${LIST[$n]} 2>&1 | tail -4
done; done
echo "## done $(date +%T) $(vcgencmd measure_temp) $(vcgencmd get_throttled)"
