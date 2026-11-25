# Bring WiFi up step by step so a brownout shows exactly where it happens.
import json
import machine
import network
import time

# Board's 3V3 rail sags when the radio starts -> disable the brownout detector
# (RTC_CNTL_BROWN_OUT_REG). Workaround for a weak supply; noted in results.
machine.mem32[0x3FF480D4] = 0
print("brownout detector disabled")

TXPOWER = 8.5  # dBm (default ~20). Lower = smaller current spikes.
print("step1 creating WLAN")
w = network.WLAN(network.STA_IF)
print("step2 activating radio (RF calibration current spike)")
w.active(True)
print("step3 active ok")
w.config(txpower=TXPOWER)
try:
    w.config(pm=w.PM_NONE)
except Exception:
    pass
print("step4 txpower", w.config("txpower"))
c = json.load(open("wifi_secrets.json"))
w.connect(c["ssid"], c["password"])
print("step5 connecting")
t = time.ticks_ms()
while not w.isconnected() and time.ticks_diff(time.ticks_ms(), t) < 20000:
    time.sleep_ms(250)
if w.isconnected():
    print("CONNECTED", w.ifconfig()[0], "rssi", w.status("rssi"))
else:
    print("FAILED status", w.status())
