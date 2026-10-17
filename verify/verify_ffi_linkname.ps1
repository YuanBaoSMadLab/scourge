# Regression: @extern symbol= link alias (dual-mode byte check).
# Probes: verify/probes/probe_ffi_linkname.sg        (positive: alias -> pow/strlen)
#         verify/probes/probe_ffi_linkname_panic.sg  (negative: missing symbol)
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
# Env note: drop lowercase proxy duplicates (case-insensitive .NET dict).
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

foreach ($sg in @("d:\Scourge\verify\probes\probe_ffi_linkname.sg",
                  "d:\Scourge\verify\probes\probe_ffi_linkname_panic.sg")) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($sg)
    Get-ChildItem "$dir" -Filter "$stem.*" -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -Confirm:$false -ErrorAction SilentlyContinue

    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", $sg) -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.run.out" -RedirectStandardError "$dir\$stem.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $sg, "-o", "$dir\$stem.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.build.out" -RedirectStandardError "$dir\$stem.build.err"
    if ($p2.ExitCode -ne 0) {
        Write-Host "$stem BUILD-FAIL"; Get-Content "$dir\$stem.build.err" -First 20; exit 1
    }
    $p3 = Start-Process -FilePath "$dir\$stem.exe" -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$stem.aot.out" -RedirectStandardError "$dir\$stem.aot.err"

    $r1 = Get-Content "$dir\$stem.run.out"
    $r3 = Get-Content "$dir\$stem.aot.out"
    $o = (($r1 -join "`n") -eq ($r3 -join "`n"))
    $e = (Get-FileHash "$dir\$stem.run.err").Hash -eq (Get-FileHash "$dir\$stem.aot.err").Hash
    $x = $p1.ExitCode -eq $p3.ExitCode
    if ($o -and $e -and $x) {
        Write-Host "$stem IDENTICAL (exit $($p1.ExitCode))"
    } else {
        Write-Host "$stem DIFF (out:$o err:$e exit:$x)"
        Get-Content "$dir\$stem.run.out","$dir\$stem.aot.out","$dir\$stem.run.err","$dir\$stem.aot.err" -ErrorAction SilentlyContinue
        $fail++
    }
}

# content assertions: positive values + panic message carries the alias
$pos = Get-Content "$dir\probe_ffi_linkname.run.out" -Raw
if ($pos -match "1024\.0" -and $pos -match "True" -and $pos -match "15" -and $pos -match "2\.5") {
    Write-Host "probe_ffi_linkname CONTENT PASS"
} else { Write-Host "probe_ffi_linkname CONTENT FAIL"; $fail++ }

$neg = Get-Content "$dir\probe_ffi_linkname_panic.run.err" -Raw
if ($neg -match "msvcrt\.dll!no_such_symbol_xyz_981") {
    Write-Host "probe_ffi_linkname_panic ALIAS-IN-MSG PASS"
} else { Write-Host "probe_ffi_linkname_panic ALIAS-IN-MSG FAIL"; $fail++ }

if ($fail -gt 0) { Write-Host "LINKNAME FAILED: $fail"; exit 1 }
Write-Host "LINKNAME ALL PASS"
