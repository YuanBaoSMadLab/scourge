#!/usr/bin/env bash
set -e
CLANG=/d/LLVM/bin/clang.exe
OBJCOPY=/d/LLVM/bin/llvm-objcopy.exe
CFLAGS="--target=armv7m-none-eabi -mcpu=cortex-m4 -mfloat-abi=soft -O2 -fno-builtin -ffunction-sections -fdata-sections -I."
LDFLAGS="-T stm32f4.ld -fuse-ld=lld -nostdlib -nostartfiles -Wl,--gc-sections"

rm -f diag.elf diag.bin
"$CLANG" $CFLAGS startup.c runtime.c sys.c uart.c diag_main.c -o diag.elf $LDFLAGS
"$OBJCOPY" -O binary diag.elf diag.bin
echo "built diag.elf / diag.bin"
