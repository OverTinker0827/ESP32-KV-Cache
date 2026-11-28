#!/bin/bash
# Every server under identical conditions (stock ondemand governor), one after another.
cd ~/kvpi
echo ondemand | sudo tee /sys/devices/system/cpu/cpufreq/policy0/scaling_governor >/dev/null
DF=~/kvpi/src/df/dragonfly-aarch64
R="--port 7777 --save \"\" --appendonly no --protected-mode no"
run() { echo "## $1  $(date +%T)  $(vcgencmd measure_temp)"; tag=$1; shift; "$@" 2>&1 | grep -v WARNING | tail -8; }
run redis8      ./baseline.sh redis8 redis-server --port 7777 --save "" --appendonly no --protected-mode no
run valkey8     ./baseline.sh valkey8 valkey-server --port 7777 --save "" --appendonly no --protected-mode no
run valkey8_io2 env SCPU=2-3 ./baseline.sh valkey8_io2 valkey-server --port 7777 --save "" --appendonly no --protected-mode no --io-threads 2
run df_1t       ./baseline.sh dragonfly_1t $DF --port 7777 --proactor_threads=1 --dbfilename= --logtostderr
run df_2t       env SCPU=2-3 ./baseline.sh dragonfly_2t $DF --port 7777 --proactor_threads=2 --dbfilename= --logtostderr
run kvpi_v0     ./baseline.sh kvpi_v0 ./kvpi/build/v0/kvpi-server -p 7777
run kvpi_v1_1c  ./baseline.sh kvpi_v1_1c ./kvpi/build/v1/kvpi-server-mt -p 7777 -c 3
run kvpi_v1_2c  env SCPU=2-3 ./baseline.sh kvpi_v1_2c ./kvpi/build/v1/kvpi-server-mt -p 7777 -c 2,3
echo "## done $(date +%T) $(vcgencmd measure_temp) $(vcgencmd get_throttled)"
