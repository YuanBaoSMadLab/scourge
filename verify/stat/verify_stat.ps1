# Regression: statistics builtins dual-mode byte check (0.9.7).
# mean/var/std predate this suite (0.9.4 M); median is new - all four
# locked here: tensor/list inputs, odd/even median, int promotion,
# NaN rejection (median panics, others propagate per IEEE).
# Sema negative: List[str] must be E2001 on both channels.
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
# Env note: drop lowercase proxy duplicates (case-insensitive .NET dict).
foreach ($n in @('http_proxy', 'https_proxy', 'no_proxy')) {
    $up = $n.ToUpperInvariant()
    if ([Environment]::GetEnvironmentVariable($n, 'Process') -ne $null -and
        [Environment]::GetEnvironmentVariable($up, 'Process') -ne $null) {
        [Environment]::SetEnvironmentVariable($n, $null, 'Process')
    }
}
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
$dir = "d:\Scourge\build-rel\verify\stat"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

# Cleanup helper: PS 5.1 pipeline Remove-Item intermittently NREs on
# redirected files - direct wildcard delete guarded by try/catch instead.
function Clean-Artifacts([string]$stem) {
    try { Remove-Item -Path "$dir\$stem.*" -Force -ErrorAction SilentlyContinue } catch {}
}

# --- positives: run vs build+aot triple diff (byte identical) -------------
$pos = @("probe_stat_median", "probe_stat_exist")
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

# --- sema negative: List[str] E2001 on both channels ----------------------
# Note: run stderr carries a mode-named abort trailer ("run abort: ..."),
# build stderr does not - the diagnostic block itself is the shared contract.
$ncode = "E2001"
Clean-Artifacts "probe_stat_neg"
$src = "d:\Scourge\verify\probes\probe_stat_neg.sg"
Set-Content -Path $src -Encoding Ascii -Value @('def main() -> int:', '    print(median(["a", "b"]))', '    return 0')
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $src) -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\probe_stat_neg.run.out" -RedirectStandardError "$dir\probe_stat_neg.run.err"
$p2 = Start-Process -FilePath $exe -ArgumentList @("build", $src, "-o", "$dir\probe_stat_neg.exe") -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\probe_stat_neg.build.out" -RedirectStandardError "$dir\probe_stat_neg.build.err"
$rErr = (Get-Content "$dir\probe_stat_neg.run.err" -Raw) -match $ncode
$bErr = (Get-Content "$dir\probe_stat_neg.build.err" -Raw) -match $ncode
if ($p1.ExitCode -eq 1 -and $p2.ExitCode -eq 1 -and $rErr -and $bErr) {
    Write-Host "probe_stat_neg REJECTED ($ncode both channels)"
} else {
    Write-Host "probe_stat_neg MISMATCH run=$($p1.ExitCode) build=$($p2.ExitCode) runHas=$rErr buildHas=$bErr"
    Get-Content "$dir\probe_stat_neg.run.err" -First 4
    $fail = 1
}

if ($fail -eq 0) { Write-Host "STAT PASS" } else { exit 1 }
