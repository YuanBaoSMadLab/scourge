#!/usr/bin/env python3
import socket, subprocess, time, os

REN = r"D:\renode\renode.exe"
RESCDIR = r"D:\Scourge\firmware\stm32f4"
PORT = 12354
ELF = r"D:/Scourge/firmware/stm32f4/firmware.elf"
OUT = r"D:/Scourge/firmware/stm32f4/uart_out.log"

def sanitize(t):
    out=[]
    for ch in t:
        if ch=='\n' or (32<=ord(ch)<127): out.append(ch)
        else: out.append('?')
    return ''.join(out)

if os.path.exists(OUT):
    os.remove(OUT)

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
    time.sleep(0.5)
    return sanitize(r())

cmd("using sysbus")
cmd('mach create "t"')
cmd("machine LoadPlatformDescription @platforms/cpus/stm32f4.repl")
cmd("sysbus LoadELF @" + ELF)
hook = ('sysbus.usart2 AddAfterWriteDoubleWordHook 0x4 '
       '"f=open(r\'' + OUT + '\',\'ab\'); f.write(chr(value & 255)); f.flush(); f.close()"')
print("HOOK CMD:", hook)
print(cmd(hook))
cmd("start")
time.sleep(4)
print("=== OUT FILE ===")
if os.path.exists(OUT):
    print(open(OUT, "r", errors="replace").read())
else:
    print("(no out file)")
cmd("quit")
time.sleep(1)
p.stdout.read()
p.wait()
print("done")
