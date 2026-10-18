# Benchmark runner: VM vs AOT vs CPython vs C on three probes.
# ASCII-only (PS 5.1 safety). Serial build (no concurrent verification).
$ErrorActionPreference = 'Stop'
$root = 'd:\Scourge\build-rel\verify\bench'
if (Test-Path $root) { Remove-Item -Recurse -Force $root }
New-Item -ItemType Directory -Force -Path $root | Out-Null
# --- scg executable probe: PATH first (verify is Scourge), fallback repo build-rel ----
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
$cmd = Get-Command scg -ErrorAction SilentlyContinue | Select-Object -First 1
if ($cmd) {
    # Sanity check: ensure it's Scourge, not ast-grep or some other tool
    $sgOutArr = & $cmd.Source --help 2>&1
    $sgHelp = ($sgOutArr -join "`n")
    $sgExit = $LASTEXITCODE
    if ($sgExit -eq 0 -and $sgHelp -match 'scourge') {
        $exe = $cmd.Source
    }
}
if (-not $exe) { $exe = Join-Path $repoRoot 'build-rel\scg.exe' }
if (-not $exe -or -not (Test-Path $exe)) { throw "scg not found: no Scourge scg in PATH and $(Join-Path $repoRoot 'build-rel\scg.exe') does not exist" }
# -------------------------------------------------------------------------------------

foreach ($f in @('bench_fib.sg', 'bench_call.sg', 'bench_loop.sg')) {
    Copy-Item (Join-Path 'd:\Scourge\verify\bench' $f) $root
}

function Run-Cmd([string]$exe, [string[]]$argList, [string]$cwd) {
    $outF = Join-Path $root 'last.out'
    $errF = Join-Path $root 'last.err'
    if (Test-Path $outF) { Remove-Item -Force $outF }
    if (Test-Path $errF) { Remove-Item -Force $errF }
    $p = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $cwd -RedirectStandardOutput $outF -RedirectStandardError $errF -Wait -PassThru -NoNewWindow
    if ($p.ExitCode -ne 0) {
        Write-Output ("  FAILED exit=" + $p.ExitCode)
        Get-Content $errF | ForEach-Object { Write-Output ("  " + $_) }
        return $null
    }
    return [System.IO.File]::ReadAllLines($outF) -join ' | '
}

# Best-of-3 elapsed for VM and AOT (timer noise reduction).
function Best-Elapsed([string]$tag, [string[]]$exeArgs, [string]$scg) {
    $best = $null
    for ($k = 0; $k -lt 3; $k++) {
        $line = Run-Cmd $exe $exeArgs $root
        if ($null -eq $line) { return }
        $el = [double]($line -replace '.*elapsed_s=', '')
        if ($null -eq $best -or $el -lt $best) { $best = $el }
    }
    Write-Output ("{0}: {1}  best elapsed_s={2:F4}" -f $tag, $line, $best)
}

# ---- VM ----
Best-Elapsed 'VM   fib ' @('run', 'bench_fib.sg') (Join-Path $root 'bench_fib.sg')
Best-Elapsed 'VM   call' @('run', 'bench_call.sg') (Join-Path $root 'bench_call.sg')
Best-Elapsed 'VM   loop' @('run', 'bench_loop.sg') (Join-Path $root 'bench_loop.sg')

# ---- AOT ----
foreach ($b in @('bench_fib', 'bench_call', 'bench_loop')) {
    $exe = Join-Path $root ($b + '.exe')
    $p = Start-Process -FilePath $exe -ArgumentList @('build', ($b + '.sg'), '-o', ($b + '.exe')) -WorkingDirectory $root -Wait -PassThru -NoNewWindow
    if ($p.ExitCode -ne 0) { Write-Output ("AOT build FAILED for " + $b); exit 1 }
    Best-Elapsed ('AOT  ' + $b.Substring(6)) @(($b + '.exe')) $root
}

# ---- CPython ----
$py = Run-Cmd 'python' @((Join-Path 'd:\Scourge\verify\bench\bench_py.py')) $root
Write-Output ("CPython fib+call+loop (single pass, no best-of):")
Get-Content 'd:\Scourge\verify\bench\bench_py.py' | Out-Null
python d:\Scourge\verify\bench\bench_py.py

# ---- C (bench_loop only) ----
$clang = 'D:\LLVM\bin\clang.exe'
$cout = Join-Path $root 'bench_loop_c.out'
$p = Start-Process -FilePath $clang -ArgumentList @('-O2', (Join-Path 'd:\Scourge\verify\bench\bench_loop.c'), '-o', (Join-Path $root 'bench_loop_c.exe')) -Wait -PassThru -NoNewWindow
if ($p.ExitCode -ne 0) { Write-Output 'C build FAILED'; exit 1 }
$best = $null
for ($k = 0; $k -lt 3; $k++) {
    $line = Run-Cmd (Join-Path $root 'bench_loop_c.exe') @() $root
    $el = [double]($line -replace '.*elapsed_s=', '')
    if ($null -eq $best -or $el -lt $best) { $best = $el }
}
Write-Output ("C    loop : {0}  best elapsed_s={1:F4}" -f $line, $best)
