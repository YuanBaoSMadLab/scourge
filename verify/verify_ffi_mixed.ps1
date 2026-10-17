# Regression: @extern mixed-signature fixed args (MS x64 positional register
# assignment) dual-mode byte check.
# Probe: verify/probes/probe_ffi_mixed.sg + probe_ffi_mixed.c (callee DLL
# built here with clang; env SCOURGE_CLANG overrides the compiler path).
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
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0
$stem = "probe_ffi_mixed"

# ---- build callee DLL (positional-ABI reference callee) ----
Get-ChildItem "$dir" -Filter "$stem.*" -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force -Confirm:$false -ErrorAction SilentlyContinue
$clang = $env:SCOURGE_CLANG
if (-not $clang) { $clang = "D:\LLVM\bin\clang.exe" }
if (-not (Test-Path $clang)) { $clang = "clang" }
$cl = Start-Process -FilePath $clang `
      -ArgumentList @("-O2", "-shared", "d:\Scourge\verify\probes\probe_ffi_mixed.c",
                      "-o", "$dir\$stem.dll") -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.clang.out" -RedirectStandardError "$dir\$stem.clang.err"
if ($cl.ExitCode -ne 0 -or -not (Test-Path "$dir\$stem.dll")) {
    Write-Host "$stem DLL-BUILD-FAIL"; Get-Content "$dir\$stem.clang.err" -First 20; exit 1
}

# ---- dual-mode byte check (CWD = $dir so the bare DLL name resolves) ----
Push-Location $dir
try {
    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", "d:\Scourge\verify\probes\$stem.sg") `
          -NoNewWindow -Wait -PassThru -RedirectStandardOutput "$dir\$stem.run.out" `
          -RedirectStandardError "$dir\$stem.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", "d:\Scourge\verify\probes\$stem.sg",
          "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
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
        Write-Host "$stem DIFF (out:$o err:$e exit:$x)"
        Get-Content "$dir\$stem.run.out","$dir\$stem.aot.out","$dir\$stem.run.err","$dir\$stem.aot.err" -ErrorAction SilentlyContinue
        $fail++
    }

    # content assertions: positional placement must deliver every argument
    $pos = Get-Content "$dir\$stem.run.out" -Raw
    if ($pos -match "8\.25" -and $pos -match "321\.0" -and $pos -match "21\.0" -and $pos -match "13\.0") {
        Write-Host "$stem CONTENT PASS"
    } else { Write-Host "$stem CONTENT FAIL"; $fail++ }
} finally {
    Pop-Location
}

if ($fail -gt 0) { Write-Host "FFI-MIXED FAILED: $fail"; exit 1 }
Write-Host "FFI-MIXED ALL PASS"
