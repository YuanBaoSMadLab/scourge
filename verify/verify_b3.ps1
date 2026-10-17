# Regression: 0.9.10 batch-3 (Option semantics / Tuple per-slot annotation
# matching / getenv prescription / async AOT passthrough) dual-mode check.
# Probes: verify/probes/probe_099_*.sg (superset incl. batch-1/2 probes).
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
foreach ($n in @('http_proxy', 'https_proxy', 'no_proxy')) {
    $up = $n.ToUpperInvariant()
    if ([Environment]::GetEnvironmentVariable($n, 'Process') -ne $null -and
        [Environment]::GetEnvironmentVariable($up, 'Process') -ne $null) {
        [Environment]::SetEnvironmentVariable($n, $null, 'Process')
    }
}
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

# ---- Part 1: dual-mode byte check on all 099 probes (b1/b2 superset) ----
$files = Get-ChildItem "d:\Scourge\verify\probes\probe_099_*.sg" |
         ForEach-Object { $_.FullName }
foreach ($sg in $files) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($sg)
    Remove-Item "$dir\$stem.*" -Recurse -Force -ErrorAction SilentlyContinue
    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", $sg) -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    if ($p2.ExitCode -ne 0) { Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err"; $fail++; continue }
    $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"
    $r1 = (Get-Content "$dir\$stem.run.out") | Where-Object { $_ -notmatch 'elapsed_ms' }
    $r3 = (Get-Content "$dir\$stem.aot.out") | Where-Object { $_ -notmatch 'elapsed_ms' }
    $o = (($r1 -join "`n") -eq ($r3 -join "`n"))
    $e = (Get-FileHash "$dir\$stem.run.err").Hash -eq (Get-FileHash "$dir\$stem.aot.err").Hash
    $x = $p1.ExitCode -eq $p3.ExitCode
    if ($o -and $e -and $x) { Write-Host "$stem IDENTICAL (exit $($p1.ExitCode))" }
    else {
        Write-Host "$stem DIFFER out=$o err=$e exit=$($p1.ExitCode)/$($p3.ExitCode)"
        $fail++
        if (-not $o) { Write-Host "--- run:"; $r1 | Select-Object -First 8; Write-Host "--- aot:"; $r3 | Select-Object -First 8 }
        if (-not $e) { Write-Host "--- run err:"; Get-Content "$dir\$stem.run.err" -First 3; Write-Host "--- aot err:"; Get-Content "$dir\$stem.aot.err" -First 3 }
    }
}

# ---- Part 2: batch-3 diagnostic prescriptions (compile-fail message checks) ----
$neg = @(
    @{ name = "neg_tuple_mismatch"; code = 'def main() -> int:`n    t: (int, str) = ("a", 1)`n    return 0'; pat = 'E2001' },
    @{ name = "neg_option_nested";  code = 'def main() -> int:`n    x: Option[Option[int]] = None`n    return 0'; pat = 'Option' },
    @{ name = "neg_option_type";    code = 'def main() -> int:`n    x: Option[int] = "hi"`n    return 0'; pat = 'ok\(\)' },
    @{ name = "neg_getenv";         code = 'capability ffi`n`n@extern("msvcrt.dll")`ndef getenv(name: str) -> str:`n    pass`n`ndef main() -> int:`n    return 0'; pat = 'E1007' },
    @{ name = "neg_option_discard"; code = 'def f() -> Option[int]:`n    return None`n`ndef main() -> int:`n    f()`n    return 0'; pat = 'E2005' },
    @{ name = "neg_await_top";      code = 'def f() -> int:`n    return await 1`n`ndef main() -> int:`n    return 0'; pat = 'E2010' }
)
foreach ($t in $neg) {
    $sg = "$dir\$($t.name).sg"
    $code = $t.code -replace '`n', "`n"
    [IO.File]::WriteAllText($sg, $code)
    $p = Start-Process -FilePath $exe -ArgumentList @("check", $sg) -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput "$dir\$($t.name).out" -RedirectStandardError "$dir\$($t.name).err"
    $txt = (Get-Content "$dir\$($t.name).err" -Raw) + (Get-Content "$dir\$($t.name).out" -Raw)
    if ($p.ExitCode -ne 0 -and $txt -match $t.pat) { Write-Host "$($t.name) prescription OK" }
    else { Write-Host "$($t.name) FAIL (exit $($p.ExitCode))"; Write-Host $txt; $fail++ }
}

if ($fail -eq 0) { Write-Host "B3 ALL PASS" } else { Write-Host "$fail FAILED"; exit 1 }
