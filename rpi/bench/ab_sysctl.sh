#!/bin/bash
# A/B kernel knobs on v3d (GET+SET, P=1 and P=16, 1c and 2+2), conntrack bypass ON for all runs.
# Each knob is set, measured, and restored to its original value.
cd ~/kvpi
echo ondemand | sudo tee /sys/devices/system/cpu/cpufreq/policy0/scaling_governor >/dev/null
sudo iptables -t raw -I PREROUTING -i lo -j NOTRACK; sudo iptables -t raw -I OUTPUT -o lo -j NOTRACK
run() {
  SCPU=3   CCPU=0,1 CT=2 ./kvbench.sh n1_${1}_1c ./kvpi/build/v3d/kvpi-server-uring -p 7777 -c 3   2>&1 | tail -4
  SCPU=2-3 CCPU=0,1 CT=2 ./kvbench.sh n1_${1}_2c ./kvpi/build/v3d/kvpi-server-uring -p 7777 -c 2,3 2>&1 | tail -4
}
for knob in net.ipv4.tcp_timestamps=0 net.core.skb_defer_max=0; do
  k=${knob%=*}; old=$(cat /proc/sys/$(echo $k | tr . /))
  sudo /usr/sbin/sysctl -q -w $knob
  echo "## $knob (was $old) $(date +%T)"; run ${k##*.}
  sudo /usr/sbin/sysctl -q -w $k=$old; echo "## restored $k=$(cat /proc/sys/$(echo $k | tr . /))"
done
sudo iptables -t raw -D PREROUTING -i lo -j NOTRACK; sudo iptables -t raw -D OUTPUT -o lo -j NOTRACK
echo "## raw rules left: $(sudo iptables -t raw -S | grep -c NOTRACK)  $(vcgencmd measure_temp) $(vcgencmd get_throttled)"
