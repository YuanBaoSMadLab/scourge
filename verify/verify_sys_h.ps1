# Regression: sys builtins (0.9.4 H) dual-mode byte check.
# Probe: codebuddytest/probe_h_sys.sg
# Passes fixed script args [x, --flag, y] + env PROBE_H_VAR to both modes
# so args()/env() are deterministic; time uses fixed epoch inputs.
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
$exe = "d:\Scourge\build-rel\scourge.exe"
$dir = "d:\Scourge\build-rel\verify"
New-Item -ItemType Directory -Force $dir | Out-Null
[Environment]::SetEnvironmentVariable("PROBE_H_VAR", "probe-value-123", "Process")

$sg = "d:\Scourge\codebuddytest\probe_h_sys.sg"
$stem = [IO.Path]::GetFileNameWithoutExtension($sg)
Remove-Item "$dir\$stem.*" -ErrorAction SilentlyContinue

$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $sg, "x", "--flag", "y") -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
$p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
if ($p2.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err"; exit 1 }
$p3 = Start-Process -FilePath "$dir\$stem.exe" -ArgumentList @("x", "--flag", "y") -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"

$r1 = Get-Content "$dir\$stem.run.out"
$r3 = Get-Content "$dir\$stem.aot.out"
$o = (($r1 -join "`n") -eq ($r3 -join "`n"))
$e = (Get-FileHash "$dir\$stem.run.err").Hash -eq (Get-FileHash "$dir\$stem.aot.err").Hash
$x = $p1.ExitCode -eq $p3.ExitCode
if ($o -and $e -and $x) {
    Write-Host "$stem IDENTICAL (exit $($p1.ExitCode))"
    Write-Host "SYS-H PASS"
} else {
    Write-Host "$stem DIFFER out=$o err=$e exit=$($p1.ExitCode)/$($p3.ExitCode)"
    if (-not $o) { Write-Host "--- run:"; $r1; Write-Host "--- aot:"; $r3 }
    if (-not $e) { Write-Host "--- run err:"; Get-Content "$dir\$stem.run.err" -First 3; Write-Host "--- aot err:"; Get-Content "$dir\$stem.aot.err" -First 3 }
    exit 1
}
