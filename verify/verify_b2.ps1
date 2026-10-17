# Regression: 0.9.9 batch-2 (slice / f-string spec / ffi-flush ordering /
# --target= one-click cross / diagnostic prescriptions) dual-mode byte check.
# Probes: verify/probes/probe_099_*.sg (re-runs batch-1 probes as superset).
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
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
$fail = 0

# ---- Part 1: dual-mode byte check on all 099 probes (b1 superset) ----
$files = Get-ChildItem "d:\Scourge\verify\probes\probe_099_*.sg" |
         ForEach-Object { $_.FullName }
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
    $r1 = (Get-Content "$dir\$stem.run.out") | Where-Object { $_ -notmatch 'elapsed_ms' }
    $r3 = (Get-Content "$dir\$stem.aot.out") | Where-Object { $_ -notmatch 'elapsed_ms' }
    $o = (($r1 -join "`n") -eq ($r3 -join "`n"))
    $e = (Get-FileHash "$dir\$stem.run.err").Hash -eq (Get-FileHash "$dir\$stem.aot.err").Hash
    $x = $p1.ExitCode -eq $p3.ExitCode
    if ($o -and $e -and $x) { Write-Host "$stem IDENTICAL (exit $($p1.ExitCode))" }
    else {
        Write-Host "$stem DIFFER out=$o err=$e exit=$($p1.ExitCode)/$($p3.ExitCode)"
        $fail++
        if (-not $o) { Write-Host "--- run:"; $r1 | Select-Object -First 8; Write-Host "--- aot:"; $r3 | Select-Object -First 8 }
        if (-not $e) { Write-Host "--- run err:"; Get-Content "$dir\$stem.run.err" -First 3; Write-Host "--- aot err:"; Get-Content "$dir\$stem.aot.err" -First 3 }
    }
}

# ---- Part 2: CLI one-click cross prescription (bogus triple, no sysroot) ----
Remove-Item "$dir\cli_*" -Force -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -ArgumentList @("build", "--target=x86_64-verify-b2-bogus", "d:\Scourge\verify\probes\probe_099_conv.sg") `
     -NoNewWindow -Wait -PassThru -RedirectStandardOutput "$dir\cli_eq.out" -RedirectStandardError "$dir\cli_eq.err"
$eqText = (Get-Content "$dir\cli_eq.err" -Raw) + (Get-Content "$dir\cli_eq.out" -Raw)
$eqOk = ($p.ExitCode -eq 1) -and ($eqText -match 'build_musl_sysroot')
$p = Start-Process -FilePath $exe -ArgumentList @("build", "--target", "x86_64-verify-b2-bogus", "d:\Scourge\verify\probes\probe_099_conv.sg") `
     -NoNewWindow -Wait -PassThru -RedirectStandardOutput "$dir\cli_sp.out" -RedirectStandardError "$dir\cli_sp.err"
$spText = (Get-Content "$dir\cli_sp.err" -Raw) + (Get-Content "$dir\cli_sp.out" -Raw)
$spOk = ($p.ExitCode -eq 1) -and ($spText -match 'build_musl_sysroot')
if ($eqOk -and $spOk) { Write-Host "cli-cross-prescription IDENTICAL (exit 1)" }
else { Write-Host "cli-cross-prescription FAIL eq=$eqOk sp=$spOk"; $fail++ }

# ---- Part 3: diagnostic prescriptions (compile-fail message checks) ----
$neg = @(
    @{ name = "neg_toplevel";    code = 'print("hi")';        pat = 'def main' },
    @{ name = "neg_dict_mixed";  code = 'd = {"a": 1, "b": "x"}'; pat = 'Dict' },
    @{ name = "neg_fmt_spec";    code = 'def main() -> int:`n    print(f"{7:.2d}")`n    return 0'; pat = '\.2d' },
    @{ name = "neg_slice_dict";  code = 'd = {"a": 1}`ndef main() -> int:`n    print(d[0:1])`n    return 0'; pat = 's\[a:b\]' }
)
foreach ($t in $neg) {
    $sg = "$dir\$($t.name).sg"
    $code = $t.code -replace '`n', "`n"
    [IO.File]::WriteAllText($sg, $code)
    $p = Start-Process -FilePath $exe -ArgumentList @("check", $sg) -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput "$dir\$($t.name).out" -RedirectStandardError "$dir\$($t.name).err"
    $txt = (Get-Content "$dir\$($t.name).err" -Raw) + (Get-Content "$dir\$($t.name).out" -Raw)
    if ($p.ExitCode -ne 0 -and $txt -match $t.pat) { Write-Host "$($t.name) prescription OK" }
    else { Write-Host "$($t.name) FAIL (exit $($p.ExitCode))"; $fail++ }
}

if ($fail -eq 0) { Write-Host "B2 ALL PASS" } else { Write-Host "$fail FAILED"; exit 1 }
