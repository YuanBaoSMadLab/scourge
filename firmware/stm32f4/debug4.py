#!/usr/bin/env python3
import socket, subprocess, time, os

REN = r"D:\renode\renode.exe"
RESCDIR = r"D:\Scourge\firmware\stm32f4"
PORT = 12348
ELF = r"D:/Scourge/firmware/stm32f4/firmware.elf"
OUT = r"D:/Scourge/firmware/stm32f4/uart_out.log"
if os.path.exists(OUT):
    os.remove(OUT)

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
    print("CMD> " + c)
    print(out)
    return out

cmd("using sysbus")
cmd('mach create "t"')
cmd("machine LoadPlatformDescription @platforms/cpus/stm32f4.repl")
cmd("sysbus LoadELF @" + ELF)
cmd('python "sb=variables[\'sysbus\']; print(type(sb)); print([n for n in dir(sb) if \'usart\' in n.lower()])"')
cmd('python "sb=variables[\'sysbus\']; u=sb.GetPeripheralByName(\'usart2\'); print(u is not None)"')
cap = ('python "'
       'sb=variables[\'sysbus\'];'
       'u=sb.GetPeripheralByName(\'usart2\');'
       'of=open(r\'' + OUT + '\',\'w\');'
       'u.CharReceived+=(lambda f:(lambda b:(f.write(chr(b)),f.flush())))(of)"')
cmd(cap)
cmd("start")
time.sleep(4)
cmd("quit")
time.sleep(1)
print("=== OUT FILE ===")
if os.path.exists(OUT):
    print(repr(open(OUT, "r", errors="replace").read()))
else:
    print("(no out file)")
p.stdout.read()
p.wait()
