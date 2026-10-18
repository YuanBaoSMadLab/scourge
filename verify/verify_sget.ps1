# Regression: sget package manager end-to-end.
# Flow: build sget -> install builtin pkgs -> FFI demo dual-mode ->
# pkg DLL deploy check -> uninstall. ASCII only (PS 5.1 ANSI parsing).
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
$sgetDir = "d:\Scourge\examples\sget"
$fail = 0

function Run([string]$file, [string]$argsLine) {
    $p = Start-Process -FilePath $file -ArgumentList $argsLine -NoNewWindow `
         -Wait -PassThru -WorkingDirectory $sgetDir `
         -RedirectStandardOutput "$sgetDir\.v.out" `
         -RedirectStandardError "$sgetDir\.v.err"
    return $p.ExitCode
}

# ---- 1. build sget itself ----
if ((Run $exe "build main.sg -o sget.exe") -ne 0) {
    Write-Host "sget BUILD-FAIL"; Get-Content "$sgetDir\.v.err"; exit 1
}
Write-Host "sget self-build PASS"

# ---- 2. install builtin packages ----
foreach ($p in @('user32', 'kernel32', 'msvcrt')) {
    if ((Run "$sgetDir\sget.exe" "install $p") -ne 0) { Write-Host "install $p FAIL"; $fail++ }
}
if (-not (Test-Path "$sgetDir\packages\user32\user32.sg") -or
    -not (Test-Path "$sgetDir\sget.lock.json")) { Write-Host "install artifacts FAIL"; $fail++ }
else { Write-Host "install PASS" }

# ---- 3. FFI demo dual-mode (identical output) ----
if ((Run $exe "build demo.sg -o demo.exe") -ne 0) {
    Write-Host "demo BUILD-FAIL"; Get-Content "$sgetDir\.v.err"; $fail++
} else {
    $p1 = Start-Process -FilePath "$sgetDir\demo.exe" -NoNewWindow -Wait -PassThru `
          -WorkingDirectory $sgetDir -RedirectStandardOutput "$sgetDir\.d.aot" `
          -RedirectStandardError "$sgetDir\.d.aoterr"
    $p2 = Start-Process -FilePath $exe -ArgumentList "run demo.sg" -NoNewWindow -Wait -PassThru `
          -WorkingDirectory $sgetDir -RedirectStandardOutput "$sgetDir\.d.vm" `
          -RedirectStandardError "$sgetDir\.d.vmerr"
    $a = Get-Content "$sgetDir\.d.aot"; $v = Get-Content "$sgetDir\.d.vm"
    if ($p1.ExitCode -eq 0 -and $p2.ExitCode -eq 0 -and (($a -join "`n") -eq ($v -join "`n")) -and
        ($a -contains 'MulDiv(7, 3, 2) = 11')) { Write-Host "ffi demo dual-mode PASS" }
    else { Write-Host "ffi demo DIFFER"; $a; $v; $fail++ }
}

# ---- 4. pkg DLL deploy: build-time copy packages/*/bin/*.dll ----
New-Item -ItemType Directory -Force "$sgetDir\packages\testpkg\bin" | Out-Null
Set-Content -Path "$sgetDir\packages\testpkg\bin\probe.dll" -Value "stub"
if ((Run $exe "build demo.sg -o demo.exe") -ne 0) { Write-Host "rebuild FAIL"; $fail++ }
if (Test-Path "$sgetDir\probe.dll") { Write-Host "pkg dll deploy PASS" }
else { Write-Host "pkg dll deploy FAIL (probe.dll missing)"; $fail++ }
Remove-Item "$sgetDir\probe.dll", "$sgetDir\packages\testpkg" -Recurse -Force -ErrorAction SilentlyContinue

# ---- 5. uninstall ----
if ((Run "$sgetDir\sget.exe" "uninstall msvcrt") -ne 0) { Write-Host "uninstall FAIL"; $fail++ }
$lock = Get-Content "$sgetDir\sget.lock.json" -Raw
if ($lock -match 'msvcrt' -or (Test-Path "$sgetDir\packages\msvcrt")) {
    Write-Host "uninstall FAIL (msvcrt still present)"; $fail++
} else { Write-Host "uninstall PASS" }

# ---- 5b. registry install: sqlite3 (network; SKIP on download failure) ----
if ((Run "$sgetDir\sget.exe" "install sqlite3") -ne 0) {
    Write-Host "registry install SKIP (network)"
} else {
    if ((Test-Path "$sgetDir\packages\sqlite3\sqlite3.sg") -and
        (Test-Path "$sgetDir\sqlite3.dll")) {
        if ((Run $exe "build sqlite_demo.sg -o sqlite_demo.exe") -eq 0) {
            $p = Start-Process -FilePath "$sgetDir\sqlite_demo.exe" -NoNewWindow -Wait -PassThru `
                 -WorkingDirectory $sgetDir -RedirectStandardOutput "$sgetDir\.sq.out" `
                 -RedirectStandardError "$sgetDir\.sq.err"
            $sq = Get-Content "$sgetDir\.sq.out" -Raw
            if ($p.ExitCode -eq 0 -and $sq -match '3\.54\.0') { Write-Host "sqlite3 registry install + FFI PASS" }
            else { Write-Host "sqlite3 demo FAIL"; $sq; $fail++ }
        } else { Write-Host "sqlite3 demo BUILD-FAIL"; Get-Content "$sgetDir\.v.err"; $fail++ }
    } else { Write-Host "sqlite3 install artifacts FAIL"; $fail++ }
}

# ---- 6. language regression: List.append probe (dual-mode) ----
$appendSg = "$sgetDir\.probe_append.sg"
Set-Content -Path $appendSg -Value @'
def fill(n: int) -> List[str]:
    out: List[str] = []
    i: int = 0
    while i < n:
        out.append("x" + str(i))
        i = i + 1
    return out

def main() -> int:
    a: List[str] = ["k"]
    a.append("m")
    print(len(a), a[0], a[1])
    b: List[str] = fill(3)
    print(len(b), b[2])
    print("".join(a))
    return 0
'@
foreach ($mode in @('run', 'build')) {
    if ($mode -eq 'run') {
        $p = Start-Process -FilePath $exe -ArgumentList @('run', $appendSg) -NoNewWindow -Wait -PassThru `
             -RedirectStandardOutput "$sgetDir\.ap.vm" -RedirectStandardError "$sgetDir\.ap.vmerr"
    } else {
        $p = Start-Process -FilePath $exe -ArgumentList @('build', $appendSg, '-o', "$sgetDir\.ap.exe") `
             -NoNewWindow -Wait -PassThru -RedirectStandardOutput "$sgetDir\.ap.bo" `
             -RedirectStandardError "$sgetDir\.ap.berr"
        if ($p.ExitCode -ne 0) { Write-Host "append build FAIL"; Get-Content "$sgetDir\.ap.berr"; $fail++; continue }
        $p = Start-Process -FilePath "$sgetDir\.ap.exe" -NoNewWindow -Wait -PassThru `
             -RedirectStandardOutput "$sgetDir\.ap.aot" -RedirectStandardError "$sgetDir\.ap.aoterr"
    }
    if ($p.ExitCode -ne 0) { Write-Host "append $mode FAIL"; $fail++ }
}
$av = Get-Content "$sgetDir\.ap.vm"; $aa = Get-Content "$sgetDir\.ap.aot"
if (($av -join "`n") -eq ($aa -join "`n") -and ($av -contains '2 k m') -and ($av -contains '3 x2')) {
    Write-Host "list.append dual-mode PASS"
} else { Write-Host "list.append DIFFER"; $av; $aa; $fail++ }
Remove-Item $appendSg, "$sgetDir\.ap.*" -Recurse -Force -ErrorAction SilentlyContinue

if ($fail -eq 0) { Write-Host "SGET ALL PASS" } else { Write-Host "$fail FAILED"; exit 1 }
