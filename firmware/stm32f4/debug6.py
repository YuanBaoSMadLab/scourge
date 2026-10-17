#!/usr/bin/env python3
import socket, subprocess, time, os, sys

REN = r"D:\renode\renode.exe"
RESCDIR = r"D:\Scourge\firmware\stm32f4"
PORT = 12350
ELF = r"D:/Scourge/firmware/stm32f4/firmware.elf"
OUT = r"D:/Scourge/firmware/stm32f4/uart_out.log"
LOG = r"D:/Scourge/firmware/stm32f4/debug6.log"
if os.path.exists(OUT):
    os.remove(OUT)

def log(m):
    with open(LOG, "a", encoding="utf-8", errors="replace") as f:
        f.write(m + "\n")

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
    time.sleep(0.4)
    out = r()
    log("CMD> " + c)
    log(out)
    return out

cmd("using sysbus")
cmd('mach create "t"')
cmd("machine LoadPlatformDescription @platforms/cpus/stm32f4.repl")
cmd("sysbus LoadELF @" + ELF)
cmd('python "print(emulationManager.CurrentEmulation.Machines.Count)"')
cmd('python "m=emulationManager.CurrentEmulation.Machines[0]; sb=m.SystemBus; print(type(sb)); print([n for n in dir(sb) if \'usart\' in n.lower() or \'Periph\' in n])"')
cmd('python "m=emulationManager.CurrentEmulation.Machines[0]; sb=m.SystemBus; u=sb.usart2; print(\'u=\', u is not None); print([n for n in dir(u) if \'Char\' in n or \'Log\' in n or \'Write\' in n])"')
cmd('python "m=emulationManager.CurrentEmulation.Machines[0]; sb=m.SystemBus; u=sb.usart2; of=open(r\'' + OUT + '\',\'w\'); u.CharReceived+=(lambda f:(lambda b:(f.write(chr(b)),f.flush())))(of); print(\'hook set\')"')
cmd("start")
time.sleep(4)
cmd("cpu")
cmd("quit")
time.sleep(1)
log("=== OUT FILE ===")
if os.path.exists(OUT):
    log(open(OUT, "r", errors="replace").read())
else:
    log("(no out file)")
p.stdout.read()
p.wait()
print("done; see debug6.log")
