# Regression: str iterability dual-mode byte check (0.9.7, defect Q15).
# Locks: for-over-str byte iteration (for ch in "abc"), reverse concat,
# empty-string zero-trip loop, str indexing (positive/negative/out-of-range
# caught via except), and the uncaught panic trailer on stderr (index OOB
# message must be byte-identical across VM and AOT).
# Sema negatives: for-over-int and float str index must be E2001 on both
# channels (static rejection, no runtime involved).
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
# Env note: drop lowercase proxy duplicates (case-insensitive .NET dict).
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
$dir = "d:\Scourge\build-rel\verify\str_for"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

# Cleanup helper: PS 5.1 pipeline Remove-Item intermittently NREs on
# redirected files - direct wildcard delete guarded by try/catch instead.
function Clean-Artifacts([string]$stem) {
    try { Remove-Item -Path "$dir\$stem.*" -Force -ErrorAction SilentlyContinue } catch {}
}

# --- positives: run vs build+aot triple diff (byte identical) -------------
$pos = @("probe_str_for", "probe_str_idx")
foreach ($n in $pos) {
    Clean-Artifacts $n

    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", "d:\Scourge\verify\probes\$n.sg") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.run.out" -RedirectStandardError "$dir\$n.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", "d:\Scourge\verify\probes\$n.sg", "-o", "$dir\$n.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.build.out" -RedirectStandardError "$dir\$n.build.err"
    if ($p2.ExitCode -ne 0) {
        Write-Host "$n BUILD-FAIL"; Get-Content "$dir\$n.build.err" -First 20; $fail = 1; continue
    }
    $p3 = Start-Process -FilePath "$dir\$n.exe" -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.aot.out" -RedirectStandardError "$dir\$n.aot.err"

    $r1 = Get-Content "$dir\$n.run.out"
    $r3 = Get-Content "$dir\$n.aot.out"
    $o = (($r1 -join "`n") -eq ($r3 -join "`n"))
    $e = (Get-FileHash "$dir\$n.run.err").Hash -eq (Get-FileHash "$dir\$n.aot.err").Hash
    $x = $p1.ExitCode -eq $p3.ExitCode
    if ($o -and $e -and $x) {
        Write-Host "$n IDENTICAL (exit $($p1.ExitCode))"
    } else {
        Write-Host "$n DIFFER out=$o err=$e exit=$($p1.ExitCode)/$($p3.ExitCode)"
        if (-not $o) { Write-Host "--- run:"; $r1; Write-Host "--- aot:"; $r3 }
        if (-not $e) { Write-Host "--- run err:"; Get-Content "$dir\$n.run.err" -First 3; Write-Host "--- aot err:"; Get-Content "$dir\$n.aot.err" -First 3 }
        $fail = 1
    }
}

# --- uncaught panic: index OOB trailer must be byte-identical -------------
# No try/except here: the panic text lands on stderr and the two modes'
# stderr hashes must match (message is the dual-mode contract).
Clean-Artifacts "probe_str_panic"
$src = "d:\Scourge\verify\probes\probe_str_panic.sg"
Set-Content -Path $src -Encoding Ascii -Value @('def main() -> int:', '    s = "abc"', '    print(s[5])', '    return 0')
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $src) -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\probe_str_panic.run.out" -RedirectStandardError "$dir\probe_str_panic.run.err"
$p2 = Start-Process -FilePath $exe -ArgumentList @("build", $src, "-o", "$dir\probe_str_panic.exe") -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\probe_str_panic.build.out" -RedirectStandardError "$dir\probe_str_panic.build.err"
$p3 = Start-Process -FilePath "$dir\probe_str_panic.exe" -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\probe_str_panic.aot.out" -RedirectStandardError "$dir\probe_str_panic.aot.err"
$e1 = Get-Content "$dir\probe_str_panic.run.err" -Raw
$e3 = Get-Content "$dir\probe_str_panic.aot.err" -Raw
# Exact stderr equality (case-sensitive) + non-zero exit on both modes:
# the panic trailer carries source path/line, so full-text match is the
# strongest form of the dual-mode message contract.
$panicOk = $p1.ExitCode -eq $p3.ExitCode -and $p1.ExitCode -ne 0 -and ($e1 -ceq $e3) -and $e1.StartsWith("panic:")
if ($panicOk) {
    Write-Host "probe_str_panic PANIC-IDENTICAL (exit $($p1.ExitCode))"
} else {
    Write-Host "probe_str_panic MISMATCH run=$($p1.ExitCode) aot=$($p3.ExitCode)"
    Write-Host "--- run err:"; $e1
    Write-Host "--- aot err:"; $e3
    $fail = 1
}

# --- sema negatives: static rejection on both channels --------------------
# Note: run stderr carries a mode-named abort trailer ("run abort: ..."),
# build stderr does not - the diagnostic block itself is the shared contract.
$ncode = "E2001"
$negs = @(
    @{ name = "neg_for_int";  lines = @('def main() -> int:', '    for x in 123:', '        print(x)', '    return 0') },
    @{ name = "neg_idx_flt";  lines = @('def main() -> int:', '    s = "abc"', '    print(s[1.5])', '    return 0') }
)
foreach ($g in $negs) {
    $n = $g.name
    Clean-Artifacts $n
    $src = "d:\Scourge\verify\probes\$n.sg"
    Set-Content -Path $src -Encoding Ascii -Value $g.lines
    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", $src) -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.run.out" -RedirectStandardError "$dir\$n.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $src, "-o", "$dir\$n.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.build.out" -RedirectStandardError "$dir\$n.build.err"
    $rErr = (Get-Content "$dir\$n.run.err" -Raw) -match $ncode
    $bErr = (Get-Content "$dir\$n.build.err" -Raw) -match $ncode
    if ($p1.ExitCode -eq 1 -and $p2.ExitCode -eq 1 -and $rErr -and $bErr) {
        Write-Host "$n REJECTED ($ncode both channels)"
    } else {
        Write-Host "$n MISMATCH run=$($p1.ExitCode) build=$($p2.ExitCode) runHas=$rErr buildHas=$bErr"
        Get-Content "$dir\$n.run.err" -First 4
        $fail = 1
    }
}

if ($fail -eq 0) { Write-Host "STRFOR PASS" } else { exit 1 }
