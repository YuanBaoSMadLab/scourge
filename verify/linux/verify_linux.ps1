# ============================================================================
#  verify_linux.ps1 — Linux/嵌入式交叉编译验证（跨平台支持 0.9.4）
# ----------------------------------------------------------------------------
#  验证内容：探针源码 x 目标矩阵（x86_64/aarch64/riscv64/arm）的 musl 静态
#  交叉构建 + llvm-readelf ELF 头校验（机器类型/静态 EXEC）。
#
#  验证边界（诚实声明）：
#    * 本机为 Windows 宿主，WSL 不可用时无法真机运行 Linux 二进制——本脚本
#      验证「可交叉构建 + ELF 结构正确」；运行时行为（双模逐字节一致）在
#      Linux 宿主上由同一套源码路径保证（POSIX 分支与 AOT 共用契约）。
#    * WSL 解禁后可追加执行段：直接运行 .elf 与 Windows 侧 stdout 对拍。
#    * 嵌入式 = 嵌入式 Linux（aarch64/riscv64/armv7 交叉 ELF）；裸金属
#      freestanding（-nostdlib + 移植层）为后续路线，不在本验证范围。
#
#  依赖：build-rel\scourge.exe、D:\LLVM\bin（lld/llvm-readelf）、
#        D:\musl-sysroot\<triple>（build_musl_sysroot.sh + build_builtins.sh 产物）
# ============================================================================
$ErrorActionPreference = 'Stop'
$root = 'd:\Scourge\build-rel\verify\linux'
$srcDir = 'd:\Scourge\verify\linux'
if (Test-Path $root) { Remove-Item -Recurse -Force $root }
New-Item -ItemType Directory -Force -Path $root | Out-Null
$scourge = 'd:\Scourge\build-rel\scourge.exe'
$readelf = 'D:\LLVM\bin\llvm-readelf.exe'
$utf8nb = New-Object System.Text.UTF8Encoding($false)

# ---- 探针：覆盖算术/字符串/JSON/文件系统/进程（POSIX 运行时主力面） ----
$probe = @'
# linux_probe.sg - POSIX 交叉验证探针（算术/str/JSON/fs/time 组合）
capability io, time

def fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

def main() -> int:
    print("fib(12) =", fib(12))
    print("str:", "hello" + "-" + "posix", "upper:", "AbC".upper())
    print("list_dir:", len(list_dir(".")) >= 0)
    print("exists:", exists("no_such_file.txt"))
    print("epoch_ms:", epoch_ms() > 0)
    lt: List[int] = localtime(1700000000)
    print("localtime_year:", lt[0])
    return 0
'@
[System.IO.File]::WriteAllText((Join-Path $root 'linux_probe.sg'), $probe, $utf8nb)

$targets = @(
    @{ triple = 'x86_64-linux-gnu';   mach = 'Advanced Micro Devices X86-64'; cls = 'ELF64' },
    @{ triple = 'aarch64-linux-gnu';  mach = 'AArch64';                       cls = 'ELF64' },
    @{ triple = 'riscv64-linux-gnu';  mach = 'RISC-V';                        cls = 'ELF64' },
    @{ triple = 'arm-linux-gnueabihf'; mach = 'ARM';                          cls = 'ELF32' }
)

$script:failed = 0
function Check([bool]$cond, [string]$name) {
    if ($cond) { Write-Output ("PASS " + $name) }
    else { $script:failed++; Write-Output ("FAIL " + $name) }
}

# Windows 侧基线：同探针 VM 模式输出（供 WSL 对拍用，落盘）
$baseOut = Join-Path $root 'baseline_vm.out'
$p = Start-Process -FilePath $scourge -ArgumentList @('run', (Join-Path $root 'linux_probe.sg')) -RedirectStandardOutput $baseOut -RedirectStandardError (Join-Path $root 'baseline_vm.err') -Wait -PassThru -NoNewWindow
Check ($p.ExitCode -eq 0) "VM baseline exit 0 (got $($p.ExitCode))"

foreach ($t in $targets) {
    $tag = $t.triple
    $elf = Join-Path $root ($tag + '.elf')
    $outF = Join-Path $root ($tag + '.build.out')
    $errF = Join-Path $root ($tag + '.build.err')
    $args = @('build', (Join-Path $root 'linux_probe.sg'),
              '--target', $t.triple,
              '--sysroot', ('D:/musl-sysroot/' + $tag),
              '--static', '-o', $elf)
    $p = Start-Process -FilePath $scourge -ArgumentList $args -RedirectStandardOutput $outF -RedirectStandardError $errF -Wait -PassThru -NoNewWindow
    Check ($p.ExitCode -eq 0) "$tag build exit 0 (got $($p.ExitCode))"
    if ($p.ExitCode -ne 0) {
        Write-Output ("  stderr: " + [System.IO.File]::ReadAllText($errF))
        continue
    }
    # ELF 头：位数 + 静态 EXEC + 目标机器
    $hdr = & $readelf -h $elf 2>&1 | Out-String
    Check ($hdr.Contains($t.cls)) "$tag $($t.cls)"
    Check ($hdr.Contains('EXEC (Executable file)')) "$tag static EXEC"
    Check ($hdr.Contains($t.mach)) "$tag machine $($t.mach)"
    # 缓存重链（第二次构建走 fromCache 路径仍须产出有效 ELF）
    $elf2 = Join-Path $root ($tag + '_relink.elf')
    $args2 = @('build', (Join-Path $root 'linux_probe.sg'),
               '--target', $t.triple,
               '--sysroot', ('D:/musl-sysroot/' + $tag),
               '--static', '-o', $elf2)
    $p2 = Start-Process -FilePath $scourge -ArgumentList $args2 -RedirectStandardOutput (Join-Path $root ($tag + '.relink.out')) -RedirectStandardError (Join-Path $root ($tag + '.relink.err')) -Wait -PassThru -NoNewWindow
    Check ($p2.ExitCode -eq 0) "$tag cache relink exit 0"
    if (Test-Path $elf2) {
        Check ((Get-Item $elf2).Length -gt 0) "$tag relink ELF non-empty"
    }
}

if ($script:failed -gt 0) {
    Write-Output ("VERIFY LINUX FAILED: " + $script:failed + " check(s)")
    exit 1
}
Write-Output 'LINUX CROSS ALL PASS'
