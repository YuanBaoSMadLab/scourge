#!/usr/bin/env python3
"""Renode headless driver for STM32F4 bare-metal firmware.

What this does, end to end, is exactly what "flashing and booting" does on real
hardware:

  1. LoadBinary  -> writes firmware.bin into the on-chip FLASH at 0x08000000
                    (this is the programmer / "烧录" step).
  2. Read the reset vector table: SP = FLASH[0], PC = FLASH[4].
  3. Set SP (reg 13) and PC (reg 15) -- identical to the Cortex-M reset sequence.
  4. Hook USART2 DR writes to capture TX into a file (the UART console).
  5. Inject RX characters via WriteChar to drive any interactive shell.

Usage:
  python3 renode_run.py [--bin firmware.bin] [--port 12360]
                        [--cmds "help;led on;crc AB;..."] [--wait 4]
"""
import socket, subprocess, time, os, sys, argparse, struct, re

REN = r"D:\renode\renode.exe"
RESCDIR = r"D:\Scourge\firmware\stm32f4"
PORT = 12360
SP_FALLBACK = 0x20030000  # STM32F407: RAM 0x20000000 + 192K
OUT = r"D:/Scourge/firmware/stm32f4/uart_out.log"


def sanitize(t):
    out = []
    for ch in t:
        if ch == '\n' or (32 <= ord(ch) < 127):
            out.append(ch)
        else:
            out.append('?')
    return ''.join(out)


def parse_vector(binpath):
    """Extract SP (word 0) and reset PC (word 1) from the flash image."""
    with open(binpath, "rb") as f:
        data = f.read(8)
    sp = struct.unpack("<I", data[0:4])[0]
    pc = struct.unpack("<I", data[4:8])[0]
    return sp, pc


def read_hex(resp):
    m = re.search(r'0x[0-9A-Fa-f]+', resp)
    if m:
        return int(m.group(0), 16)
    return None


class Harness:
    def __init__(self, binpath, port=PORT, out=OUT):
        self.binpath = binpath
        self.port = port
        self.out = out
        self.proc = None
        self.s = None
        self.sp = SP_FALLBACK
        self.pc = None

    def _cmd(self, c, dt=0.4):
        self.s.sendall((c + "\n").encode())
        time.sleep(dt)
        try:
            return sanitize(self.s.recv(16384).decode(errors="replace"))
        except Exception:
            return ""

    def start(self):
        if os.path.exists(self.out):
            os.remove(self.out)
        self.proc = subprocess.Popen([REN, "-P", str(self.port), "--disable-gui"],
                                     cwd=RESCDIR, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True)
        time.sleep(4)
        self.s = socket.create_connection(("127.0.0.1", self.port), timeout=10)
        self.s.settimeout(1.0)

    def setup(self):
        self.sp, self.pc = parse_vector(self.binpath)
        self._cmd("using sysbus")
        self._cmd('mach create "stm32f4-fw"')
        self._cmd("machine LoadPlatformDescription @platforms/cpus/stm32f4.repl")
        # The programmer step: flash the raw binary into 0x08000000.
        self._cmd("sysbus LoadBinary @" + self.binpath.replace("\\", "/") + " 0x08000000")
        # Cortex-M reset sequence: SP then PC from the vector table.
        self._cmd("cpu SetRegisterUlong 13 " + hex(self.sp))
        if self.pc is not None:
            self._cmd("cpu SetRegisterUlong 15 " + hex(self.pc))
        # Capture every byte the firmware writes to USART2 DR (0x40004404).
        hook = ('sysbus.usart2 AddAfterWriteDoubleWordHook 0x4 '
               '"f=open(r\'' + self.out + '\',\'ab\'); f.write(chr(int(value) & 255)); f.flush(); f.close()"')
        self._cmd(hook)

    def run(self):
        self._cmd("start")

    def send_line(self, line, dt=0.05):
        for c in line:
            self._cmd("sysbus.usart2 WriteChar " + str(ord(c)), dt=dt)
        self._cmd("sysbus.usart2 WriteChar 13", dt=dt)

    def send_chars(self, s, dt=0.05):
        for c in s:
            self._cmd("sysbus.usart2 WriteChar " + str(ord(c)), dt=dt)

    def quit(self):
        self._cmd("quit", dt=0.5)
        time.sleep(1)
        try:
            self.s.close()
        except Exception:
            pass
        try:
            self.proc.kill()
        except Exception:
            pass
        subprocess.run(["taskkill", "/F", "/IM", "renode.exe"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def read_uart(self):
        if os.path.exists(self.out):
            return open(self.out, "r", errors="replace").read()
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(RESCDIR, "firmware.bin"))
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--cmds", default="")
    ap.add_argument("--wait", type=float, default=3.0)
    ap.add_argument("--after", type=float, default=1.5)
    args = ap.parse_args()

    h = Harness(args.bin, args.port)
    h.start()
    h.setup()
    h.run()
    time.sleep(args.wait)
    if args.cmds:
        for line in args.cmds.split(";"):
            line = line.strip()
            if line:
                h.send_line(line)
                time.sleep(args.after)
    time.sleep(1.0)
    txt = h.read_uart()
    h.quit()
    print(txt)


if __name__ == "__main__":
    main()
