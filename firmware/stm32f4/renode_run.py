#!/usr/bin/env python3
"""Renode headless driver for STM32F4 bare-metal firmware.

This reproduces the real "flash and boot" flow on an STM32F4 Discovery:

  1. LoadBinary  -> write firmware.bin into on-chip FLASH at 0x08000000
                    AND into the boot alias at 0x00000000 (STM32 aliases FLASH
                    to 0x0 at reset so the Cortex-M reset vector table is
                    readable there). This is the "烧录" step.
  2. The CPU reset sequence reads SP=*(0x0), PC=*(0x4) and starts executing.
  3. Hook USART2 DR writes to capture TX into a file (the UART console).
  4. Inject RX characters via WriteChar to drive the interactive shell.

Usage:
  python3 renode_run.py [--bin firmware.bin] [--port 12360]
                        [--cmds "help;led on;crc AB;..."] [--wait 4]
"""
import socket, subprocess, time, os, sys, argparse, struct

REN = r"D:\renode\renode.exe"
RESCDIR = r"D:\Scourge\firmware\stm32f4"
PORT = 12360
BOOTREPL = "stm32f4_boot.repl"   # stm32f4.repl + a 0x00000000 boot alias
OUT = r"D:/Scourge/firmware/stm32f4/uart_out.log"


def sanitize(t):
    out = []
    for ch in t:
        if ch == '\n' or (32 <= ord(ch) < 127):
            out.append(ch)
        else:
            out.append('?')
    return ''.join(out)


class Harness:
    def __init__(self, binpath, port=PORT, out=OUT):
        self.binpath = binpath
        self.port = port
        self.out = out
        self.proc = None
        self.s = None

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
        binf = "@" + self.binpath.replace("\\", "/")
        self._cmd("using sysbus")
        self._cmd('mach create "stm32f4-fw"')
        self._cmd("machine LoadPlatformDescription @" + BOOTREPL)
        # Flash the image at the real FLASH base and at the boot alias 0x0.
        self._cmd("sysbus LoadBinary " + binf + " 0x08000000")
        self._cmd("sysbus LoadBinary " + binf + " 0x00000000")
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
