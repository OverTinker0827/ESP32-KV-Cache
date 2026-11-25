"""Find the ESP32 on the LAN: probe port 6379 on the /24 and confirm the MAC via ARP.

Usage: python tools/find_board.py [subnet_prefix=192.168.1] [mac=78-ee-4c-01-13-3c] [wait_s=90]
Prints "FOUND <ip>" and exits 0 once a host answers +PONG.
"""
import concurrent.futures as cf
import socket
import subprocess
import sys
import time

prefix = sys.argv[1] if len(sys.argv) > 1 else "192.168.1"
mac = (sys.argv[2] if len(sys.argv) > 2 else "78-ee-4c-01-13-3c").lower()
wait = float(sys.argv[3]) if len(sys.argv) > 3 else 90


def probe(ip):
    try:
        s = socket.create_connection((ip, 6379), timeout=0.6)
        s.settimeout(3)
        s.sendall(b"*1\r\n$4\r\nPING\r\n")
        r = s.recv(64)
        s.close()
        return ip if r.startswith(b"+PONG") else None
    except OSError:
        return None


def arp_ip():
    out = subprocess.run(["arp", "-a"], capture_output=True, text=True).stdout.lower()
    for line in out.splitlines():
        if mac in line:
            return line.split()[0]
    return None


t_end = time.time() + wait
rnd = 0
while time.time() < t_end:
    rnd += 1
    ip = arp_ip()
    cands = [ip] if ip else []
    if ip and probe(ip):
        print("FOUND", ip, "(by MAC)")
        sys.exit(0)
    with cf.ThreadPoolExecutor(128) as ex:
        hits = [h for h in ex.map(probe, [f"{prefix}.{i}" for i in range(1, 255)]) if h]
    if hits:
        print("FOUND", hits[0], "arp_mac_ip=", arp_ip())
        sys.exit(0)
    print(f"round {rnd}: not found yet (arp says {ip})", flush=True)
    time.sleep(3)
print("NOT FOUND")
sys.exit(1)
