# Regression: fmt probes (f-string interpolation + format() desugar).
# Runnable probes -> dual-mode byte check.
# Negative check probes -> must FAIL check (parse/Sema rejection).
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
$dir = "d:\Scourge\build-rel\verify"
$cb = "d:\Scourge\verify\probes"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

# ---- Part 1: runnable probes, dual-mode byte comparison ----
$files = Get-ChildItem "$cb\probe_fmt_acc_*.sg" | ForEach-Object { $_.FullName }
foreach ($scg in $files) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($scg)
    Remove-Item "$dir\$stem.*" -Recurse -Force -ErrorAction SilentlyContinue
    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", $scg) -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $scg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    if ($p2.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err"; $fail++; continue }
    $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"
    $r1 = (Get-Content "$dir\$stem.run.out" -ErrorAction SilentlyContinue) | Where-Object { $_ -notmatch 'elapsed_ms' }
    $r3 = (Get-Content "$dir\$stem.aot.out" -ErrorAction SilentlyContinue) | Where-Object { $_ -notmatch 'elapsed_ms' }
    $o = (($r1 -join "`n") -eq ($r3 -join "`n"))
    $e = (Get-FileHash "$dir\$stem.run.err").Hash -eq (Get-FileHash "$dir\$stem.aot.err").Hash
    $x = $p1.ExitCode -eq $p3.ExitCode
    if ($o -and $e -and $x) { Write-Host "$stem IDENTICAL (exit $($p1.ExitCode))" }
    else {
        Write-Host "$stem DIFFER out=$o err=$e exit=$($p1.ExitCode)/$($p3.ExitCode)"
        $fail++
        if (-not $o) { Write-Host "--- run:"; $r1 | Select-Object -First 5; Write-Host "--- aot:"; $r3 | Select-Object -First 5 }
        if (-not $e) { Write-Host "--- run err:"; Get-Content "$dir\$stem.run.err" -First 3; Write-Host "--- aot err:"; Get-Content "$dir\$stem.aot.err" -First 3 }
    }
}

# ---- Part 2: negative probes must fail check ----
foreach ($stem in @('probe_fmt_neg_spec', 'probe_fmt_neg_fmtlit',
                    'probe_fmt_neg_count', 'probe_fmt_neg_empty',
                    'probe_fmt_neg_width', 'probe_fmt_neg_prec')) {
    $scg = "$cb\$stem.sg"
    $p = Start-Process -FilePath $exe -ArgumentList @("check", $scg) -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput "$dir\$stem.chk.out" -RedirectStandardError "$dir\$stem.chk.err"
    if ($p.ExitCode -eq 0) { Write-Host "$stem EXPECTED-FAIL-BUT-PASSED"; $fail++ }
    else { Write-Host "$stem REJECTED (exit $($p.ExitCode))" }
}

if ($fail -eq 0) { Write-Host "FMT ALL PASS" } else { Write-Host "$fail FAILED"; exit 1 }
