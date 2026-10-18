# Regression: Dict runtime dual-mode byte check (0.9.7, defect Q5).
# Q5 claimed Dict was annotation-only; in fact literal/str-key indexing/
# len/in/for-key-iteration were already live. New in 0.9.7: AOT print(Dict).
# Locks: literal + insert + len + membership + for-key + print (insertion
# order), missing-key panic stderr equality, and the capability asymmetry
# contract: container-valued Dict prints on VM but is a compile-time E1007
# on AOT (64-bit value slot carries no type tag for repr).
# Sema negative: Dict member methods (get/keys/...) are E2003 by design.
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
$dir = "d:\Scourge\build-rel\verify\dict"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

function Clean-Artifacts([string]$stem) {
    try { Remove-Item -Path "$dir\$stem.*" -Force -ErrorAction SilentlyContinue } catch {}
}

# --- positive: survey triple diff (byte identical) ------------------------
$n = "probe_dict_survey"
Clean-Artifacts $n
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", "d:\Scourge\verify\probes\$n.sg") -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$n.run.out" -RedirectStandardError "$dir\$n.run.err"
$p2 = Start-Process -FilePath $exe -ArgumentList @("build", "d:\Scourge\verify\probes\$n.sg", "-o", "$dir\$n.exe") -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$n.build.out" -RedirectStandardError "$dir\$n.build.err"
if ($p2.ExitCode -ne 0) {
    Write-Host "$n BUILD-FAIL"; Get-Content "$dir\$n.build.err" -First 20; $fail = 1
} else {
    $p3 = Start-Process -FilePath "$dir\$n.exe" -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.aot.out" -RedirectStandardError "$dir\$n.aot.err"
    $r1 = Get-Content "$dir\$n.run.out"
    $r3 = Get-Content "$dir\$n.aot.out"
    $o = (($r1 -join "`n") -eq ($r3 -join "`n"))
    $e = (Get-FileHash "$dir\$n.run.err").Hash -eq (Get-FileHash "$dir\$n.aot.err").Hash
    $x = $p1.ExitCode -eq $p3.ExitCode
    if ($o -and $e -and $x) {
        Write-Host "$n IDENTICAL (exit $($p1.ExitCode))"
    } else {
        Write-Host "$n DIFFER out=$o err=$e exit=$($p1.ExitCode)/$($p3.ExitCode)"
        if (-not $o) { Write-Host "--- run:"; $r1; Write-Host "--- aot:"; $r3 }
        $fail = 1
    }
}

# --- panic: missing key stderr must be byte-identical ---------------------
Clean-Artifacts "probe_dict_panic"
$src = "d:\Scourge\verify\probes\probe_dict_panic.sg"
Set-Content -Path $src -Encoding Ascii -Value @('def main() -> int:', '    d: Dict[str, int] = {"a": 1}', '    print(d["zz"])', '    return 0')
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $src) -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\probe_dict_panic.run.out" -RedirectStandardError "$dir\probe_dict_panic.run.err"
$p2 = Start-Process -FilePath $exe -ArgumentList @("build", $src, "-o", "$dir\probe_dict_panic.exe") -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\probe_dict_panic.build.out" -RedirectStandardError "$dir\probe_dict_panic.build.err"
$p3 = Start-Process -FilePath "$dir\probe_dict_panic.exe" -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\probe_dict_panic.aot.out" -RedirectStandardError "$dir\probe_dict_panic.aot.err"
$e1 = Get-Content "$dir\probe_dict_panic.run.err" -Raw
$e3 = Get-Content "$dir\probe_dict_panic.aot.err" -Raw
$panicOk = $p1.ExitCode -eq $p3.ExitCode -and $p1.ExitCode -ne 0 -and ($e1 -ceq $e3) -and $e1.StartsWith("panic:")
if ($panicOk) {
    Write-Host "probe_dict_panic PANIC-IDENTICAL (exit $($p1.ExitCode))"
} else {
    Write-Host "probe_dict_panic MISMATCH run=$($p1.ExitCode) aot=$($p3.ExitCode)"
    Write-Host "--- run err:"; $e1
    Write-Host "--- aot err:"; $e3
    $fail = 1
}

# --- asymmetry contract: container-valued Dict print ----------------------
# VM prints it; AOT must reject at compile time with E1007 (not a dual-mode
# violation: E1007 is the established explicit-rejection channel).
Clean-Artifacts "probe_dict_cval"
$src = "d:\Scourge\verify\probes\probe_dict_cval.sg"
Set-Content -Path $src -Encoding Ascii -Value @('def main() -> int:', '    x: List[int] = [1, 2]', '    d: Dict[str, List[int]] = {"a": x}', '    print(d)', '    return 0')
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $src) -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\probe_dict_cval.run.out" -RedirectStandardError "$dir\probe_dict_cval.run.err"
$p2 = Start-Process -FilePath $exe -ArgumentList @("build", $src, "-o", "$dir\probe_dict_cval.exe") -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\probe_dict_cval.build.out" -RedirectStandardError "$dir\probe_dict_cval.build.err"
$vmOut = (Get-Content "$dir\probe_dict_cval.run.out" -Raw)
$aotErr = (Get-Content "$dir\probe_dict_cval.build.err" -Raw)
if ($p1.ExitCode -eq 0 -and $vmOut.Trim() -eq '{"a": [1, 2]}' -and
    $p2.ExitCode -eq 1 -and $aotErr -match "E1007") {
    Write-Host "probe_dict_cval ASYMMETRY-OK (vm prints, aot E1007)"
} else {
    Write-Host "probe_dict_cval MISMATCH vm=$($p1.ExitCode) build=$($p2.ExitCode)"
    Write-Host "--- vm out:"; $vmOut
    Write-Host "--- build err:"; $aotErr.Substring(0, [Math]::Min(300, $aotErr.Length))
    $fail = 1
}

if ($fail -eq 0) { Write-Host "DICT PASS" } else { exit 1 }
