# Batch 7 regression: autodiff/nn stdlib (std/autodiff.sg + std/nn.sg).
# Probes: linreg vs hand-written gradient, MLP vs finite difference,
# nn training (SGD + Adam). Dual-mode byte check, AOT run x5 stable.
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

$probes = @('probe_b7_ad_linreg', 'probe_b7_ad_mlp', 'probe_b7_nn_train')
foreach ($stem in $probes) {
    $sg = "d:\Scourge\verify\probes\$stem.sg"
    Remove-Item "$dir\$stem.*" -Recurse -Force -ErrorAction SilentlyContinue
    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", $sg) -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
    if ($p1.ExitCode -ne 0) { Write-Host "$stem VM-FAIL"; Get-Content "$dir\$stem.run.err" -First 5; $fail++; continue }
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    if ($p2.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err" -First 5; $fail++; continue }
    # AOT run x5: outputs must be byte-identical across runs (constant).
    $uniq = @()
    for ($k = 0; $k -lt 5; $k++) {
        $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
              -RedirectStandardOutput "$dir\$stem.aot.$k.out" -RedirectStandardError "$dir\$stem.aot.err"
        if ($p3.ExitCode -ne 0) { Write-Host "$stem AOT-FAIL (run $k)"; Get-Content "$dir\$stem.aot.err" -First 5; $fail++; break }
        $uniq += (Get-Content "$dir\$stem.aot.$k.out" -Raw)
    }
    if ($uniq.Count -eq 5 -and @($uniq | Sort-Object -Unique).Count -ne 1) {
        Write-Host "$stem NOT-STABLE"; $fail++; continue
    }
    $r1 = (Get-Content "$dir\$stem.run.out") -join "`n"
    $r3 = (Get-Content "$dir\$stem.aot.0.out") -join "`n"
    $e = (Get-FileHash "$dir\$stem.run.err").Hash -eq (Get-FileHash "$dir\$stem.aot.err").Hash
    if ($r1 -eq $r3 -and $e) { Write-Host "$stem IDENTICAL x5 (exit 0)" }
    else {
        Write-Host "$stem DIFFER err=$e"
        if ($r1 -ne $r3) { Write-Host "--- run:"; Get-Content "$dir\$stem.run.out" -First 5; Write-Host "--- aot:"; Get-Content "$dir\$stem.aot.0.out" -First 5 }
        $fail++
    }
}

if ($fail -eq 0) { Write-Host "B7 ALL PASS" } else { Write-Host "$fail FAILED"; exit 1 }
