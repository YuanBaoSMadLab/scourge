# Parameterized dual-mode byte-identical verification (ASCII only)
# Probes (source assets) live in d:\Scourge\verify (git-tracked);
# artifacts go to d:\Scourge\build\verify (git-ignored).
$Files = @("p_try_min.sg","p_try_cross.sg","p_try_nested.sg","p_try_uncaught.sg","p_result.sg","p_list.sg")
# Env note: drop lowercase proxy duplicates (case-insensitive .NET dict).
foreach ($n in @('http_proxy', 'https_proxy', 'no_proxy')) {
    $up = $n.ToUpperInvariant()
    if ([Environment]::GetEnvironmentVariable($n, 'Process') -ne $null -and
        [Environment]::GetEnvironmentVariable($up, 'Process') -ne $null) {
        [Environment]::SetEnvironmentVariable($n, $null, 'Process')
    }
}
$exe = "d:\Scourge\build\scourge.exe"
$src = "d:\Scourge\verify"
$dir = "d:\Scourge\build\verify"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0
foreach ($f in $Files) {
    $sg = Join-Path $src $f
    $stem = [IO.Path]::GetFileNameWithoutExtension($f)
    Remove-Item "$dir\$stem.run.out","$dir\$stem.run.err","$dir\$stem.aot.out","$dir\$stem.aot.err","$dir\$stem.build.out","$dir\$stem.build.err","$dir\$stem.exe" -ErrorAction SilentlyContinue
    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", $sg) -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    if ($p2.ExitCode -ne 0) {
        Write-Host "$stem BUILD-FAIL (run exit $($p1.ExitCode))"
        Get-Content "$dir\$stem.build.err"
        $fail++
        continue
    }
    $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"
    $o = (Get-FileHash "$dir\$stem.run.out").Hash -eq (Get-FileHash "$dir\$stem.aot.out").Hash
    $e = (Get-FileHash "$dir\$stem.run.err").Hash -eq (Get-FileHash "$dir\$stem.aot.err").Hash
    $x = $p1.ExitCode -eq $p3.ExitCode
    if ($o -and $e -and $x) {
        Write-Host "$stem IDENTICAL (exit $($p1.ExitCode))"
    } else {
        Write-Host "$stem DIFFER out=$o err=$e exit=$($p1.ExitCode)/$($p3.ExitCode)"
        $fail++
        Write-Host "--- run stdout:"; Get-Content "$dir\$stem.run.out"
        Write-Host "--- aot stdout:"; Get-Content "$dir\$stem.aot.out"
        Write-Host "--- run stderr:"; Get-Content "$dir\$stem.run.err"
        Write-Host "--- aot stderr:"; Get-Content "$dir\$stem.aot.err"
    }
}
if ($fail -gt 0) { exit 1 } else { Write-Host "ALL PASS" }
