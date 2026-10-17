#!/usr/bin/env python3
import socket, subprocess, time, os

REN = r"D:\renode\renode.exe"
RESCDIR = r"D:\Scourge\firmware/stm32f4"
PORT = 12345
OUT = os.path.join(RESCDIR, "uart_out.log")
if os.path.exists(OUT):
    os.remove(OUT)

p = subprocess.Popen([REN, "-P", str(PORT), "--disable-gui"], cwd=RESCDIR,
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
time.sleep(4)
s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
s.settimeout(1.0)

def r():
    try:
        return s.recv(4096).decode(errors="replace")
    except Exception:
        return ""

def cmd(c):
    s.sendall((c + "\n").encode())
    time.sleep(0.5)
    out = r()
    print("CMD> " + c)
    print(out)

cmd("using sysbus")
cmd('mach create "t"')
cmd("machine LoadPlatformDescription @platforms/cpus/stm32f4.repl")
cmd("sysbus LoadELF @/d/Scourge/firmware/stm32f4/firmware.elf")
cmd('python "print(sysbus)"')
cmd("start")
cmd('python "of=open(r\'/d/Scourge/firmware/stm32f4/uart_out.log\',\'w\'); sysbus.usart2.CharReceived += (lambda f: (lambda b: (f.write(chr(b)), f.flush())))(of)"')
time.sleep(3)
cmd("quit")
time.sleep(1)
print("=== RENODE STDOUT (tail) ===")
print(p.stdout.read()[-2500:])
p.wait()
print("=== OUT FILE ===")
if os.path.exists(OUT):
    print(open(OUT, "r", errors="replace").read())
else:
    print("(no out file)")
