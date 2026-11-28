#!/bin/bash
# N1 A/B: loopback conntrack bypass (raw-table NOTRACK on lo). Rules are non-persistent and removed at the end.
cd ~/kvpi
echo ondemand | sudo tee /sys/devices/system/cpu/cpufreq/policy0/scaling_governor >/dev/null
run() { # tag
  SCPU=3   CCPU=0,1 CT=2 ./kvbench.sh n1_${1}_1c ./kvpi/build/v3d/kvpi-server-uring -p 7777 -c 3   2>&1 | tail -4
  SCPU=2-3 CCPU=0,1 CT=2 ./kvbench.sh n1_${1}_2c ./kvpi/build/v3d/kvpi-server-uring -p 7777 -c 2,3 2>&1 | tail -4
}
echo "## tracked $(date +%T)"; run track
sudo iptables -t raw -I PREROUTING -i lo -j NOTRACK
sudo iptables -t raw -I OUTPUT -o lo -j NOTRACK
sudo iptables -t raw -S
echo "## notrack $(date +%T)"; run notrack
sudo iptables -t raw -D PREROUTING -i lo -j NOTRACK
sudo iptables -t raw -D OUTPUT -o lo -j NOTRACK
echo "## rules removed:"; sudo iptables -t raw -S
echo "## done $(vcgencmd measure_temp) $(vcgencmd get_throttled)"
