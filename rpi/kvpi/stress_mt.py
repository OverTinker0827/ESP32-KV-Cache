"""Concurrency stress for kvpi-server-mt: many connections hammer a SMALL key set (so the same
stripes are written and read from every core at once). Every value is self-checking
(key | writer id | counter | length | crc32 of the body | padding), so a torn or mixed-up read
from the seqlock path shows up as a checksum or key mismatch.
usage: python3 stress_mt.py [port] [procs] [seconds]"""
import multiprocessing as mp
import random
import socket
import sys
import time
import zlib

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7777
PROCS = int(sys.argv[2]) if len(sys.argv) > 2 else 8
SECS = float(sys.argv[3]) if len(sys.argv) > 3 else 20
NKEYS = 64


def cmd(*a):
    out = b"*%d\r\n" % len(a)
    for x in a:
        out += b"$%d\r\n%s\r\n" % (len(x), x)
    return out


class Conn:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", PORT))
        self.buf = b""

    def line(self):
        while b"\r\n" not in self.buf:
            self.buf += self.s.recv(65536)
        l, self.buf = self.buf.split(b"\r\n", 1)
        return l

    def bulk(self):
        h = self.line()
        if h == b"$-1":
            return None
        n = int(h[1:])
        while len(self.buf) < n + 2:
            self.buf += self.s.recv(65536)
        v, self.buf = self.buf[:n], self.buf[n + 2:]
        return v


def make_val(key, wid, ctr):
    body = b"%s|%d|%d|" % (key, wid, ctr) + bytes([65 + (ctr % 26)]) * random.choice([0, 5, 40, 200, 1500, 9000])
    return body + b"#%08x" % zlib.crc32(body)


def check(key, v):
    body, tail = v[:-9], v[-9:]
    if tail[:1] != b"#" or int(tail[1:], 16) != zlib.crc32(body):
        return "crc"
    if body.split(b"|", 1)[0] != key:
        return "key"
    return None


def run(wid, q):
    random.seed(wid)
    c = Conn()
    ops = bad = 0
    t_end = time.time() + SECS
    while time.time() < t_end:
        batch = []
        for _ in range(random.choice([1, 1, 4, 16])):  # also pipelined
            key = b"sk:%d" % random.randrange(NKEYS)
            if random.random() < 0.4:
                batch.append(("S", key))
                c.s.sendall(cmd(b"SET", key, make_val(key, wid, ops)))
            else:
                batch.append(("G", key))
                c.s.sendall(cmd(b"GET", key))
            ops += 1
        for kind, key in batch:
            if kind == "S":
                if c.line() != b"+OK":
                    bad += 1
            else:
                v = c.bulk()
                if v is not None and check(key, v):
                    bad += 1
    q.put((ops, bad))


if __name__ == "__main__":
    q = mp.Queue()
    ps = [mp.Process(target=run, args=(i, q)) for i in range(PROCS)]
    for p in ps:
        p.start()
    tot = [q.get() for _ in ps]
    for p in ps:
        p.join()
    ops, bad = sum(t[0] for t in tot), sum(t[1] for t in tot)
    c = Conn()
    c.s.sendall(cmd(b"INFO"))
    info = c.bulk().decode()
    print("ops %d  bad %d  | %s" % (ops, bad, " ".join(info.split()[1:])))
    sys.exit(1 if bad else 0)
