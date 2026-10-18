# Stdlib parity verification (v0.9.3 new builtins: trig / rounding / rng / sorted).
# ASCII-only script (PS 5.1 safety); probe sources copied from verify\stdlib.
# Checks:
#   P1: stdlib_math.sg - VM (run) vs AOT (build) stdout byte-identical (SHA256), exit 0 both.
#   P2: stdlib_panic.sg - VM vs AOT stderr byte-identical (SHA256), exit 1 both
#       (floor(1e100) => "floor() result out of int64 range" panic, byte-same message).
#   P3: scg test math2.sg - all explicit tests pass (exit 0, ok=true).
$ErrorActionPreference = 'Stop'
$root = 'd:\Scourge\build-rel\verify\stdlib'
$srcDir = 'd:\Scourge\verify\stdlib'
if (Test-Path $root) { Remove-Item -Recurse -Force $root }
New-Item -ItemType Directory -Force -Path $root | Out-Null
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
$utf8nb = New-Object System.Text.UTF8Encoding($false)

Copy-Item (Join-Path $srcDir 'stdlib_math.sg') $root
Copy-Item (Join-Path $srcDir 'stdlib_panic.sg') $root
Copy-Item (Join-Path $srcDir 'math2.sg') $root
Copy-Item (Join-Path $srcDir 'math2.sgt') $root

function Run-Proc([string[]]$procArgs, [string]$tag) {
    $outF = Join-Path $root ($tag + '.out')
    $errF = Join-Path $root ($tag + '.err')
    if (Test-Path $outF) { Remove-Item -Force $outF }
    if (Test-Path $errF) { Remove-Item -Force $errF }
    $p = Start-Process -FilePath $exe -ArgumentList $procArgs -RedirectStandardOutput $outF -RedirectStandardError $errF -Wait -PassThru -NoNewWindow
    return @{
        Exit = $p.ExitCode
        Out = $outF
        Err = $errF
    }
}

# Run a built AOT executable directly (not through scourge).
function Run-Exe([string]$exe, [string]$tag) {
    $outF = Join-Path $root ($tag + '.out')
    $errF = Join-Path $root ($tag + '.err')
    if (Test-Path $outF) { Remove-Item -Force $outF }
    if (Test-Path $errF) { Remove-Item -Force $errF }
    $p = Start-Process -FilePath $exe -RedirectStandardOutput $outF -RedirectStandardError $errF -Wait -PassThru -NoNewWindow
    return @{
        Exit = $p.ExitCode
        Out = $outF
        Err = $errF
    }
}

function Hash-Of([string]$f) { (Get-FileHash -Algorithm SHA256 $f).Hash }

$script:failed = 0
function Check([bool]$cond, [string]$name) {
    if ($cond) { Write-Output ("PASS " + $name) }
    else { $script:failed++; Write-Output ("FAIL " + $name) }
}

# ---- P1: math parity - VM vs AOT stdout byte-identical ----
$vm = Run-Proc @('run', (Join-Path $root 'stdlib_math.sg')) 'vm_math'
Check ($vm.Exit -eq 0) "P1 VM run exit 0 (got $($vm.Exit))"
if ($vm.Exit -ne 0) { Write-Output ("  stderr: " + [System.IO.File]::ReadAllText($vm.Err)) }
$build = Run-Proc @('build', (Join-Path $root 'stdlib_math.sg'), '-o', (Join-Path $root 'math_aot.exe')) 'build_math'
Check ($build.Exit -eq 0) "P1 AOT build exit 0 (got $($build.Exit))"
if ($build.Exit -ne 0) { Write-Output ("  stderr: " + [System.IO.File]::ReadAllText($build.Err)) }
if ($build.Exit -eq 0) {
    $aot = Run-Exe (Join-Path $root 'math_aot.exe') 'aot_math'
    Check ($aot.Exit -eq 0) "P1 AOT run exit 0 (got $($aot.Exit))"
    Check ((Hash-Of $vm.Out) -eq (Hash-Of $aot.Out)) 'P1 stdout parity (SHA256 identical)'
}

# ---- P2: panic parity - byte-identical stderr, exit 1 both ----
$vmp = Run-Proc @('run', (Join-Path $root 'stdlib_panic.sg')) 'vm_panic'
Check ($vmp.Exit -eq 1) "P2 VM panic exit 1 (got $($vmp.Exit))"
$buildp = Run-Proc @('build', (Join-Path $root 'stdlib_panic.sg'), '-o', (Join-Path $root 'panic_aot.exe')) 'build_panic'
if ($buildp.Exit -eq 0) {
    $aotp = Run-Exe (Join-Path $root 'panic_aot.exe') 'aot_panic'
    Check ($aotp.Exit -eq 1) "P2 AOT panic exit 1 (got $($aotp.Exit))"
    Check ((Hash-Of $vmp.Err) -eq (Hash-Of $aotp.Err)) 'P2 panic stderr parity (SHA256 identical)'
} else {
    Check $false "P2 AOT panic build exit 0 (got $($buildp.Exit))"
    Write-Output ("  stderr: " + [System.IO.File]::ReadAllText($buildp.Err))
}

# ---- P3: scg test math2.sg - all explicit tests pass ----
$t = Run-Proc @('test', '--json', (Join-Path $root 'math2.sg')) 'test_math2'
Check ($t.Exit -eq 0) "P3 test exit 0 (got $($t.Exit))"
$tjson = [System.IO.File]::ReadAllText($t.Out, [System.Text.Encoding]::UTF8).Replace([string][char]13, '')
Check ($tjson.Contains('"ok": true')) 'P3 ok=true'
Check ($tjson.Contains('"testFailures": 0')) 'P3 testFailures=0'
Check ($tjson.Contains('"propertyFailures": 0')) 'P3 propertyFailures=0'
foreach ($n in @('test_trig_values', 'test_round_half_even', 'test_floor_ceil_trunc_float',
                 'test_rng_seed_reproducible', 'test_sorted_str', 'test_sorted_bool')) {
    $key = '"name": "' + $n + '"'
    $start = $tjson.IndexOf($key)
    if ($start -lt 0) { Check $false ("P3 test present: " + $n); continue }
    $next = $tjson.IndexOf('"name":', $start + $key.Length)
    if ($next -lt 0) { $next = $tjson.Length }
    Check ($tjson.Substring($start, $next - $start).Contains('"status": "pass"')) ("P3 test pass: " + $n)
}

if ($script:failed -gt 0) {
    Write-Output ("VERIFY STDLIB FAILED: " + $script:failed + " check(s)")
    exit 1
}
Write-Output 'ALL PASS'
