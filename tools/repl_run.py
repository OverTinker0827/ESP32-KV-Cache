"""Run `import <module>` on the MicroPython REPL and stream output, surviving resets.

Usage: python tools/repl_run.py COM5 wifi_test 40 ["stop regex"]
Unlike mpremote, this never hangs if the board browns out / resets mid-run.
"""
import re
import sys
import time
import serial

port, mod = sys.argv[1], sys.argv[2]
timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 40
stop = re.compile(sys.argv[4]) if len(sys.argv) > 4 else None
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dtr = False  # don't reset the board on open
s.rts = False
s.open()
# wait for a prompt (board may be rebooting after an earlier open)
t0 = time.time()
seen = b""
while time.time() - t0 < 8:
    s.write(b"\r\x03")
    time.sleep(0.3)
    seen += s.read(4096)
    if b">>> " in seen:
        break
s.reset_input_buffer()
s.write(f"import {mod}\r".encode())
t_end = time.time() + timeout
buf = b""
while time.time() < t_end:
    buf += s.read(4096)
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        txt = line.decode(errors="replace").rstrip()
        print(txt, flush=True)
        if stop and stop.search(txt):
            sys.exit(0)
print(buf.decode(errors="replace"))
