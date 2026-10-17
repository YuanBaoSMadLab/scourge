# Regression: deferred inference for empty containers (List/Dict placeholder
# refined by first write / call argument / return value).
# Positives: probe_infer_container.sg - dual-mode byte-identical.
# Sema negatives: probe_infer_neg_{list,dict,for,ret}.sg - both channels exit 1
#   with E2002 in stderr.
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

function Clean-Artifacts([string]$stem) {
    try { Remove-Item -Path "$dir\$stem.*" -Recurse -Force -Confirm:$false -ErrorAction SilentlyContinue } catch {}
}

# --- positive: run vs build+aot triple diff (byte identical) -------------
$stem = "probe_infer_container"
Clean-Artifacts $stem
$p1 = Start-Process -FilePath $exe -ArgumentList @("run", "d:\Scourge\verify\probes\$stem.sg") -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
$p2 = Start-Process -FilePath $exe -ArgumentList @("build", "d:\Scourge\verify\probes\$stem.sg", "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
if ($p1.ExitCode -ne 0) {
    Write-Host "$stem VM-FAIL"; Get-Content "$dir\$stem.run.err" -First 5; $fail = 1
} elseif ($p2.ExitCode -ne 0) {
    Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err" -First 5; $fail = 1
} else {
    $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"
    $o = ((Get-Content "$dir\$stem.run.out") -join "`n") -eq ((Get-Content "$dir\$stem.aot.out") -join "`n")
    $e = (Get-FileHash "$dir\$stem.run.err").Hash -eq (Get-FileHash "$dir\$stem.aot.err").Hash
    $x = $p1.ExitCode -eq $p3.ExitCode
    if ($o -and $e -and $x) {
        Write-Host "$stem IDENTICAL (exit $($p1.ExitCode))"
    } else {
        Write-Host "$stem DIFFER out=$o err=$e exit=$($p1.ExitCode)/$($p3.ExitCode)"
        if (-not $o) { Write-Host "--- run:"; Get-Content "$dir\$stem.run.out"; Write-Host "--- aot:"; Get-Content "$dir\$stem.aot.out" }
        $fail = 1
    }
}

# --- sema negatives: both channels exit 1 with E2002 ----------------------
foreach ($stem in @("probe_infer_neg_list", "probe_infer_neg_dict",
                    "probe_infer_neg_for", "probe_infer_neg_ret")) {
    Clean-Artifacts $stem
    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", "d:\Scourge\verify\probes\$stem.sg") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", "d:\Scourge\verify\probes\$stem.sg", "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    $rErr = (Get-Content "$dir\$stem.run.err" -Raw) -match "E2002"
    $bErr = (Get-Content "$dir\$stem.build.err" -Raw) -match "E2002"
    if ($p1.ExitCode -eq 1 -and $p2.ExitCode -eq 1 -and $rErr -and $bErr) {
        Write-Host "$stem REJECTED (E2002 both channels)"
    } else {
        Write-Host "$stem MISMATCH run=$($p1.ExitCode) build=$($p2.ExitCode) runHas=$rErr buildHas=$bErr"
        Get-Content "$dir\$stem.run.err" -First 4
        $fail = 1
    }
}

if ($fail -eq 0) { Write-Host "INFER-CONTAINER PASS" } else { exit 1 }
