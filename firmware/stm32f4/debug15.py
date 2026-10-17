#!/usr/bin/env python3
import socket, subprocess, time, os

REN = r"D:\renode\renode.exe"
RESCDIR = r"D:\Scourge/firmware\stm32f4"
PORT = 12359
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
for q in ["cpu SetRegisterUlong SP 0x20030000",
          "cpu SetRegisterUlong 13 0x20030000",
          "cpu SetRegister 13 0x20030000",
          "sysbus.cpu SetRegisterUlong 13 0x20030000"]:
    print(">>>", q)
    print(cmd(q))
print("SP now:", cmd("cpu GetRegisterUlong 13"))
cmd("quit")
time.sleep(1)
p.stdout.read()
p.wait()
print("done")
