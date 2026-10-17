#!/usr/bin/env bash
set -e
CLANG=/d/LLVM/bin/clang.exe
OBJCOPY=/d/LLVM/bin/llvm-objcopy.exe
CFLAGS="--target=armv7m-none-eabi -mcpu=cortex-m4 -mfloat-abi=soft -O2 -g -fno-builtin -ffunction-sections -fdata-sections -I."
LDFLAGS="-T stm32f4.ld -fuse-ld=lld -nostdlib -nostartfiles -Wl,--gc-sections"

rm -f firmware.elf firmware.bin
"$CLANG" $CFLAGS startup.c runtime.c sys.c uart.c crc32.c filter.c stats.c sensor.c shell.c main.c -o firmware.elf $LDFLAGS
"$OBJCOPY" -O binary firmware.elf firmware.bin
echo "built firmware.elf / firmware.bin"
