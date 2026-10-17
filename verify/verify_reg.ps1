# Regression: train_linreg / attention_softmax dual-mode byte check (ASCII only)
# Plus: ovf_* scalar overflow probes — inline-lowered AOT (llvm.*.with.overflow)
# must produce byte-identical panics to the VM (add/sub/mul/neg + try-catch).
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
New-Item -ItemType Directory -Force $dir | Out-Null
$files = @("d:\Scourge\release\examples\train_linreg.sg","d:\Scourge\release\examples\attention_softmax.sg") + (Get-ChildItem "d:\Scourge\verify\ovf_*.sg" | ForEach-Object { $_.FullName }) + (Get-ChildItem "d:\Scourge\verify\dvz_*.sg" | ForEach-Object { $_.FullName })
$fail = 0
foreach ($sg in $files) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($sg)
    Remove-Item "$dir\$stem.run.out","$dir\$stem.run.err","$dir\$stem.aot.out","$dir\$stem.aot.err","$dir\$stem.exe","$dir\$stem.build.out","$dir\$stem.build.err" -ErrorAction SilentlyContinue
    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", $sg) -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    if ($p2.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err"; $fail++; continue }
    $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"
    # strip elapsed_ms lines from run/aot stdout before compare
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
        if (-not $e) { Write-Host "--- run err:"; Get-Content "$dir\$stem.run.err" -First 3; Write-Host "--- aot err:"; Get-Content "$dir\$stem.aot.err" -First 3 }
    }
}
if ($fail -gt 0) { exit 1 } else { Write-Host "ALL PASS" }
