# Boot microredis on the ESP32 under identical conditions to the C builds:
#  - CPU at 240 MHz
#  - WiFi STA, power-save disabled (modem sleep adds ms-scale latency)
import gc
import json
import machine
import network
import time

machine.freq(240_000_000)
# weak 3V3 supply on this board: disable brownout detector before the radio starts
machine.mem32[0x3FF480D4] = 0

with open("wifi_secrets.json") as f:
    cfg = json.load(f)

try:
    network.hostname("microredis")
except Exception:
    pass
wlan = network.WLAN(network.STA_IF)
wlan.active(True)
try:
    wlan.config(pm=wlan.PM_NONE)
except Exception as e:
    print("pm config failed", e)
if not wlan.isconnected():
    wlan.connect(cfg["ssid"], cfg["password"])
    t0 = time.ticks_ms()
    while not wlan.isconnected():
        if time.ticks_diff(time.ticks_ms(), t0) > 30000:
            raise RuntimeError("wifi timeout")
        time.sleep_ms(100)
print("IP", wlan.ifconfig()[0])

gc.collect()
print("heap_free_before_import", gc.mem_free())
from microredis.main import run  # noqa: E402

gc.collect()
print("heap_free_after_import", gc.mem_free())
run()
