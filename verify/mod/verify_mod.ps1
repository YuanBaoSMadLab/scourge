# 0.9.6 L-section (modules & packages) dual-mode verification. ASCII only.
# Layout convention: dotted module a.b -> a/b.sg; local package cache
# <root>/<pkg>@<ver>/... with numeric-aware version pick (1.10 > 1.9).
# Positive probes: run vs AOT build byte-identical (stdout/stderr/exit).
# Negative probes: run vs build identical check diagnostics (E5001/E5003).
$ErrorActionPreference = 'Stop'
$src = 'd:\Scourge\verify\mod\probes'
$pkgSrc = 'd:\Scourge\verify\mod\packages'
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
$work = 'd:\Scourge\build-rel\verify\mod'
$script:fail = 0

if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $work | Out-Null
Copy-Item -Recurse -Force (Join-Path $src '*') $work
$pkgRoot = Join-Path $work 'packages'
New-Item -ItemType Directory -Force $pkgRoot | Out-Null
Copy-Item -Recurse -Force (Join-Path $pkgSrc '*') $pkgRoot
$env:SCOURGE_PACKAGES = $pkgRoot

function Run-Mode([string]$mode, [string]$file, [string]$tag) {
    $outF = Join-Path $work ($tag + '.' + $mode + '.out')
    $errF = Join-Path $work ($tag + '.' + $mode + '.err')
    $argList = @($mode, $file)
    if ($mode -eq 'build') { $argList += @('-o', (Join-Path $work ($tag + '.exe'))) }
    $p = Start-Process -FilePath $exe -ArgumentList $argList -NoNewWindow -Wait -PassThru `
         -WorkingDirectory $work -RedirectStandardOutput $outF -RedirectStandardError $errF
    return @{ Exit = $p.ExitCode; OutF = $outF; ErrF = $errF }
}

# Diagnostic-only stderr: drivers append mode-specific trailer lines after the
# diagnostics; compiler diagnostics are exactly the lines containing error[.
function Diag-Of([string]$f) {
    @(Get-Content $f) | Where-Object { $_ -match 'error\[' }
}

function Dump-Out([string]$f) { Get-Content $f | ForEach-Object { Write-Host ('  ' + $_) } }

function Check-Dual([string]$file, [string]$tag, [bool]$expectOk) {
    $r = Run-Mode 'run' $file $tag
    $b = Run-Mode 'build' $file $tag
    if ($expectOk) {
        if ($b.Exit -ne 0) {
            Write-Host ($tag + ' BUILD-FAIL (run exit ' + $r.Exit + ')')
            Get-Content $b.ErrF | ForEach-Object { Write-Host ('  ' + $_) }
            $script:fail++
            return
        }
        $p3 = Start-Process -FilePath (Join-Path $work ($tag + '.exe')) -NoNewWindow -Wait -PassThru `
              -WorkingDirectory $work `
              -RedirectStandardOutput (Join-Path $work ($tag + '.aot.out')) `
              -RedirectStandardError (Join-Path $work ($tag + '.aot.err'))
        $o = (Get-FileHash $r.OutF).Hash -eq (Get-FileHash (Join-Path $work ($tag + '.aot.out'))).Hash
        $e = (Get-FileHash $r.ErrF).Hash -eq (Get-FileHash (Join-Path $work ($tag + '.aot.err'))).Hash
        $x = $r.Exit -eq $p3.ExitCode
        if ($o -and $e -and $x) {
            Write-Host ($tag + ' IDENTICAL (exit ' + $r.Exit + ')')
        } else {
            Write-Host ($tag + ' DIFFER out=' + $o + ' err=' + $e + ' exit=' + $r.Exit + '/' + $p3.ExitCode)
            $script:fail++
            Write-Host '--- run stdout:'; Dump-Out $r.OutF
            Write-Host '--- aot stdout:'; Dump-Out (Join-Path $work ($tag + '.aot.out'))
            Write-Host '--- run stderr:'; Dump-Out $r.ErrF
            Write-Host '--- aot stderr:'; Dump-Out (Join-Path $work ($tag + '.aot.err'))
        }
    } else {
        if ($r.Exit -eq 0 -or $b.Exit -eq 0) {
            Write-Host ($tag + ' UNEXPECTED-PASS (run=' + $r.Exit + ' build=' + $b.Exit + ')')
            $script:fail++
            return
        }
        $o = (Get-FileHash $r.OutF).Hash -eq (Get-FileHash $b.OutF).Hash
        $e = ((Diag-Of $r.ErrF) -join "`n") -eq ((Diag-Of $b.ErrF) -join "`n")
        $x = $r.Exit -eq $b.Exit
        if ($o -and $e -and $x) {
            Write-Host ($tag + ' NEG-IDENTICAL (exit ' + $r.Exit + ')')
        } else {
            Write-Host ($tag + ' NEG-DIFFER out=' + $o + ' err=' + $e + ' exit=' + $r.Exit + '/' + $b.Exit)
            $script:fail++
            Write-Host '--- run stderr:'; Dump-Out $r.ErrF
            Write-Host '--- build stderr:'; Dump-Out $b.ErrF
        }
    }
}

# ---- dual-mode byte-identical checks ----
Check-Dual (Join-Path $work 'main.sg') 'main' $true
Check-Dual (Join-Path $work 'pkgmain.sg') 'pkgmain' $true
Check-Dual (Join-Path $work 'vermain.sg') 'vermain' $true
Check-Dual (Join-Path $work 'badprivate.sg') 'badprivate' $false
Check-Dual (Join-Path $work 'badfromprivate.sg') 'badfromprivate' $false
Check-Dual (Join-Path $work 'badinternal.sg') 'badinternal' $false
Check-Dual (Join-Path $work 'badmissing.sg') 'badmissing' $false

# ---- exact expected stdout (positive probes) ----
$expectMain = @('3','5','12','100','10','10','30','100','11','42','100',
                'sub-c','tag:sub-c','tag:sub-c')
$gotMain = @(Get-Content (Join-Path $work 'main.run.out')) -replace "`r", ''
if (($gotMain -join "`n") -ne ($expectMain -join "`n")) {
    Write-Host 'main EXPECTED-STDOUT MISMATCH'
    Write-Host '--- expected:'; $expectMain | ForEach-Object { Write-Host ('  ' + $_) }
    Write-Host '--- got:'; $gotMain | ForEach-Object { Write-Host ('  ' + $_) }
    $script:fail++
} else { Write-Host 'main EXPECTED-STDOUT OK' }

$gotPkg = @(Get-Content (Join-Path $work 'pkgmain.run.out')) -replace "`r", ''
if (($gotPkg -join "`n") -ne '15') { Write-Host 'pkgmain EXPECTED-STDOUT MISMATCH'; $script:fail++ }
else { Write-Host 'pkgmain EXPECTED-STDOUT OK' }

$gotVer = @(Get-Content (Join-Path $work 'vermain.run.out')) -replace "`r", ''
if (($gotVer -join "`n") -ne '1.10') {
    Write-Host 'vermain EXPECTED-STDOUT MISMATCH (package version pick)'
    $gotVer | ForEach-Object { Write-Host ('  ' + $_) }
    $script:fail++
} else { Write-Host 'vermain EXPECTED-STDOUT OK (1.10 picked)' }

# ---- expected diagnostic codes on stderr (negative probes) ----
foreach ($t in @('badprivate', 'badfromprivate', 'badinternal')) {
    $err = Get-Content (Join-Path $work ($t + '.run.err')) -Raw
    if (-not $err.Contains('E5003')) { Write-Host ($t + ' missing E5003'); $script:fail++ }
    else { Write-Host ($t + ' E5003 OK') }
}
$err = Get-Content (Join-Path $work 'badmissing.run.err') -Raw
if (-not $err.Contains('E5001')) { Write-Host 'badmissing missing E5001'; $script:fail++ }
else { Write-Host 'badmissing E5001 OK' }

# ---- interface contract: @private must not be exported (L7) ----
$p = Start-Process -FilePath $exe -ArgumentList @('interface', '--out', '-', (Join-Path $work 'a\b.sg')) `
     -NoNewWindow -Wait -PassThru -WorkingDirectory $work `
     -RedirectStandardOutput (Join-Path $work 'sgi.out') `
     -RedirectStandardError (Join-Path $work 'sgi.err')
$sgi = Get-Content (Join-Path $work 'sgi.out') -Raw
if ($p.ExitCode -ne 0) {
    Write-Host 'interface a/b.sg FAIL'; $script:fail++
    Get-Content (Join-Path $work 'sgi.err') | ForEach-Object { Write-Host ('  ' + $_) }
} elseif ($sgi.Contains('secret')) {
    Write-Host 'interface LEAKS @private secret'; $script:fail++
} elseif (-not ($sgi.Contains('def add') -and $sgi.Contains('def mul'))) {
    Write-Host 'interface missing public funcs'; $script:fail++
} else { Write-Host 'interface PRIVATE-HIDDEN OK' }

Remove-Item Env:\SCOURGE_PACKAGES -ErrorAction SilentlyContinue
if ($script:fail -gt 0) {
    Write-Host ('VERIFY MOD FAILED: ' + $script:fail + ' check(s)')
    exit 1
}
Write-Host 'ALL PASS'
