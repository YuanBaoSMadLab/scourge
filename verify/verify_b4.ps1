# Regression: 0.9.10 batch-4 (F-15 module-level literal globals /
# net_http_get_json framing parse) dual-mode byte check.
# Probes: verify/probes/probe_099_*.sg (superset incl. batch-1/2/3 probes)
#         + probe_f_http_json*.sg (server-dependent, like probe_f_* family).
# Needs local test servers: python net_test_server.py must be running
# (TCP echo 23456, fixed HTTP 23458, immediate-close 23460,
#  chunked HTTP 23462). Auto-started here when the echo port is silent.
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
foreach ($n in @('http_proxy', 'https_proxy', 'no_proxy')) {
    $up = $n.ToUpperInvariant()
    if ([Environment]::GetEnvironmentVariable($n, 'Process') -ne $null -and
        [Environment]::GetEnvironmentVariable($up, 'Process') -ne $null) {
        [Environment]::SetEnvironmentVariable($n, $null, 'Process')
    }
}
# --- scg executable probe: PATH first (verify is Scourge), fallback repo build-rel ----
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
$cmd = Get-Command scg -ErrorAction SilentlyContinue | Select-Object -First 1
if ($cmd) {
    # Sanity check: ensure it's Scourge, not ast-grep or some other tool
    $sgOutArr = & $cmd.Source --help 2>&1
    $sgHelp = ($sgOutArr -join "`n")
    $sgExit = $LASTEXITCODE
    if ($sgExit -eq 0 -and $sgHelp -match 'scourge') {
        $exe = $cmd.Source
    }
}
if (-not $exe) { $exe = Join-Path $repoRoot 'build-rel\scg.exe' }
if (-not $exe -or -not (Test-Path $exe)) { throw "scg not found: no Scourge scg in PATH and $(Join-Path $repoRoot 'build-rel\scg.exe') does not exist" }
# -------------------------------------------------------------------------------------
$dir = "d:\Scourge\build-rel\verify"
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
        -ArgumentList @("d:\Scourge\verify\probes\net_test_server.py") `
        -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 1200
}
$fail = 0

# ---- Part 1: dual-mode byte check (099 superset + http_json probes) ----
$files = (Get-ChildItem "d:\Scourge\verify\probes\probe_099_*.sg" |
          ForEach-Object { $_.FullName }) +
         (Get-ChildItem "d:\Scourge\verify\probes\probe_f_http_json*.sg" |
          ForEach-Object { $_.FullName })
foreach ($scg in $files) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($scg)
    Remove-Item "$dir\$stem.*" -Recurse -Force -ErrorAction SilentlyContinue
    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", $scg) -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $scg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
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

# ---- Part 2: panic message prescription (positive check on the
# immediate-close probe; message text locked verbatim dual-mode) ----
$stem = "probe_f_http_json_panic"
$errTxt = Get-Content "$dir\$stem.run.err" -Raw -ErrorAction SilentlyContinue
if ($errTxt -match 'net_http_get_json\(\) .+?') { Write-Host "http_json panic msg present" }
else { Write-Host "http_json panic msg MISSING"; $fail++ }

if ($srvProc) { Stop-Process -Id $srvProc.Id -Force -ErrorAction SilentlyContinue }
if ($fail -eq 0) { Write-Host "B4 ALL PASS" } else { Write-Host "$fail FAILED"; exit 1 }
