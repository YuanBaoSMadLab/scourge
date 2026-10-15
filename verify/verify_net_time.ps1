# Regression: net_* / sleep builtins (0.9.4 F/G) dual-mode byte check.
# Probes live in codebuddytest/ (probe_f_* / probe_g_sleep).
# Needs local test servers: python net_test_server.py must be running
# (TCP echo 23456, fixed HTTP 23458, immediate-close 23460).
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
# Env note: if proxy vars coexist in different cases (http_proxy/HTTP_PROXY
# etc), Start-Process fails on a case-insensitive .NET env dictionary; drop
# lowercase duplicates from the process block.
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

# Start helper servers if the echo port is not listening.
$up = $true
try {
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect("127.0.0.1", 23456)
    $c.Close()
} catch { $up = $false }
$srvProc = $null
if (-not $up) {
    $srvProc = Start-Process -FilePath "python" `
        -ArgumentList @("d:\Scourge\codebuddytest\net_test_server.py") `
        -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 1200
}

$files = (Get-ChildItem "d:\Scourge\codebuddytest\probe_f_*.sg" | ForEach-Object { $_.FullName }) +
         (Get-ChildItem "d:\Scourge\codebuddytest\probe_g_sleep.sg" | ForEach-Object { $_.FullName })
# probe_f_server needs a python client mid-run; handled after the loop.
$files = $files | Where-Object { $_ -notmatch 'probe_f_server\.sg$' }
$fail = 0
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
        if (-not $o) { Write-Host "--- run:"; $r1 | Select-Object -First 5; Write-Host "--- aot:"; $r3 | Select-Object -First 5 }
        if (-not $e) { Write-Host "--- run err:"; Get-Content "$dir\$stem.run.err" -First 3; Write-Host "--- aot err:"; Get-Content "$dir\$stem.aot.err" -First 3 }
    }
}
if ($srvProc -ne $null) { Stop-Process -Id $srvProc.Id -Force -ErrorAction SilentlyContinue }

# ---- probe_f_server: dual-mode with python client mid-run ----
$sg = "d:\Scourge\codebuddytest\probe_f_server.sg"
$stem = "probe_f_server"
$client = "d:\Scourge\codebuddytest\net_test_client.py"
$srvFail = 0
$runs = @(
    @{ exe = $exe; arg = @("run", $sg); out = "$dir\$stem.run.out" },
    @{ exe = "$dir\$stem.exe"; arg = @(); out = "$dir\$stem.aot.out" }
)
# Build AOT exe first (build output must not race the server run).
Remove-Item "$dir\$stem.exe" -ErrorAction SilentlyContinue
$p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
if ($p2.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; $fail++ }
else {
    # Run 1: VM (scourge run), Run 2: AOT exe directly.
    $p = Start-Process -FilePath $exe -ArgumentList @("run", $sg) -NoNewWindow -PassThru `
         -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
    Start-Sleep -Milliseconds 900
    $null = Start-Process -FilePath "python" -ArgumentList @($client) -NoNewWindow -Wait -PassThru `
            -RedirectStandardOutput "$dir\$stem.client.out" -RedirectStandardError "$dir\$stem.client.err"
    $p.WaitForExit()
    $p = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -PassThru `
         -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"
    Start-Sleep -Milliseconds 900
    $null = Start-Process -FilePath "python" -ArgumentList @($client) -NoNewWindow -Wait -PassThru `
            -RedirectStandardOutput "$dir\$stem.client.out" -RedirectStandardError "$dir\$stem.client.err"
    $p.WaitForExit()
    $o = ((Get-Content "$dir\$stem.run.out" -ErrorAction SilentlyContinue) -join "`n") -eq
         ((Get-Content "$dir\$stem.aot.out" -ErrorAction SilentlyContinue) -join "`n")
    $e = (Get-FileHash "$dir\$stem.run.err").Hash -eq (Get-FileHash "$dir\$stem.aot.err").Hash
    if ($o -and $e) { Write-Host "$stem IDENTICAL" }
    else {
        Write-Host "$stem DIFFER out=$o err=$e"
        Write-Host "--- run:"; Get-Content "$dir\$stem.run.out" -ErrorAction SilentlyContinue | Select-Object -First 5
        Write-Host "--- aot:"; Get-Content "$dir\$stem.aot.out" -ErrorAction SilentlyContinue | Select-Object -First 5
        $srvFail = 1
    }
}
$fail += $srvFail

if ($fail -eq 0) { Write-Host "NET/TIME ALL PASS" } else { Write-Host "$fail FAILED"; exit 1 }
