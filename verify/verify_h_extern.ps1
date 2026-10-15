# Regression: @extern dll calls (0.9.4 H) dual-mode byte check.
# Probe: codebuddytest/probe_h_extern.sg  (positive: kernel32/msvcrt)
#        codebuddytest/probe_h_extern_panic.sg (negative: missing dll)
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
# Env note: drop lowercase proxy duplicates (case-insensitive .NET dict).
foreach ($n in @('http_proxy', 'https_proxy', 'no_proxy')) {
    $up = $n.ToUpperInvariant()
    if ([Environment]::GetEnvironmentVariable($n, 'Process') -ne $null -and
        [Environment]::GetEnvironmentVariable($up, 'Process') -ne $null) {
        [Environment]::SetEnvironmentVariable($n, $null, 'Process')
    }
}
$exe = "d:\Scourge\build\scourge.exe"
$dir = "d:\Scourge\build\verify"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

foreach ($sg in @("d:\Scourge\codebuddytest\probe_h_extern.sg",
                  "d:\Scourge\codebuddytest\probe_h_extern_panic.sg")) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($sg)
    Get-ChildItem "$dir" -Filter "$stem.*" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", $sg) -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    if ($p2.ExitCode -ne 0) {
        Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err" -First 20; exit 1
    }
    $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"

    $r1 = Get-Content "$dir\$stem.run.out"
    $r3 = Get-Content "$dir\$stem.aot.out"
    $o = (($r1 -join "`n") -eq ($r3 -join "`n"))
    $e = (Get-FileHash "$dir\$stem.run.err").Hash -eq (Get-FileHash "$dir\$stem.aot.err").Hash
    $x = $p1.ExitCode -eq $p3.ExitCode
    if ($o -and $e -and $x) {
        Write-Host "$stem IDENTICAL (exit $($p1.ExitCode))"
    } else {
        Write-Host "$stem DIFFER out=$o err=$e exit=$($p1.ExitCode)/$($p3.ExitCode)"
        if (-not $o) { Write-Host "--- run:"; $r1; Write-Host "--- aot:"; $r3 }
        if (-not $e) { Write-Host "--- run err:"; Get-Content "$dir\$stem.run.err" -First 3; Write-Host "--- aot err:"; Get-Content "$dir\$stem.aot.err" -First 3 }
        $fail = 1
    }
}
if ($fail -eq 0) { Write-Host "EXTERN-H PASS" } else { exit 1 }
