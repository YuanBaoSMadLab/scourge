import socket, subprocess, time, os, sys

REN = r"D:\renode\renode.exe"
RESCDIR = r"D:\Scourge\firmware\stm32f4"
BIN = r"D:\Scourge\firmware\stm32f4\firmware.bin"
PORT = 12373


def sanitize(t):
    out = []
    for ch in t:
        if ch == '\n' or (32 <= ord(ch) < 127):
            out.append(ch)
        else:
            out.append('?')
    return ''.join(out)


def main():
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
        print("[CMD] %s\n[RES] %s\n" % (c, r.strip()))
        return r

    cmd("using sysbus")
    cmd('mach create "x"')
    cmd("machine LoadPlatformDescription @platforms/cpus/stm32f4.repl")
    print("----- try WITHOUT @ -----")
    cmd("sysbus LoadBinary " + BIN.replace("\\", "/") + " 0x08000000")
    cmd("sysbus ReadDoubleWord 0x08000000")
    cmd("sysbus ReadDoubleWord 0x08000004")
    print("----- try WITH @ -----")
    cmd("sysbus LoadBinary @" + BIN.replace("\\", "/") + " 0x08000000")
    cmd("sysbus ReadDoubleWord 0x08000000")
    cmd("sysbus ReadDoubleWord 0x08000004")
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
