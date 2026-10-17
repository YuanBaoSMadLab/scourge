# Regression: List.append scalar domains + list comprehension (0.9.8).
# append: int/float/bool/str domains, empty-list first-append etag fix,
# alias visibility (shared handle), int->float widening at element
# boundary. Comprehension: [e for x in iter (if c)?] over range/List/str,
# filter, empty result, len() composition.
# Sema negatives on both channels:
#   append type mismatch  -> E2001
#   append argument count -> E2001
#   dict comprehension    -> E2001 (iter source limited to range/List/str)
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
# Env note: drop lowercase proxy duplicates (case-insensitive .NET dict).
foreach ($n in @('http_proxy', 'https_proxy', 'no_proxy')) {
    $up = $n.ToUpperInvariant()
    if ([Environment]::GetEnvironmentVariable($n, 'Process') -ne $null -and
        [Environment]::GetEnvironmentVariable($up, 'Process') -ne $null) {
        [Environment]::SetEnvironmentVariable($n, $null, 'Process')
    }
}
$exe = "d:\Scourge\build-rel\scourge.exe"
$dir = "d:\Scourge\build-rel\verify\list"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

function Clean-Artifacts([string]$stem) {
    try { Remove-Item -Path "$dir\$stem.*" -Force -ErrorAction SilentlyContinue } catch {}
}

# --- positives: run vs build+aot triple diff (byte identical) -------------
$pos = @("probe_list_append", "probe_list_comp")
foreach ($n in $pos) {
    Clean-Artifacts $n

    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", "d:\Scourge\verify\probes\$n.sg") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.run.out" -RedirectStandardError "$dir\$n.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", "d:\Scourge\verify\probes\$n.sg", "-o", "$dir\$n.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.build.out" -RedirectStandardError "$dir\$n.build.err"
    if ($p2.ExitCode -ne 0) {
        Write-Host "$n BUILD-FAIL"; Get-Content "$dir\$n.build.err" -First 20; $fail = 1; continue
    }
    $p3 = Start-Process -FilePath "$dir\$n.exe" -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.aot.out" -RedirectStandardError "$dir\$n.aot.err"

    $r1 = Get-Content "$dir\$n.run.out"
    $r3 = Get-Content "$dir\$n.aot.out"
    $o = (($r1 -join "`n") -eq ($r3 -join "`n"))
    $e = (Get-FileHash "$dir\$n.run.err").Hash -eq (Get-FileHash "$dir\$n.aot.err").Hash
    $x = $p1.ExitCode -eq $p3.ExitCode
    if ($o -and $e -and $x) {
        Write-Host "$n IDENTICAL (exit $($p1.ExitCode))"
    } else {
        Write-Host "$n DIFFER out=$o err=$e exit=$($p1.ExitCode)/$($p3.ExitCode)"
        if (-not $o) { Write-Host "--- run:"; $r1; Write-Host "--- aot:"; $r3 }
        if (-not $e) { Write-Host "--- run err:"; Get-Content "$dir\$n.run.err" -First 3; Write-Host "--- aot err:"; Get-Content "$dir\$n.aot.err" -First 3 }
        $fail = 1
    }
}

# --- sema negatives: E2001 on both channels --------------------------------
# run stderr carries a mode-named abort trailer; build stderr does not -
# the diagnostic block itself is the shared contract (error code match).
$negs = @(
    @{ name = "probe_list_neg_type"; code = "E2001"; lines = @('def main() -> int:', '    xs: List[int] = [1]', '    xs.append("s")', '    return 0') },
    @{ name = "probe_list_neg_arity"; code = "E2001"; lines = @('def main() -> int:', '    xs: List[int] = [1]', '    xs.append(2, 3)', '    return 0') },
    @{ name = "probe_list_neg_dictsrc"; code = "E2001"; lines = @('def main() -> int:', '    d: Dict[str, int] = {"a": 1}', '    xs: List[int] = [v for v in d]', '    return 0') }
)
foreach ($neg in $negs) {
    $n = $neg.name; $ncode = $neg.code
    Clean-Artifacts $n
    $src = "d:\Scourge\verify\probes\$n.sg"
    Set-Content -Path $src -Encoding Ascii -Value $neg.lines
    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", $src) -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.run.out" -RedirectStandardError "$dir\$n.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", $src, "-o", "$dir\$n.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.build.out" -RedirectStandardError "$dir\$n.build.err"
    $rErr = (Get-Content "$dir\$n.run.err" -Raw) -match $ncode
    $bErr = (Get-Content "$dir\$n.build.err" -Raw) -match $ncode
    if ($p1.ExitCode -eq 1 -and $p2.ExitCode -eq 1 -and $rErr -and $bErr) {
        Write-Host "$n REJECTED ($ncode both channels)"
    } else {
        Write-Host "$n MISMATCH run=$($p1.ExitCode) build=$($p2.ExitCode) runHas=$rErr buildHas=$bErr"
        Get-Content "$dir\$n.run.err" -First 4
        $fail = 1
    }
}

if ($fail -eq 0) { Write-Host "LIST PASS" } else { exit 1 }
