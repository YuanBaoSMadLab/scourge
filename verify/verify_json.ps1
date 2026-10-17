# Regression: JSON (json_decode + Json value methods) probes.
# Runnable probes -> dual-mode byte check (run vs AOT build exe).
# Negative check probes -> must FAIL check (static type rejection).
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
# Env note: drop lowercase proxy duplicates (case-insensitive .NET dict).
foreach ($n in @('http_proxy', 'https_proxy', 'no_proxy')) {
    $up = $n.ToUpperInvariant()
    if ([Environment]::GetEnvironmentVariable($n, 'Process') -ne $null -and
        [Environment]::GetEnvironmentVariable($up, 'Process') -ne $null) {
        [Environment]::SetEnvironmentVariable($n, $null, 'Process')
    }
}
$exe = "d:\Scourge\build-rel\scourge.exe"
$dir = "d:\Scourge\build-rel\verify"
$cb = "d:\Scourge\verify\probes"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

# ---- Part 1: runnable probes, dual-mode byte comparison ----
$files = Get-ChildItem "$cb\probe_json_*.sg" | ForEach-Object { $_.FullName }
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
    $r1 = (Get-Content "$dir\$stem.run.out" -ErrorAction SilentlyContinue)
    $r3 = (Get-Content "$dir\$stem.aot.out" -ErrorAction SilentlyContinue)
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
foreach ($stem in @('neg_json_type')) {
    $sg = "$cb\$stem.sg"
    $p = Start-Process -FilePath $exe -ArgumentList @("check", $sg) -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput "$dir\$stem.chk.out" -RedirectStandardError "$dir\$stem.chk.err"
    if ($p.ExitCode -eq 0) { Write-Host "$stem EXPECTED-FAIL-BUT-PASSED"; $fail++ }
    else { Write-Host "$stem REJECTED (exit $($p.ExitCode))" }
}

if ($fail -eq 0) { Write-Host "JSON ALL PASS" } else { Write-Host "$fail FAILED"; exit 1 }
