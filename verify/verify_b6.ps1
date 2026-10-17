# Regression: 0.9.11 batch-6 (GUI M1+M2, AOT-only Win32 backend).
# probe_f_gui_smoke : VM run must fixed-panic (gui_* is build-mode-only);
#                     AOT build+run opens window/label/button/input, reads/
#                     writes text, retitles, hides/shows, polls (no user
#                     events -> empty Dict), closes, prints "hi abc 0".
#                     AOT output must be constant over 5 runs, exit 0.
# probe_f_gui_nocap : missing 'capability gui' -> both modes reject at
#                     check time (E5004), exit != 0.
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
$exe = "d:\Scourge\build-rel\scourge.exe"
$dir = "d:\Scourge\build-rel\verify"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

# --- probe_f_gui_smoke ---
$stem = "probe_f_gui_smoke"
$sg = "d:\Scourge\verify\probes\$stem.sg"
# VM mode: fixed panic expected (exit 1, message mentions gui_*)
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $sg) `
      -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.vm.out" `
      -RedirectStandardError "$dir\$stem.vm.err"
$vmErr = Get-Content "$dir\$stem.vm.err" -Raw
if ($p1.ExitCode -eq 0 -or -not $vmErr.Contains("gui_*")) {
    Write-Host "$stem VM-PANIC-MISSING (exit $($p1.ExitCode))"
    Write-Host $vmErr
    $fail++
}
# AOT: build once
$p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") `
      -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.build.out" `
      -RedirectStandardError "$dir\$stem.build.err"
if ($p2.ExitCode -ne 0) {
    Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err"; $fail++
} else {
    $results = @()
    for ($i = 0; $i -lt 5; $i++) {
        $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
              -RedirectStandardOutput "$dir\$stem.aot.out" `
              -RedirectStandardError "$dir\$stem.aot.err"
        if ($p3.ExitCode -ne 0) {
            Write-Host "$stem AOT-RUN-FAIL (exit $($p3.ExitCode))"
            Get-Content "$dir\$stem.aot.err"
            $fail++
            break
        }
        $results += (Get-Content "$dir\$stem.aot.out" -Raw)
    }
    if ($results.Count -eq 5) {
        $uniq = @($results | Sort-Object -Unique)
        if ($uniq.Count -ne 1) {
            Write-Host "$stem NON-DETERMINISTIC over 5 runs"
            $fail++
        } else {
            Write-Host "$stem STABLE x5 AOT ($($uniq[0].Trim() -replace "`n", ' | '))"
        }
    }
}

# --- probe_f_gui_nocap: static reject in both modes ---
$stem = "probe_f_gui_nocap"
$sg = "d:\Scourge\verify\probes\$stem.sg"
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", $sg) `
      -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.vm.out" `
      -RedirectStandardError "$dir\$stem.vm.err"
$vmErr = Get-Content "$dir\$stem.vm.err" -Raw
if ($p1.ExitCode -eq 0 -or -not $vmErr.Contains("gui")) {
    Write-Host "$stem VM-NOCAP-MISSING (exit $($p1.ExitCode))"
    Write-Host $vmErr
    $fail++
}
$p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") `
      -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.build.out" `
      -RedirectStandardError "$dir\$stem.build.err"
$bErr = Get-Content "$dir\$stem.build.err" -Raw
if ($p2.ExitCode -eq 0 -or -not $bErr.Contains("gui")) {
    Write-Host "$stem BUILD-NOCAP-MISSING (exit $($p2.ExitCode))"
    Write-Host $bErr
    $fail++
}

if ($fail -eq 0) { Write-Host "B6 ALL PASS" } else { Write-Host "$fail FAILED"; exit 1 }
