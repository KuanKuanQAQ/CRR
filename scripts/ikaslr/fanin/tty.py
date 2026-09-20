#!/usr/bin/env python3
# Send a line to the guest serial console and wait for a marker in serial.log.
import socket, sys, time, os
W = os.path.dirname(os.path.abspath(__file__))
cmd = sys.argv[1]
marker = sys.argv[2] if len(sys.argv) > 2 else None
timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 60
log = os.path.join(W, "serial.log")
start = os.path.getsize(log)
s = socket.socket(socket.AF_UNIX); s.connect(os.path.join(W, "tty.sock"))
s.sendall((cmd + "\n").encode())
t0 = time.time()
while marker and time.time() - t0 < timeout:
    with open(log, errors="replace") as f:
        f.seek(start); out = f.read()
    if marker in out:
        print(out[-3000:]); sys.exit(0)
    time.sleep(0.5)
if marker:
    with open(log, errors="replace") as f:
        f.seek(start); print(f.read()[-3000:])
    print("TIMEOUT"); sys.exit(1)
