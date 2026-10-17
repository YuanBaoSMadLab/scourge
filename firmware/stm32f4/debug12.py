#!/usr/bin/env python3
import socket, subprocess, time, os

REN = r"D:\renode\renode.exe"
RESCDIR = r"D:\Scourge\firmware\stm32f4"
PORT = 12356
ELF = r"D:/Scourge/firmware/stm32f4/firmware.elf"

def sanitize(t):
    out=[]
    for ch in t:
        if ch=='\n' or (32<=ord(ch)<127): out.append(ch)
        else: out.append('?')
    return ''.join(out)

p = subprocess.Popen([REN, "-P", str(PORT), "--disable-gui"], cwd=RESCDIR,
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
time.sleep(4)
s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
s.settimeout(1.0)

def r():
    try:
        return s.recv(16384).decode(errors="replace")
    except Exception:
        return ""

def cmd(c):
    s.sendall((c + "\n").encode())
    time.sleep(0.4)
    return sanitize(r())

cmd("using sysbus")
cmd('mach create "t"')
cmd("machine LoadPlatformDescription @platforms/cpus/stm32f4.repl")
cmd("sysbus LoadELF @" + ELF)
cmd("start")
time.sleep(0.3)
print("SP0 :", cmd("cpu GetRegisterUlong sp"))
print("PC0 :", cmd("cpu GetRegisterUlong pc"))
time.sleep(2)
print("PC1 :", cmd("cpu GetRegisterUlong pc"))
cmd("quit")
time.sleep(1)
p.stdout.read()
p.wait()
print("done")
