"""Reset the ESP32 via RTS and echo serial output until a regex matches.

Usage: python tools/serial_wait.py COM5 "READY|Server started" [timeout_s] [--no-reset]
Prints every line; exits 0 on match, 1 on timeout. Lines starting with RESULT/IP are
also written to results/serial_<ts>.log.
"""
import re
import sys
import time
import pathlib
import serial

port, pattern = sys.argv[1], re.compile(sys.argv[2])
timeout = float(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else 60
reset = "--no-reset" not in sys.argv

s = serial.Serial(port, 115200, timeout=0.2)
if reset:
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
log = pathlib.Path(__file__).resolve().parents[1] / "results"
log.mkdir(exist_ok=True)
logf = open(log / f"serial_{time.strftime('%Y%m%d_%H%M%S')}.log", "w", encoding="utf-8")
t_end = time.time() + timeout
buf = b""
while time.time() < t_end:
    buf += s.read(4096)
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        txt = line.decode(errors="replace").rstrip()
        print(txt, flush=True)
        logf.write(txt + "\n")
        if pattern.search(txt):
            logf.close()
            sys.exit(0)
logf.close()
sys.exit(1)
