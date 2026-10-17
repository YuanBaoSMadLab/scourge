import socket, subprocess, time, os, sys, struct

REN = r"D:\renode\renode.exe"
RESCDIR = r"D:\Scourge\firmware\stm32f4"
BIN = r"D:\Scourge\firmware\stm32f4\diag.bin"
REPL = "stm32f4_boot.repl"
OUT = r"D:/Scourge/firmware/stm32f4/uart_out.log"
PORT = 12376


def sanitize(t):
    out = []
    for ch in t:
        if ch == '\n' or (32 <= ord(ch) < 127):
            out.append(ch)
        else:
            out.append('?')
    return ''.join(out)


def readhex(r):
    import re
    m = re.search(r'0x[0-9A-Fa-f]+', r)
    return m.group(0) if m else r.strip()


def main():
    if os.path.exists(OUT):
        os.remove(OUT)
    proc = subprocess.Popen([REN, "-P", str(PORT), "--disable-gui"],
                            cwd=RESCDIR, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    time.sleep(4)
    s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
    s.settimeout(1.0)

    def cmd(c, dt=0.5):
        s.sendall((c + "\n").encode())
        time.sleep(dt)
        try:
            r = sanitize(s.recv(16384).decode(errors="replace"))
        except Exception as e:
            r = "<timeout:%s>" % e
        return r

    cmd("using sysbus")
    cmd('mach create "x"')
    cmd("machine LoadPlatformDescription @" + REPL)
    # Flash the image at the real FLASH base AND at the boot alias 0x0 so the
    # Cortex-M reset vector table is readable at 0x00000000.
    cmd("sysbus LoadBinary @" + BIN.replace("\\", "/") + " 0x08000000")
    cmd("sysbus LoadBinary @" + BIN.replace("\\", "/") + " 0x00000000")
    cmd("sysbus ReadDoubleWord 0x00000000")
    cmd("sysbus ReadDoubleWord 0x00000004")
    hook = ('sysbus.usart2 AddAfterWriteDoubleWordHook 0x4 '
           '"f=open(r\'' + OUT + '\',\'ab\'); f.write(chr(int(value) & 255)); f.flush(); f.close()"')
    cmd(hook)
    cmd("start")
    time.sleep(2)
    r = cmd("cpu GetRegisterUlong 15", dt=0.3)
    print("PC after start = %s" % readhex(r))
    for addr in [0x20000000, 0x20000008, 0x2000000C, 0x20000010]:
        r = cmd("sysbus ReadDoubleWord " + hex(addr), dt=0.3)
        print("RAM %s = %s" % (hex(addr), readhex(r)))
    try:
        txt = open(OUT).read() if os.path.exists(OUT) else "(no uart file)"
    except Exception as e:
        txt = "(read err %s)" % e
    print("=== UART ===\n%r" % txt)
    cmd("quit", dt=1.0)
    time.sleep(1)
    try:
        s.close()
    except Exception:
        pass
    try:
        proc.kill()
    except Exception:
        pass
    subprocess.run(["taskkill", "/F", "/IM", "renode.exe"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
