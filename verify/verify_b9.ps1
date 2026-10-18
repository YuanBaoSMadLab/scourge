# Batch 9 probe 1 (F-21): Tuple[K, V, ...] generic spelling. Covers func
# param/return annotations, local var, 3-tuple, plus Dict.items() cross-check.
# Also rejects bare "Tuple" without type params at compile time in both modes.
# ASCII only: PowerShell 5.1 parses BOM-less ps1 as ANSI.
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
$fail = 0

# --- probe 1: Tuple[...] spelling dual-mode identical (exit 0, 10 lines) ---
$stem = 'probe_b9_tuple'
$scg = "d:\Scourge\verify\probes\$stem.sg"
Remove-Item "$dir\$stem.*" -Recurse -Force -ErrorAction SilentlyContinue
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $scg) -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
if ($p1.ExitCode -ne 0) { Write-Host "$stem VM-FAIL"; Get-Content "$dir\$stem.run.err" -First 5; $fail++ }
else {
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $scg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    if ($p2.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err" -First 5; $fail++ }
    else {
        $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
              -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"
        if ($p3.ExitCode -ne 0) { Write-Host "$stem AOT-FAIL"; Get-Content "$dir\$stem.aot.err" -First 5; $fail++ }
        else {
            $r1 = (Get-Content "$dir\$stem.run.out") -join "`n"
            $r3 = (Get-Content "$dir\$stem.aot.out") -join "`n"
            $e1 = (Get-FileHash "$dir\$stem.run.err").Hash
            $e3 = (Get-FileHash "$dir\$stem.aot.err").Hash
            if ($r1 -eq $r3 -and $e1 -eq $e3 -and $r1.Split("`n").Count -eq 10) {
                Write-Host "$stem IDENTICAL (exit 0, 10 lines)"
            }
            else {
                Write-Host "$stem DIFFER"
                Write-Host "--- run:"; Get-Content "$dir\$stem.run.out"
                Write-Host "--- aot:"; Get-Content "$dir\$stem.aot.out"
                $fail++
            }
        }
    }
}

# --- probe 2: nn.rand_uniform XOR training (random init breaks symmetry,
# --- fixed seed; dual-mode identical, 6 lines) ---
$stem = 'probe_b9_randinit'
$scg = "d:\Scourge\verify\probes\$stem.sg"
Remove-Item "$dir\$stem.*" -Recurse -Force -ErrorAction SilentlyContinue
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $scg) -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
if ($p1.ExitCode -ne 0) { Write-Host "$stem VM-FAIL"; Get-Content "$dir\$stem.run.err" -First 5; $fail++ }
else {
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $scg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    if ($p2.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err" -First 5; $fail++ }
    else {
        $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
              -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"
        if ($p3.ExitCode -ne 0) { Write-Host "$stem AOT-FAIL"; Get-Content "$dir\$stem.aot.err" -First 5; $fail++ }
        else {
            $r1 = (Get-Content "$dir\$stem.run.out") -join "`n"
            $r3 = (Get-Content "$dir\$stem.aot.out") -join "`n"
            $e1 = (Get-FileHash "$dir\$stem.run.err").Hash
            $e3 = (Get-FileHash "$dir\$stem.aot.err").Hash
            if ($r1 -eq $r3 -and $e1 -eq $e3 -and $r1.Split("`n").Count -eq 6 -and
                $r1.Contains("accuracy 4 / 4")) {
                Write-Host "$stem IDENTICAL (XOR 4/4)"
            }
            else {
                Write-Host "$stem DIFFER"
                Write-Host "--- run:"; Get-Content "$dir\$stem.run.out"
                Write-Host "--- aot:"; Get-Content "$dir\$stem.aot.out"
                $fail++
            }
        }
    }
}

# --- probe 3: bare Tuple (no params) rejected at compile time, both modes ---
$rej = "$dir\probe_b9_tuple_reject.sg"
Set-Content -Path $rej -Value "def main() -> int:`r`n    x: Tuple = (1, 2)`r`n    print(x)`r`n    return 0`r`n" -Encoding ASCII
foreach ($mode in @("run", "build")) {
    $p = Start-Process -FilePath $exe -ArgumentList @($mode, $rej) -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput "$dir\probe_b9_rej.$mode.out" -RedirectStandardError "$dir\probe_b9_rej.$mode.err"
    $txt = (Get-Content "$dir\probe_b9_rej.$mode.out" -Raw) + (Get-Content "$dir\probe_b9_rej.$mode.err" -Raw)
    if ($p.ExitCode -eq 0 -or -not $txt.Contains("Tuple")) {
        Write-Host "reject $mode FAIL (exit $($p.ExitCode))"; $fail++
    } else { Write-Host "reject $mode OK" }
}

# --- probe 4: implicit monomorphization (batch 9-3). Unannotated params
# --- instantiate per call-site arg types (int/float/str/List[float]);
# --- default-value param typing; dual-mode identical, 12 lines.
$stem = 'probe_b9_monomorph'
$scg = "d:\Scourge\verify\probes\$stem.sg"
Remove-Item "$dir\$stem.*" -Recurse -Force -ErrorAction SilentlyContinue
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $scg) -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
if ($p1.ExitCode -ne 0) { Write-Host "$stem VM-FAIL"; Get-Content "$dir\$stem.run.err" -First 5; $fail++ }
else {
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $scg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    if ($p2.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err" -First 5; $fail++ }
    else {
        $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
              -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"
        if ($p3.ExitCode -ne 0) { Write-Host "$stem AOT-FAIL"; Get-Content "$dir\$stem.aot.err" -First 5; $fail++ }
        else {
            $r1 = (Get-Content "$dir\$stem.run.out") -join "`n"
            $r3 = (Get-Content "$dir\$stem.aot.out") -join "`n"
            $e1 = (Get-FileHash "$dir\$stem.run.err").Hash
            $e3 = (Get-FileHash "$dir\$stem.aot.err").Hash
            if ($r1 -eq $r3 -and $e1 -eq $e3 -and $r1.Split("`n").Count -eq 12 -and
                $r1.Contains("i 7") -and $r1.Contains("f 6.5") -and
                $r1.Contains("s hi-hi") -and $r1.Contains("m 3.5") -and
                $r1.Contains("L 1.5") -and $r1.Contains("d 15")) {
                Write-Host "$stem IDENTICAL (mono int/float/str/list, 12 lines)"
            }
            else {
                Write-Host "$stem DIFFER"
                Write-Host "--- run:"; Get-Content "$dir\$stem.run.out"
                Write-Host "--- aot:"; Get-Content "$dir\$stem.aot.out"
                $fail++
            }
        }
    }
}

# --- probe 5: generic self-recursion without return annotation rejected
# --- at compile time (Q10 boundary), both modes.
$rej = "$dir\probe_b9_mono_reject.sg"
Set-Content -Path $rej -Value "def fact(n):`r`n    if n <= 1:`r`n        return 1`r`n    return n * fact(n - 1)`r`n`r`ndef main() -> int:`r`n    print(fact(5))`r`n    return 0`r`n" -Encoding ASCII
foreach ($mode in @("run", "build")) {
    $p = Start-Process -FilePath $exe -ArgumentList @($mode, $rej) -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput "$dir\probe_b9_monorej.$mode.out" -RedirectStandardError "$dir\probe_b9_monorej.$mode.err"
    $txt = (Get-Content "$dir\probe_b9_monorej.$mode.out" -Raw) + (Get-Content "$dir\probe_b9_monorej.$mode.err" -Raw)
    if ($p.ExitCode -eq 0 -or -not $txt.Contains("E2001")) {
        Write-Host "mono-reject $mode FAIL (exit $($p.ExitCode))"; $fail++
    } else { Write-Host "mono-reject $mode OK" }
}

# --- probe 6: half-open slices (s[:p] / s[p+1:]) inside a monomorphized
# --- instance (cloneExpr null-guard regression). Dual-mode identical, 2 lines.
$stem = 'probe_b9_monoslice'
$scg = "d:\Scourge\verify\probes\$stem.sg"
Remove-Item "$dir\$stem.*" -Recurse -Force -ErrorAction SilentlyContinue
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $scg) -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
if ($p1.ExitCode -ne 0) { Write-Host "$stem VM-FAIL"; Get-Content "$dir\$stem.run.err" -First 5; $fail++ }
else {
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $scg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    if ($p2.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err" -First 5; $fail++ }
    else {
        $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
              -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"
        if ($p3.ExitCode -ne 0) { Write-Host "$stem AOT-FAIL"; Get-Content "$dir\$stem.aot.err" -First 5; $fail++ }
        else {
            $r1 = (Get-Content "$dir\$stem.run.out") -join "`n"
            $r3 = (Get-Content "$dir\$stem.aot.out") -join "`n"
            $e1 = (Get-FileHash "$dir\$stem.run.err").Hash
            $e3 = (Get-FileHash "$dir\$stem.aot.err").Hash
            if ($r1 -eq $r3 -and $e1 -eq $e3 -and
                $r1.Contains("28") -and $r1.Contains("4") -and
                -not $r1.Contains("13")) {
                Write-Host "$stem IDENTICAL (mono half-open slice, 2 lines)"
            }
            else {
                Write-Host "$stem DIFFER"
                Write-Host "--- run:"; Get-Content "$dir\$stem.run.out"
                Write-Host "--- aot:"; Get-Content "$dir\$stem.aot.out"
                $fail++
            }
        }
    }
}

# --- probe 7: empty dict literal under Dict[K,V] annotation + cross-pointer
# --- str key lookup (checkDictLit ktag regression). Dual-mode identical, 3 lines.
$stem = 'probe_b9_dictempty'
$scg = "d:\Scourge\verify\probes\$stem.sg"
Remove-Item "$dir\$stem.*" -Recurse -Force -ErrorAction SilentlyContinue
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $scg) -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
if ($p1.ExitCode -ne 0) { Write-Host "$stem VM-FAIL"; Get-Content "$dir\$stem.run.err" -First 5; $fail++ }
else {
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $scg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    if ($p2.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err" -First 5; $fail++ }
    else {
        $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
              -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"
        if ($p3.ExitCode -ne 0) { Write-Host "$stem AOT-FAIL"; Get-Content "$dir\$stem.aot.err" -First 5; $fail++ }
        else {
            $r1 = (Get-Content "$dir\$stem.run.out") -join "`n"
            $r3 = (Get-Content "$dir\$stem.aot.out") -join "`n"
            $e1 = (Get-FileHash "$dir\$stem.run.err").Hash
            $e3 = (Get-FileHash "$dir\$stem.aot.err").Hash
            if ($r1 -eq $r3 -and $e1 -eq $e3 -and $r1.Split("`n").Count -eq 3 -and
                -not $r1.Contains("-1")) {
                Write-Host "$stem IDENTICAL (dict empty-lit ktag, 3 lines)"
            }
            else {
                Write-Host "$stem DIFFER"
                Write-Host "--- run:"; Get-Content "$dir\$stem.run.out"
                Write-Host "--- aot:"; Get-Content "$dir\$stem.aot.out"
                $fail++
            }
        }
    }
}

if ($fail -eq 0) { Write-Host "B9-1 ALL PASS" } else { Write-Host "$fail FAILED"; exit 1 }
