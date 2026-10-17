# Batch 8 probe 1 (F-20): callable panic(). Covers caught panic (crossing a
# function call, resume after recover), tail-call dead-block path (AOT
# implicit-return closeout), uncaught panic (exit 1 + byte-identical stderr),
# and Sema rejection of non-str arg. ASCII only: PowerShell 5.1 parses
# BOM-less ps1 as ANSI.
$exe = "d:\Scourge\build-rel\scourge.exe"
$dir = "d:\Scourge\build-rel\verify"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

# --- probe 1: caught panic, tail panic, resume (exit 0, stderr empty) ---
$stem = 'probe_b8_panic'
$sg = "d:\Scourge\verify\probes\$stem.sg"
Remove-Item "$dir\$stem.*" -Recurse -Force -ErrorAction SilentlyContinue
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $sg) -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
if ($p1.ExitCode -ne 0) { Write-Host "$stem VM-FAIL"; Get-Content "$dir\$stem.run.err" -First 5; $fail++ }
else {
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
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
            if ($r1 -eq $r3 -and $e1 -eq $e3) { Write-Host "$stem IDENTICAL (exit 0)" }
            else {
                Write-Host "$stem DIFFER"
                Write-Host "--- run:"; Get-Content "$dir\$stem.run.out"
                Write-Host "--- aot:"; Get-Content "$dir\$stem.aot.out"
                $fail++
            }
        }
    }
}

# --- probe 2: uncaught panic -> exit 1, byte-identical canonical stderr ---
$stem = 'probe_b8_panic_uncaught'
$sg = "d:\Scourge\verify\probes\$stem.sg"
Remove-Item "$dir\$stem.*" -Recurse -Force -ErrorAction SilentlyContinue
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $sg) -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
if ($p1.ExitCode -ne 1) { Write-Host "$stem VM-EXIT $($p1.ExitCode) (want 1)"; $fail++ }
else {
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    if ($p2.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err" -First 5; $fail++ }
    else {
        $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
              -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"
        if ($p3.ExitCode -ne 1) { Write-Host "$stem AOT-EXIT $($p3.ExitCode) (want 1)"; $fail++ }
        else {
            $e1 = (Get-FileHash "$dir\$stem.run.err").Hash
            $e3 = (Get-FileHash "$dir\$stem.aot.err").Hash
            if ($e1 -eq $e3) {
                Write-Host "$stem IDENTICAL stderr (exit 1)"
                Get-Content "$dir\$stem.run.err" -First 2
            }
            else {
                Write-Host "$stem STDERR-DIFFER"
                Write-Host "--- run:"; Get-Content "$dir\$stem.run.err" -First 3
                Write-Host "--- aot:"; Get-Content "$dir\$stem.aot.err" -First 3
                $fail++
            }
        }
    }
}

# --- probe 3: Sema rejects non-str arg (compile-time, both modes) ---
$rej = "$dir\probe_b8_panic_reject.sg"
Set-Content -Path $rej -Value "def main() -> int:`r`n    panic(1)`r`n    return 0`r`n" -Encoding ASCII
foreach ($mode in @("run", "build")) {
    $p = Start-Process -FilePath $exe -ArgumentList @($mode, $rej) -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput "$dir\probe_b8_rej.$mode.out" -RedirectStandardError "$dir\probe_b8_rej.$mode.err"
    $txt = (Get-Content "$dir\probe_b8_rej.$mode.out" -Raw) + (Get-Content "$dir\probe_b8_rej.$mode.err" -Raw)
    if ($p.ExitCode -eq 0 -or -not $txt.Contains("panic()")) {
        Write-Host "reject $mode FAIL (exit $($p.ExitCode))"; $fail++
    } else { Write-Host "reject $mode OK" }
}

# --- probe 4: F-19 same-package multi-module import merge (exit 0) ---
$stem = 'probe_b8_import'
$sg = "d:\Scourge\verify\probes\$stem.sg"
Remove-Item "$dir\$stem.*" -Recurse -Force -ErrorAction SilentlyContinue
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $sg) -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
if ($p1.ExitCode -ne 0) { Write-Host "$stem VM-FAIL"; Get-Content "$dir\$stem.run.err" -First 5; $fail++ }
else {
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    if ($p2.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err" -First 5; $fail++ }
    else {
        $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
              -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"
        if ($p3.ExitCode -ne 0) { Write-Host "$stem AOT-FAIL"; Get-Content "$dir\$stem.aot.err" -First 5; $fail++ }
        else {
            $r1 = (Get-Content "$dir\$stem.run.out") -join "`n"
            $r3 = (Get-Content "$dir\$stem.aot.out") -join "`n"
            if ($r1 -eq $r3 -and $r1.Split("`n").Count -eq 3) { Write-Host "$stem IDENTICAL (exit 0)" }
            else {
                Write-Host "$stem DIFFER"
                Write-Host "--- run:"; Get-Content "$dir\$stem.run.out"
                Write-Host "--- aot:"; Get-Content "$dir\$stem.aot.out"
                $fail++
            }
        }
    }
}

# --- probe 5: F-22 print/str of List[Tuple] via Dict.items() (exit 0) ---
$stem = 'probe_b8_dictitems'
$sg = "d:\Scourge\verify\probes\$stem.sg"
Remove-Item "$dir\$stem.*" -Recurse -Force -ErrorAction SilentlyContinue
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $sg) -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
if ($p1.ExitCode -ne 0) { Write-Host "$stem VM-FAIL"; Get-Content "$dir\$stem.run.err" -First 5; $fail++ }
else {
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    if ($p2.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err" -First 5; $fail++ }
    else {
        $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
              -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"
        if ($p3.ExitCode -ne 0) { Write-Host "$stem AOT-FAIL"; Get-Content "$dir\$stem.aot.err" -First 5; $fail++ }
        else {
            $r1 = (Get-Content "$dir\$stem.run.out") -join "`n"
            $r3 = (Get-Content "$dir\$stem.aot.out") -join "`n"
            if ($r1 -eq $r3 -and $r1.Split("`n").Count -eq 5) { Write-Host "$stem IDENTICAL (exit 0)" }
            else {
                Write-Host "$stem DIFFER"
                Write-Host "--- run:"; Get-Content "$dir\$stem.run.out"
                Write-Host "--- aot:"; Get-Content "$dir\$stem.aot.out"
                $fail++
            }
        }
    }
}

if ($fail -eq 0) { Write-Host "B8 ALL PASS" } else { Write-Host "$fail FAILED"; exit 1 }
