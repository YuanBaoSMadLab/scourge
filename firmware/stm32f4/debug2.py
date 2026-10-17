#!/usr/bin/env python3
import socket, subprocess, time, os

REN = r"D:\renode\renode.exe"
RESCDIR = r"D:\Scourge\firmware\stm32f4"
PORT = 12346
ELF = r"D:/Scourge/firmware/stm32f4/firmware.elf"

p = subprocess.Popen([REN, "-P", str(PORT), "--disable-gui"], cwd=RESCDIR,
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
time.sleep(4)
s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
s.settimeout(1.0)

def r():
    try:
        return s.recv(8192).decode(errors="replace")
    except Exception:
        return ""

def cmd(c):
    s.sendall((c + "\n").encode())
    time.sleep(0.5)
    out = r()
    print("CMD> " + c)
    print(out)
    return out

cmd("using sysbus")
cmd('mach create "t"')
cmd("machine LoadPlatformDescription @platforms/cpus/stm32f4.repl")
cmd("sysbus LoadELF @" + ELF)
cmd('python "print(dir())"')
cmd('python "print(emulationManager is not None)"')
cmd("Create Terminal Tester sysbus.usart2")
cmd('python "print([x for x in dir() if \'erm\' in x or \'ysbus\' in x or \'achine\' in x])"')
cmd("start")
time.sleep(3)
cmd("pause")
cmd("quit")
time.sleep(1)
print("=== RENODE STDOUT tail ===")
print(p.stdout.read()[-1500:])
p.wait()
