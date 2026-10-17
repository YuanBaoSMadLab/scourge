# Regression: HTTP server probes (net_http_read_request/write_response).
# Flow: build probe -> AOT exe server + python client -> VM run server +
# python client -> diff server logs (dual-mode byte check).
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
# Env note: drop lowercase proxy duplicates (case-insensitive .NET dict).
foreach ($n in @('http_proxy', 'https_proxy', 'no_proxy')) {
    $up = $n.ToUpperInvariant()
    if ([Environment]::GetEnvironmentVariable($n, 'Process') -ne $null -and
        [Environment]::GetEnvironmentVariable($up, 'Process') -ne $null) {
        [Environment]::SetEnvironmentVariable($n, $null, 'Process')
    }
}
# --- sg executable probe: PATH first (verify is Scourge), fallback repo build-rel ----
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
$cmd = Get-Command sg -ErrorAction SilentlyContinue | Select-Object -First 1
if ($cmd) {
    # Sanity check: ensure it's Scourge, not ast-grep or some other tool
    $sgOutArr = & $cmd.Source --help 2>&1
    $sgHelp = ($sgOutArr -join "`n")
    $sgExit = $LASTEXITCODE
    if ($sgExit -eq 0 -and $sgHelp -match 'scourge') {
        $exe = $cmd.Source
    }
}
if (-not $exe) { $exe = Join-Path $repoRoot 'build-rel\sg.exe' }
if (-not $exe -or -not (Test-Path $exe)) { throw "sg not found: no Scourge sg in PATH and $(Join-Path $repoRoot 'build-rel\sg.exe') does not exist" }
# -------------------------------------------------------------------------------------
$dir = "d:\Scourge\build-rel\verify"
$cb = "d:\Scourge\verify\probes"
$py = "d:\Scourge\verify\http_test_client.py"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0
$sg = "$cb\probe_http_acc_server.sg"
$stem = "probe_http_acc_server"

# ---- Part 1: build AOT server ----
Remove-Item "$dir\$stem*" -Recurse -Force -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") `
     -NoNewWindow -Wait -PassThru `
     -RedirectStandardOutput "$dir\$stem.build.out" `
     -RedirectStandardError "$dir\$stem.build.err"
if ($p.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err"; exit 1 }

# ---- Part 2: run server mode (AOT exe / VM run) + python client ----
foreach ($mode in @('aot', 'vm')) {
    $log = "$dir\$stem.$mode.log"
    if ($mode -eq 'aot') {
        $proc = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -PassThru `
                -RedirectStandardOutput $log -RedirectStandardError "$dir\$stem.$mode.err"
    } else {
        $proc = Start-Process -FilePath $exe -ArgumentList @("run", $sg) `
                -NoNewWindow -PassThru `
                -RedirectStandardOutput $log -RedirectStandardError "$dir\$stem.$mode.err"
    }
    Start-Sleep -Milliseconds 1200
    $c = Start-Process -FilePath "python" -ArgumentList @($py) -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput "$dir\$stem.$mode.client.out" `
         -RedirectStandardError "$dir\$stem.$mode.client.err"
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    else { Wait-Process -Id $proc.Id -ErrorAction SilentlyContinue }
    $srvOut = Get-Content $log -ErrorAction SilentlyContinue
    $cliPass = ($c.ExitCode -eq 0)
    $srvBye = ($srvOut -contains 'bye')
    if ($cliPass -and $srvBye) { Write-Host "$stem $mode PASS (client exit 0, server bye)" }
    else {
        Write-Host "$stem $mode FAIL client=$cliPass bye=$srvBye"
        Get-Content "$dir\$stem.$mode.err" -ErrorAction SilentlyContinue | Select-Object -First 3
        $srvOut | Select-Object -First 5
        $fail++
    }
}

# ---- Part 3: dual-mode server log byte identity ----
$a = Get-Content "$dir\$stem.aot.log" -ErrorAction SilentlyContinue
$v = Get-Content "$dir\$stem.vm.log" -ErrorAction SilentlyContinue
if (($a -join "`n") -eq ($v -join "`n")) { Write-Host "$stem logs IDENTICAL" }
else { Write-Host "$stem logs DIFFER"; $fail++ }

if ($fail -eq 0) { Write-Host "HTTP ALL PASS" } else { Write-Host "$fail FAILED"; exit 1 }
