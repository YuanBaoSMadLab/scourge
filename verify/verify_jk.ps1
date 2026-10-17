# Regression: J (error handling) / K (@pure purity enforcement) probes.
# Runnable probes (probe_j_*, probe_k_pure_ok) -> dual-mode byte check.
# Negative check probes (neg_jk_*) -> must FAIL check with E1007.
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
$dir = "d:\Scourge\build-rel\verify"
$cb = "d:\Scourge\verify\probes"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

# ---- Part 1: runnable probes, dual-mode byte comparison ----
$files = (Get-ChildItem "$cb\probe_j_*.sg" | ForEach-Object { $_.FullName }) +
         (Get-ChildItem "$cb\probe_k_pure_ok.sg" | ForEach-Object { $_.FullName })
foreach ($sg in $files) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($sg)
    Remove-Item "$dir\$stem.*" -Recurse -Force -ErrorAction SilentlyContinue
    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", $sg) -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
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

# ---- Part 2: negative probes must fail check with E1007 ----
foreach ($stem in @('probe_k_pure_print', 'probe_k_pure_call', 'probe_k_pure_net',
                    'probe_k_unknown')) {
    $sg = "$cb\$stem.sg"
    $p = Start-Process -FilePath $exe -ArgumentList @("check", $sg) -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput "$dir\$stem.chk.out" -RedirectStandardError "$dir\$stem.chk.err"
    $txt = (Get-Content "$dir\$stem.chk.out" -Raw) + (Get-Content "$dir\$stem.chk.err" -Raw)
    if ($p.ExitCode -eq 0) { Write-Host "$stem EXPECTED-FAIL-BUT-PASSED"; $fail++ }
    elseif ($txt -notmatch 'E1007') { Write-Host "$stem WRONG-CODE (expected E1007)"; $fail++ }
    else { Write-Host "$stem REJECTED (E1007, exit $($p.ExitCode))" }
}

if ($fail -eq 0) { Write-Host "J/K ALL PASS" } else { Write-Host "$fail FAILED"; exit 1 }
