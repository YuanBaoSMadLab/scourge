import socket, subprocess, time, os, sys, struct

REN = r"D:\renode\renode.exe"
RESCDIR = r"D:\Scourge\firmware\stm32f4"
BIN = r"D:\Scourge\firmware\stm32f4\diag.bin"
OUT = r"D:/Scourge/firmware/stm32f4/uart_out.log"
PORT = 12374


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
    with open(BIN, "rb") as f:
        d = f.read(8)
    sp = struct.unpack("<I", d[0:4])[0]
    pc = struct.unpack("<I", d[4:8])[0]
    print("VECTOR SP=0x%08X PC=0x%08X" % (sp, pc))

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
    cmd("machine LoadPlatformDescription @platforms/cpus/stm32f4.repl")
    cmd("sysbus LoadBinary @" + BIN.replace("\\", "/") + " 0x08000000")
    cmd("cpu SetRegisterUlong 13 " + hex(sp))
    cmd("cpu SetRegisterUlong 15 " + hex(pc))
    hook = ('sysbus.usart2 AddAfterWriteDoubleWordHook 0x4 '
           '"f=open(r\'' + OUT + '\',\'ab\'); f.write(chr(int(value) & 255)); f.flush(); f.close()"')
    cmd(hook)
    cmd("start")
    time.sleep(2)
    for addr in [0x20000000, 0x20000004, 0x20000008, 0x2000000C, 0x20000010]:
        r = cmd("sysbus ReadDoubleWord " + hex(addr), dt=0.3)
        print("RAM %s = %s" % (hex(addr), readhex(r)))
    r = cmd("sysbus ReadDoubleWord 0x40004400", dt=0.3)  # USART2_SR
    print("USART2_SR = %s" % readhex(r))
    r = cmd("cpu GetRegisterUlong 15", dt=0.3)
    print("PC = %s" % readhex(r))
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
