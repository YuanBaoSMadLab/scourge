# Regression: capability model (0.9.7) probes.
# Runnable probes -> dual-mode byte check (run vs AOT exe).
# Negative probe -> E5004 static gate on BOTH run and build paths.
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
# probe_cap writes probe_cap_tmp.txt relative to CWD: both modes are pinned
# to the same working dir so outputs are comparable; cleaned between runs.
$exe = "d:\Scourge\build-rel\scourge.exe"
$dir = "d:\Scourge\build-rel\verify_cap"
$src = "d:\Scourge\verify\cap"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

# ---- Part 1: runnable probes, dual-mode byte comparison ----
foreach ($stem in @('probe_cap', 'probe_cap_pure')) {
    $sg = "$src\$stem.sg"
    Remove-Item "$dir\$stem.*" -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item "$dir\probe_cap_tmp.txt" -Force -ErrorAction SilentlyContinue
    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", $sg) -NoNewWindow -Wait -PassThru `
          -WorkingDirectory $dir `
          -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
    Remove-Item "$dir\probe_cap_tmp.txt" -Force -ErrorAction SilentlyContinue
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -WorkingDirectory $dir `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    if ($p2.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err"; $fail++; continue }
    $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
          -WorkingDirectory $dir `
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
    Remove-Item "$dir\probe_cap_tmp.txt" -Force -ErrorAction SilentlyContinue
}

# ---- Part 2: negative probe -> E5004 on run AND build paths ----
$stem = 'neg_cap_io'
$sg = "$src\$stem.sg"
foreach ($mode in @('run', 'build')) {
    $p = Start-Process -FilePath $exe -ArgumentList @($mode, $sg) -NoNewWindow -Wait -PassThru `
         -WorkingDirectory $dir `
         -RedirectStandardOutput "$dir\$stem.$mode.out" -RedirectStandardError "$dir\$stem.$mode.err"
    $txt = Get-Content "$dir\$stem.$mode.err" -Raw -ErrorAction SilentlyContinue
    if ($p.ExitCode -eq 0) { Write-Host "$stem/$mode EXPECTED-FAIL-BUT-PASSED"; $fail++ }
    elseif ($txt -notmatch 'error\[E5004\]') { Write-Host "$stem/$mode WRONG-DIAG (no E5004)"; $fail++ }
    else { Write-Host "$stem/$mode REJECTED (exit $($p.ExitCode))" }
}

if ($fail -eq 0) { Write-Host "CAP ALL PASS" } else { Write-Host "$fail FAILED"; exit 1 }
