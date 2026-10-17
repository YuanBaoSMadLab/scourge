import socket, subprocess, time, os, sys

REN = r"D:\renode\renode.exe"
RESCDIR = r"D:\Scourge\firmware\stm32f4"
ELF = r"D:\Scourge\firmware\stm32f4\firmware.elf"
OUT = r"D:/Scourge/firmware/stm32f4/uart_out.log"
PORT = 12371
SP_VALUE = 0x20030000


def sanitize(t):
    out = []
    for ch in t:
        if ch == '\n' or (32 <= ord(ch) < 127):
            out.append(ch)
        else:
            out.append('?')
    return ''.join(out)


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
        print("[CMD] %s\n[RES] %s\n" % (c, r))
        return r

    cmd("using sysbus")
    cmd('mach create "x"')
    cmd("machine LoadPlatformDescription @platforms/cpus/stm32f4.repl")
    cmd("sysbus LoadELF @" + ELF.replace("\\", "/"))
    cmd("cpu SetRegisterUlong 13 " + hex(SP_VALUE))
    hook = ('sysbus.usart2 AddAfterWriteDoubleWordHook 0x4 '
           '"f=open(r\'' + OUT + '\',\'ab\'); f.write(chr(int(value) & 255)); f.flush(); f.close()"')
    cmd(hook)
    cmd("cpu GetRegisterUlong 13")
    cmd("cpu GetRegisterUlong 15")
    cmd("start")
    time.sleep(5)
    try:
        txt = open(OUT).read() if os.path.exists(OUT) else "(no uart file)"
    except Exception as e:
        txt = "(read err %s)" % e
    print("=== UART ===\n%s" % txt)
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
