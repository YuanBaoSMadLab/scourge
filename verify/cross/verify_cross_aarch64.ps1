# Regression: aarch64-linux-gnu cross build, artifact-level (0.9.8).
# Host has no aarch64 execution path, so semantic truth comes from the
# host dual-mode (VM run + native build/run, must be byte identical);
# the cross artifact is validated structurally: ELF64/EM_AARCH64/EXEC,
# static (no needed libs), runtime+main symbols, AArch64 disasm probe.
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
foreach ($n in @('http_proxy', 'https_proxy', 'no_proxy')) {
    $up = $n.ToUpperInvariant()
    if ([Environment]::GetEnvironmentVariable($n, 'Process') -ne $null -and
        [Environment]::GetEnvironmentVariable($up, 'Process') -ne $null) {
        [Environment]::SetEnvironmentVariable($n, $null, 'Process')
    }
}
# --- sg executable probe: PATH first (verify is Scourge), fallback repo build-rel ----
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = $null
try { $repoRoot = git -C $here rev-parse --show-toplevel 2>$null } catch {}
if (-not $repoRoot) {
    # Walk up to find CMakeLists.txt as fallback
    $p = $here
    while ($p -and -not (Test-Path (Join-Path $p 'CMakeLists.txt'))) {
        $parent = Split-Path $p -Parent
        if (-not $parent -or $parent -eq $p) { break }
        $p = $parent
    }
    $repoRoot = $p
}
$exe = $null
$cmd = Get-Command sg -ErrorAction SilentlyContinue | Select-Object -First 1
if ($cmd) {
    # Sanity check: ensure it's Scourge, not ast-grep or some other tool
    $sgOutArr = & $cmd.Source --help 2>&1
    $sgHelp = ($sgOutArr -join "`n")
    $sgExit = $LASTEXITCODE
    if ($sgExit -eq 0 -and $sgHelp -match 'scourge') {
        $exe = $cmd.Source
    }
}
if (-not $exe) { $exe = Join-Path $repoRoot 'build-rel\sg.exe' }
if (-not $exe -or -not (Test-Path $exe)) { throw "sg not found: no Scourge sg in PATH and $(Join-Path $repoRoot 'build-rel\sg.exe') does not exist" }
# -------------------------------------------------------------------------------------
$llvm = "D:\LLVM\bin"
$probe = "d:\Scourge\verify\probes\probe_cross_aarch64.sg"
$dir = "d:\Scourge\build-rel\verify\cross"
$elf = "$dir\probe_cross_aarch64.elf"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

# --- 1. host VM run = expected 7 lines (semantic reference) ---------------
$p1 = Start-Process -FilePath $exe -ArgumentList @('run', $probe) -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\vm.out" -RedirectStandardError "$dir\vm.err"
$expect = @('cross', '3', '3.5', '5 9', 'abc', '012', '2 True 2')
$got = Get-Content "$dir\vm.out"
if ($p1.ExitCode -eq 0 -and (($got -join "`n") -eq ($expect -join "`n"))) {
    Write-Host "host-vm EXPECTED"
} else {
    Write-Host "host-vm MISMATCH exit=$($p1.ExitCode)"; $got; $fail++
}

# --- 2. host AOT build+run byte-identical to VM (dual-mode) ---------------
$p2 = Start-Process -FilePath $exe -ArgumentList @('build', $probe, '-o', "$dir\probe_host.exe") -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\hb.out" -RedirectStandardError "$dir\hb.err"
if ($p2.ExitCode -ne 0) { Write-Host "host-build FAIL"; Get-Content "$dir\hb.err" -First 10; $fail++ }
else {
    $p3 = Start-Process -FilePath "$dir\probe_host.exe" -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\host.out" -RedirectStandardError "$dir\host.err"
    $h = Get-Content "$dir\host.out"
    if ($p3.ExitCode -eq 0 -and (($h -join "`n") -eq ($expect -join "`n"))) {
        Write-Host "host-aot IDENTICAL"
    } else {
        Write-Host "host-aot DIFFER exit=$($p3.ExitCode)"; $h; $fail++
    }
}

# --- 3. cross build: aarch64-linux-gnu musl static -------------------------
$p4 = Start-Process -FilePath $exe -ArgumentList @('build', $probe, '-o', $elf, '--target', 'aarch64-linux-gnu', '--sysroot', 'D:/musl-sysroot/aarch64-linux-gnu', '--static') -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\xb.out" -RedirectStandardError "$dir\xb.err"
if ($p4.ExitCode -ne 0) {
    Write-Host "cross-build FAIL"; Get-Content "$dir\xb.err" -First 20; $fail++
} else {
    Write-Host "cross-build OK"

    # 4a. ELF header: 64-bit EM_AARCH64 EXEC
    $hdr = (& "$llvm\llvm-readobj.exe" --file-headers $elf) -join "`n"
    $okHdr = ($hdr -match 'Class: 64-bit') -and ($hdr -match 'Machine: EM_AARCH64') -and
             ($hdr -match 'Type: Executable')
    # 4b. static: NeededLibraries list empty (no .so anywhere)
    $libs = (& "$llvm\llvm-readobj.exe" --needed-libs $elf) -join "`n"
    $okStatic = ($libs -match 'NeededLibraries') -and (-not ($libs -match '\.so'))
    # 4c. symbols: main + core runtime present
    $syms = (& "$llvm\llvm-nm.exe" $elf) -join "`n"
    $okSyms = ($syms -match ' T main') -and ($syms -match ' T sg_print_i64') -and
              ($syms -match ' T sg_panic')
    # 4d. disasm: AArch64-only features in the Scourge main body window
    #     (ELF main is a C shim calling sg_<stem>__main; adrp = PC-relative
    #     page addressing, x-register ops, bl into sg_*)
    $dis = (& "$llvm\llvm-objdump.exe" -d $elf) -join "`n"
    $idx = $dis.IndexOf('<sg_probe_cross_aarch64__main>:')
    $mainAsm = if ($idx -ge 0) { $dis.Substring($idx, [Math]::Min(1500, $dis.Length - $idx)) } else { "" }
    $okAsm = ($mainAsm -match 'adrp') -and ($mainAsm -match 'bl\s+0x[0-9a-f]+ <sg_print') -and
             ($mainAsm -match '\bx[0-9]') -and (-not ($mainAsm -match '\b(call|movl|lea)\b'))

    if ($okHdr -and $okStatic -and $okSyms -and $okAsm) {
        Write-Host "cross-artifact AARCH64-STATIC-OK"
    } else {
        Write-Host "cross-artifact CHECK hdr=$okHdr static=$okStatic syms=$okSyms asm=$okAsm"
        if (-not $okHdr)  { $hdr  | Select-Object -First 8 }
        if (-not $okStatic) { $libs | Select-Object -First 5 }
        if (-not $okSyms)  { $syms | Select-String 'main|sg_print' | Select-Object -First 5 }
        if (-not $okAsm)   { $mainAsm | Select-Object -First 6 }
        $fail++
    }
}

if ($fail -eq 0) { Write-Host "CROSS-AARCH64 PASS" } else { exit 1 }
