#!/usr/bin/env python3
import socket, subprocess, time, os, sys

REN = r"D:\renode\renode.exe"
RESCDIR = r"D:\Scourge\firmware\stm32f4"
RESC = os.path.join(RESCDIR, "renode.resc")
PORT = 12345
OUT = os.path.join(RESCDIR, "uart_out.log")

if os.path.exists(OUT):
    os.remove(OUT)

p = subprocess.Popen([REN, "-P", str(PORT), "--disable-gui", "--hide-log", "--hide-analyzers", RESC],
                     cwd=RESCDIR, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

time.sleep(6)
print("=== uart_out.log exists:", os.path.exists(OUT))
if os.path.exists(OUT):
    print("=== content ===")
    print(open(OUT, "r", errors="replace").read())

subprocess.run(["taskkill", "/F", "/IM", "renode.exe"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
