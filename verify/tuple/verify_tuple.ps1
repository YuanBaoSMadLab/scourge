# Regression: Tuple (0.9.6) dual-mode byte check.
# Probe lives in verify/tuple/probe_tuple.sg (single probe, one run).
# Error scenarios are compile-time rejections (E2001/E2004) checked below.
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
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
$dir = "d:\Scourge\build-rel\verify"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

# ---- 1. dual-mode byte check on the probe ----
$scg = "d:\Scourge\verify\tuple\probe_tuple.sg"
$stem = "tuple_probe"
Remove-Item "$dir\$stem.*" -ErrorAction SilentlyContinue
Remove-Item "$dir\$stem.sgbuild" -Recurse -Force -ErrorAction SilentlyContinue
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $scg) -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
$p2 = Start-Process -FilePath $exe -ArgumentList @("build", $scg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
if ($p2.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err"; $fail++ }
else {
    $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"
    $r1 = (Get-Content "$dir\$stem.run.out") | Where-Object { $_ -notmatch 'elapsed_ms' }
    $r3 = (Get-Content "$dir\$stem.aot.out") | Where-Object { $_ -notmatch 'elapsed_ms' }
    $o = (($r1 -join "`n") -eq ($r3 -join "`n"))
    $e = (Get-FileHash "$dir\$stem.run.err").Hash -eq (Get-FileHash "$dir\$stem.aot.err").Hash
    $x = $p1.ExitCode -eq $p3.ExitCode
    if ($o -and $e -and $x) { Write-Host "$stem IDENTICAL (exit $($p1.ExitCode))" }
    else {
        Write-Host "$stem DIFFER out=$o err=$e exit=$($p1.ExitCode)/$($p3.ExitCode)"
        $fail++
        if (-not $o) { Write-Host "--- run:"; $r1 | Select-Object -First 5; Write-Host "--- aot:"; $r3 | Select-Object -First 5 }
    }
}

# ---- 2. compile-time rejection scenarios (VM diagnostics are the single gate) ----
$cases = @(
    @{ n = "tuple_err";  m = "E2001" },  # subscript write: tuple immutable
    @{ n = "tuple_err3"; m = "E2001" }   # == with mismatched arity
)
foreach ($c in $cases) {
    $sgE = "d:\Scourge\build-rel\verify\$($c.n).sg"
    $p = Start-Process -FilePath $exe -ArgumentList @("run", $sgE) -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput "$dir\$($c.n).out" -RedirectStandardError "$dir\$($c.n).err"
    $err = Get-Content "$dir\$($c.n).err" -Raw
    if ($p.ExitCode -ne 0 -and $err -match $c.m) { Write-Host "$($c.n) REJECTED ($($c.m), exit $($p.ExitCode))" }
    else { Write-Host "$($c.n) NOT-REJECTED (exit $($p.ExitCode))"; $fail++ }
}
# index out of range reports E2004 (message text verified manually: the
# redirected stderr is not UTF-8, so Chinese message text is not matched here)
$p2r = Start-Process -FilePath $exe -ArgumentList @("run", "d:\Scourge\build-rel\verify\tuple_err2.sg") -NoNewWindow -Wait -PassThru `
       -RedirectStandardOutput "$dir\tuple_err2.out" -RedirectStandardError "$dir\tuple_err2.err"
$err2 = Get-Content "$dir\tuple_err2.err" -Raw
if ($p2r.ExitCode -ne 0 -and $err2 -match "E2004") {
    Write-Host "tuple_err2 REJECTED (E2004, exit $($p2r.ExitCode))"
} else { Write-Host "tuple_err2 NOT-REJECTED (exit $($p2r.ExitCode))"; $fail++ }

if ($fail -eq 0) { Write-Host "TUPLE ALL PASS" } else { Write-Host "$fail FAILED"; exit 1 }
