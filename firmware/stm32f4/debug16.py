#!/usr/bin/env python3
import socket, subprocess, time, os, sys
sys.path.insert(0, r"D:\Scourge\firmware\stm32f4")
from renode_run import Harness, RESCDIR

h = Harness(os.path.join(RESCDIR, "firmware.elf"), 12361)
h.start()
h._cmd("using sysbus")
h._cmd('mach create "t"')
h._cmd("machine LoadPlatformDescription @platforms/cpus/stm32f4.repl")
h._cmd("sysbus LoadELF @" + h.elf)
h._cmd("cpu SetRegisterUlong 13 " + hex(h.SP_VALUE))
print("SP before start:", h._cmd("cpu GetRegisterUlong 13"))
h._cmd('sysbus.usart2 AddAfterWriteDoubleWordHook 0x4 "f=open(r\'' + h.out + '\',\'ab\'); f.write(chr(int(value) & 255)); f.flush(); f.close()"')
h.run()
time.sleep(0.5)
print("SP after start (0.5s):", h._cmd("cpu GetRegisterUlong 13"))
print("PC after start:", h._cmd("cpu GetRegisterUlong 15"))
time.sleep(3)
print("SP after 3s:", h._cmd("cpu GetRegisterUlong 13"))
print("OUT exists:", os.path.exists(h.out), "size:", os.path.getsize(h.out) if os.path.exists(h.out) else 0)
print("OUT content:", repr(open(h.out).read() if os.path.exists(h.out) else ""))
h.quit()
print("done")
