# Test framework verification (sg test, Task: property tests + .sgt runner).
# ASCII-only script (PS 5.1 safety); probe sources copied from verify\test.
# mathlib probes are INTENTIONALLY imperfect (they prove the oracle works):
#   explicit:  test_add_basic pass, test_add_wrong fail, test_oob_panic panic
#   property:  add/div/capped/scaled pass, fact fail (counterexample n=0,
#              ensures violation), lookup skip (Dict param not generated)
#   => exit code 1; ok.sg (all-correct contracts) must exit 0.
$ErrorActionPreference = 'Stop'
$root = 'd:\Scourge\build-rel\verify\test'
$srcDir = 'd:\Scourge\verify\test'
if (Test-Path $root) { Remove-Item -Recurse -Force $root }
New-Item -ItemType Directory -Force -Path $root | Out-Null
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
$utf8nb = New-Object System.Text.UTF8Encoding($false)

Copy-Item (Join-Path $srcDir 'mathlib.sg') $root
Copy-Item (Join-Path $srcDir 'mathlib.sgt') $root

# ok.sg: contracts all correct => whole command exits 0.
$okSg = @'
# ok.sg - all-pass property probe (positive exit code check).

@pure
def ident(x: int) -> int:
    """
    identity function probe.
    requires: x >= -1000000 and x <= 1000000
    ensures: result == x
    """
    return x
'@
[System.IO.File]::WriteAllText((Join-Path $root 'ok.sg'), $okSg, $utf8nb)

function Run-Test([string[]]$testArgs, [string]$tag) {
    $outF = Join-Path $root ($tag + '.out')
    $errF = Join-Path $root ($tag + '.err')
    if (Test-Path $outF) { Remove-Item -Force $outF }
    if (Test-Path $errF) { Remove-Item -Force $errF }
    $all = @('test') + $testArgs
    $p = Start-Process -FilePath $exe -ArgumentList $all -RedirectStandardOutput $outF -RedirectStandardError $errF -Wait -PassThru -NoNewWindow
    return @{
        Exit = $p.ExitCode
        Out = [System.IO.File]::ReadAllText($outF, [System.Text.Encoding]::UTF8)
        Err = [System.IO.File]::ReadAllText($errF, [System.Text.Encoding]::UTF8)
    }
}

function Hash-Of([string]$f) { (Get-FileHash -Algorithm SHA256 $f).Hash }

$script:failed = 0
function Check([bool]$cond, [string]$name) {
    if ($cond) { Write-Output ("PASS " + $name) }
    else { $script:failed++; Write-Output ("FAIL " + $name) }
}

# Window of one property object: from "func": "X" up to the next "func" key.
function Prop-Window([string]$json, [string]$func) {
    $key = '"func": "' + $func + '"'
    $start = $json.IndexOf($key)
    if ($start -lt 0) { return '' }
    $next = $json.IndexOf('"func":', $start + $key.Length)
    if ($next -lt 0) { $next = $json.Length }
    return $json.Substring($start, $next - $start)
}

# Window of one test object: from "name": "X" up to the next "name" key.
function Test-Window([string]$json, [string]$name) {
    $key = '"name": "' + $name + '"'
    $start = $json.IndexOf($key)
    if ($start -lt 0) { return '' }
    $next = $json.IndexOf('"name":', $start + $key.Length)
    if ($next -lt 0) { $next = $json.Length }
    return $json.Substring($start, $next - $start)
}

# ---- T1: full run (--json --runs 32) => exit 1, statuses as expected ----
$r1 = Run-Test @('--json', '--runs', '32', (Join-Path $root 'mathlib.sg')) 't1'
Check ($r1.Exit -eq 1) "T1 exit code 1 (got $($r1.Exit))"
if ($r1.Exit -ne 0 -and $r1.Err -ne '') { Write-Output ("  stderr: " + $r1.Err) }
$json = $r1.Out
Check ($json.Contains('"module": "mathlib"')) 'T1 module field'
Check ($json.Contains('"seed": null')) 'T1 default seed is null (derived)'

Check ((Test-Window $json 'test_add_basic').Contains('"status": "pass"')) 'T1 test_add_basic pass'
Check ((Test-Window $json 'test_add_wrong').Contains('"status": "fail"')) 'T1 test_add_wrong fail'
Check ((Test-Window $json 'test_oob_panic').Contains('"status": "panic"')) 'T1 test_oob_panic panic'

foreach ($f in @('add', 'div', 'capped', 'scaled')) {
    Check ((Prop-Window $json $f).Contains('"status": "pass"')) ("T1 prop " + $f + " pass")
}
$fw = Prop-Window $json 'fact'
Check ($fw.Contains('"status": "fail"')) 'T1 prop fact fail'
Check ($fw.Contains('"counterexample"')) 'T1 fact reports counterexample'
Check ($fw.Contains('"args"')) 'T1 fact counterexample has args'
Check ($fw.Contains('"0"')) 'T1 fact counterexample first arg is 0 (boundary pool)'
Check ($fw.Contains('"reason"')) 'T1 fact counterexample has reason'
$lw = Prop-Window $json 'lookup'
Check ($lw.Contains('"status": "skip"')) 'T1 prop lookup skip'
Check ($lw.Contains('"skipReason"')) 'T1 lookup skipReason present'

Check ($json.Contains('"tests": 3')) 'T1 summary tests=3'
Check ($json.Contains('"testFailures": 2')) 'T1 summary testFailures=2'
Check ($json.Contains('"properties": 6')) 'T1 summary properties=6'
Check ($json.Contains('"propertyFailures": 1')) 'T1 summary propertyFailures=1'
Check ($json.Contains('"ok": false')) 'T1 summary ok=false'

# ---- T2: determinism - same args twice => byte-identical JSON ----
$r2 = Run-Test @('--json', '--runs', '32', (Join-Path $root 'mathlib.sg')) 't2'
Check ((Hash-Of (Join-Path $root 't1.out')) -eq (Hash-Of (Join-Path $root 't2.out'))) 'T2 deterministic rerun (same bytes)'

# ---- T3: --runs / --seed take effect and stay deterministic ----
$r3a = Run-Test @('--json', '--seed', '7', '--runs', '4', (Join-Path $root 'mathlib.sg')) 't3a'
Check ($r3a.Exit -eq 1) "T3 exit code 1 (got $($r3a.Exit))"
Check ($r3a.Out.Contains('"seed": 7')) 'T3 explicit seed echoed'
Check ($r3a.Out.Contains('"runs": 4,')) 'T3 runs=4 in property results'
$r3b = Run-Test @('--json', '--seed', '7', '--runs', '4', (Join-Path $root 'mathlib.sg')) 't3b'
Check ((Hash-Of (Join-Path $root 't3a.out')) -eq (Hash-Of (Join-Path $root 't3b.out'))) 'T3 explicit-seed rerun identical'

# ---- T4: no .sgt sidecar => tests array empty, properties still run ----
$nosgt = Join-Path $root 'nosgt'
New-Item -ItemType Directory -Force -Path $nosgt | Out-Null
Copy-Item (Join-Path $root 'mathlib.sg') $nosgt
$r4 = Run-Test @('--json', '--runs', '8', (Join-Path $nosgt 'mathlib.sg')) 't4'
Check ($r4.Exit -eq 1) "T4 exit code 1 (got $($r4.Exit))"
# stdout is text-mode (LF -> CRLF); strip CR before newline-sensitive match.
$json4 = $r4.Out.Replace([string][char]13, '')
Check ($json4.Contains('"tests": [' + [char]10 + '  ]')) 'T4 empty tests array'
Check ($r4.Out.Contains('"testFailures": 0')) 'T4 testFailures=0'
Check ((Prop-Window $r4.Out 'fact').Contains('"status": "fail"')) 'T4 properties still verified'

# ---- T5: all-correct module (ok.sg) => exit 0 ----
$r5 = Run-Test @('--json', (Join-Path $root 'ok.sg')) 't5'
Check ($r5.Exit -eq 0) "T5 exit code 0 (got $($r5.Exit))"
Check ($r5.Out.Contains('"ok": true')) 'T5 ok=true'

# ---- T6: usage errors => exit 2 ----
$r6 = Run-Test @('--runs', '0', (Join-Path $root 'mathlib.sg')) 't6'
Check ($r6.Exit -eq 2) "T6 --runs 0 rejected with exit 2 (got $($r6.Exit))"

# ---- T7: text mode smoke (human report) ----
$r7 = Run-Test @((Join-Path $root 'mathlib.sg')) 't7'
Check ($r7.Exit -eq 1) "T7 text mode exit 1 (got $($r7.Exit))"

if ($script:failed -gt 0) {
    Write-Output ("VERIFY TEST FAILED: " + $script:failed + " check(s)")
    exit 1
}
Write-Output 'ALL PASS'
