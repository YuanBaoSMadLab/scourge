# Regression: 0.9.11 batch-5 (true-concurrency M1) dual-mode byte check.
# Probes: probe_f_thread_sum.sg (4 threads x 1000 locked Dict increments -> 4000)
#         + probe_f_chan_pingpong.sg (sender thread -> sentinel + close; main
#           recv loop breaks on "END" -> first/last/count = msg0/msg19/20).
# Dual-mode determinism: multi-thread probes print only after join, so the
# output is stable. Each probe runs 5x per mode; results must be constant.
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
$exe = "d:\Scourge\build-rel\scourge.exe"
$dir = "d:\Scourge\build-rel\verify"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

$probes = @("probe_f_thread_sum", "probe_f_chan_pingpong")

foreach ($stem in $probes) {
    $sg = "d:\Scourge\verify\probes\$stem.sg"
    $results = @()
    for ($i = 0; $i -lt 5; $i++) {
        # VM mode
        $p1 = Start-Process -FilePath $exe -ArgumentList @("run", $sg) `
              -NoNewWindow -Wait -PassThru `
              -RedirectStandardOutput "$dir\$stem.vm.out" `
              -RedirectStandardError "$dir\$stem.vm.err"
        if ($p1.ExitCode -ne 0) {
            Write-Host "$stem VM-RUN-FAIL (exit $($p1.ExitCode))"
            Get-Content "$dir\$stem.vm.err"
            $fail++
            break
        }
        # AOT mode: build once per probe (not per repeat), then run 5x
        if ($i -eq 0) {
            $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") `
                  -NoNewWindow -Wait -PassThru `
                  -RedirectStandardOutput "$dir\$stem.build.out" `
                  -RedirectStandardError "$dir\$stem.build.err"
            if ($p2.ExitCode -ne 0) {
                Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err"
                $fail++
                break
            }
        }
        $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
              -RedirectStandardOutput "$dir\$stem.aot.out" `
              -RedirectStandardError "$dir\$stem.aot.err"
        if ($p3.ExitCode -ne 0) {
            Write-Host "$stem AOT-RUN-FAIL (exit $($p3.ExitCode))"
            Get-Content "$dir\$stem.aot.err"
            $fail++
            break
        }
        # stderr must be empty (no panic) and output identical across modes
        $eEmpty = (Get-Item "$dir\$stem.vm.err").Length -eq 0 -and
                  (Get-Item "$dir\$stem.aot.err").Length -eq 0
        $same = ((Get-Content "$dir\$stem.vm.out" -Raw) -eq
                 (Get-Content "$dir\$stem.aot.out" -Raw))
        if (-not ($eEmpty -and $same)) {
            Write-Host "$stem DIFFER same=$same errEmpty=$eEmpty"
            Write-Host "--- vm:";  Get-Content "$dir\$stem.vm.out"
            Write-Host "--- aot:"; Get-Content "$dir\$stem.aot.out"
            $fail++
            break
        }
        $results += (Get-Content "$dir\$stem.vm.out" -Raw)
    }
    if ($results.Count -eq 5) {
        $uniq = @($results | Sort-Object -Unique)
        if ($uniq.Count -ne 1) {
            Write-Host "$stem NON-DETERMINISTIC over 5 runs"
            $uniq | ForEach-Object { Write-Host "--- $_" }
            $fail++
        } else {
            Write-Host "$stem STABLE x5 dual-mode ($($uniq[0].Trim() -replace "`n", ' | '))"
        }
    }
}

if ($fail -eq 0) { Write-Host "B5 ALL PASS" } else { Write-Host "$fail FAILED"; exit 1 }
