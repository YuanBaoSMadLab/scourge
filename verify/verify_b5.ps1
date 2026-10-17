# Regression: 0.9.11 batch-5 (true-concurrency M1) dual-mode byte check.
# Probes: probe_f_thread_sum.sg (4 threads x 1000 locked Dict increments -> 4000)
#         + probe_f_chan_pingpong.sg (sender thread -> sentinel + close; main
#           recv loop breaks on "END" -> first/last/count = msg0/msg19/20).
# Dual-mode determinism: multi-thread probes print only after join, so the
# output is stable. Each probe runs 5x per mode; results must be constant.
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
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
