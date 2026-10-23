# verify_script.ps1 - Script-mode dual-mode regression (top-level statements
# synthesized into main). Locks: single-line script, def mixed with top-level
# statements (selective move; defs stay module-level), top-level return as
# exit code (7) and bare return (0) - VM vs AOT byte-identical + equal exit
# codes. Negatives: script/main mutual exclusion (E2007), importing a script
# module rejected (E1007). ASCII only: PS 5.1 parses BOM-less ps1 as ANSI.
foreach ($n in @('http_proxy', 'https_proxy', 'no_proxy')) {
    $up = $n.ToUpperInvariant()
    if ([Environment]::GetEnvironmentVariable($n, 'Process') -ne $null -and
        [Environment]::GetEnvironmentVariable($up, 'Process') -ne $null) {
        [Environment]::SetEnvironmentVariable($n, $null, 'Process')
    }
}
# --- scg executable probe: repo build-rel FIRST (version-drift guard: PATH
# may carry a stale installed compiler that lacks current diagnostics), then
# PATH as fallback (sanity: --help mentions scourge) --------------------------
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = $null
try { $repoRoot = git -C $here rev-parse --show-toplevel 2>$null } catch {}
if (-not $repoRoot) {
    $p = $here
    while ($p -and -not (Test-Path (Join-Path $p 'CMakeLists.txt'))) {
        $parent = Split-Path $p -Parent
        if (-not $parent -or $parent -eq $p) { break }
        $p = $parent
    }
    $repoRoot = $p
}
$exe = $null
$repoExe = Join-Path $repoRoot 'build-rel\scg.exe'
if (Test-Path $repoExe) {
    $exe = $repoExe
} else {
    $cmd = Get-Command scg -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) {
        $helpArr = & $cmd.Source --help 2>&1
        $helpTxt = ($helpArr -join "`n")
        if ($LASTEXITCODE -eq 0 -and $helpTxt -match 'scourge') {
            $exe = $cmd.Source
        }
    }
}
if (-not $exe -or -not (Test-Path $exe)) { throw "scg not found: $repoExe does not exist and no Scourge scg in PATH" }
# ---------------------------------------------------------------------------
$dir = Join-Path $repoRoot 'build-rel\verify\script'
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

function Clean-Artifacts([string]$stem) {
    try { Remove-Item -Path (Join-Path $dir "$stem.*") -Force -ErrorAction SilentlyContinue } catch {}
    try { Remove-Item -Path (Join-Path $dir "$stem.sgbuild") -Recurse -Force -ErrorAction SilentlyContinue } catch {}
}

# --- positives: run vs build+aot triple diff (stdout/stderr byte-identical,
# exit codes equal - includes non-zero exit codes for top-level return) -----
$pos = @('probe_script_hello', 'probe_script_full', 'probe_script_return', 'probe_script_bare')
foreach ($n in $pos) {
    Clean-Artifacts $n
    $src = Join-Path $here "$n.sg"
    $p1 = Start-Process -FilePath $exe -ArgumentList @('run', $src) -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.run.out" -RedirectStandardError "$dir\$n.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @('build', $src, '-o', "$dir\$n.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.build.out" -RedirectStandardError "$dir\$n.build.err"
    if ($p2.ExitCode -ne 0) {
        Write-Host "$n BUILD-FAIL"; Get-Content "$dir\$n.build.err" -First 20; $fail = 1; continue
    }
    $p3 = Start-Process -FilePath "$dir\$n.exe" -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.aot.out" -RedirectStandardError "$dir\$n.aot.err"
    $o = ((Get-Content "$dir\$n.run.out") -join "`n") -eq ((Get-Content "$dir\$n.aot.out") -join "`n")
    $e = (Get-FileHash "$dir\$n.run.err").Hash -eq (Get-FileHash "$dir\$n.aot.err").Hash
    $x = $p1.ExitCode -eq $p3.ExitCode
    if ($o -and $e -and $x) {
        Write-Host "$n IDENTICAL (exit $($p1.ExitCode))"
    } else {
        Write-Host "$n DIFFER out=$o err=$e exit=$($p1.ExitCode)/$($p3.ExitCode)"
        if (-not $o) { Write-Host '--- run:'; Get-Content "$dir\$n.run.out"; Write-Host '--- aot:'; Get-Content "$dir\$n.aot.out" }
        if (-not $e) { Write-Host '--- run err:'; Get-Content "$dir\$n.run.err" -First 3; Write-Host '--- aot err:'; Get-Content "$dir\$n.aot.err" -First 3 }
        $fail = 1
    }
}

# --- negatives: static rejection on both channels ---------------------------
# run stderr carries a mode-named abort trailer; the diagnostic code itself
# is the shared contract (E2007 mutual exclusion / E1007 script import).
$negs = @(
    @{ name = 'neg_script_main'; src = Join-Path $here 'neg_script_main.sg'; code = 'E2007' },
    @{ name = 'negimp_main';     src = Join-Path $here 'negimp\main.sg';     code = 'E1007' }
)
foreach ($g in $negs) {
    $n = $g.name
    Clean-Artifacts $n
    $p1 = Start-Process -FilePath $exe -ArgumentList @('run', $g.src) -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.run.out" -RedirectStandardError "$dir\$n.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @('build', $g.src, '-o', "$dir\$n.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.build.out" -RedirectStandardError "$dir\$n.build.err"
    $rErr = (Get-Content "$dir\$n.run.err" -Raw) -match $g.code
    $bErr = (Get-Content "$dir\$n.build.err" -Raw) -match $g.code
    if ($p1.ExitCode -eq 1 -and $p2.ExitCode -eq 1 -and $rErr -and $bErr) {
        Write-Host "$n REJECTED ($($g.code) both channels)"
    } else {
        Write-Host "$n MISMATCH run=$($p1.ExitCode) build=$($p2.ExitCode) runHas=$rErr buildHas=$bErr"
        Get-Content "$dir\$n.run.err" -First 4
        $fail = 1
    }
}

if ($fail -eq 0) { Write-Host 'SCRIPT PASS' } else { exit 1 }
