# Regression + perf: str self-concat in-place growth (str_concat_grow).
# Semantics: += loop, s = s + b loop, local alias, self-append (memmove
# overlap), alias-marking channels (param bind / list / dict / set / tuple /
# Result / return-local), if-else dual emit sites, call-in-RHS ordering,
# RHS panic leaves lhs untouched. Triple diff (VM run vs AOT exe) must be
# byte-identical on stdout/stderr/exit.
# Perf: 40000/80000 accumulation, VM run vs AOT exe wall time report.
# ASCII only: Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
foreach ($n in @('http_proxy', 'https_proxy', 'no_proxy')) {
    $up = $n.ToUpperInvariant()
    if ([Environment]::GetEnvironmentVariable($n, 'Process') -ne $null -and
        [Environment]::GetEnvironmentVariable($up, 'Process') -ne $null) {
        [Environment]::SetEnvironmentVariable($n, $null, 'Process')
    }
}
$exe = "d:\Scourge\build\scourge.exe"
$dir = "d:\Scourge\build\verify\str_grow"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

function Clean-Artifacts([string]$stem) {
    try { Remove-Item -Path "$dir\$stem.*" -Force -ErrorAction SilentlyContinue } catch {}
}

# --- semantics: run vs build+aot triple diff (byte identical) --------------
$pos = @("probe_grow_basic", "probe_grow_alias", "probe_grow_flow",
         "probe_grow_loop")
foreach ($n in $pos) {
    Clean-Artifacts $n

    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", "d:\Scourge\codebuddytest\$n.sg") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.run.out" -RedirectStandardError "$dir\$n.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", "d:\Scourge\codebuddytest\$n.sg", "-o", "$dir\$n.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.build.out" -RedirectStandardError "$dir\$n.build.err"
    if ($p2.ExitCode -ne 0) {
        Write-Host "$n BUILD-FAIL"; Get-Content "$dir\$n.build.err" -First 20; $fail = 1; continue
    }
    $p3 = Start-Process -FilePath "$dir\$n.exe" -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.aot.out" -RedirectStandardError "$dir\$n.aot.err"

    $r1 = Get-Content "$dir\$n.run.out"
    $r3 = Get-Content "$dir\$n.aot.out"
    $o = (($r1 -join "`n") -ceq ($r3 -join "`n"))
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

# --- perf: VM run vs AOT exe wall time (report only, no assertion) ---------
# 20 x 40000 accumulations (=800k appends) so the loop dominates the ~13ms
# process-spawn floor of direct invocation. Old AOT (bump-per-concat, no
# grow) measured ~1046ms for a SINGLE 40000 loop; the grow path must keep
# the loop O(n) - watch that aot stays far below vm.
$src = "d:\Scourge\codebuddytest\probe_grow_bench.sg"
Set-Content -Path $src -Encoding Ascii -Value @(
    'def main() -> int:',
    '    s = ""',
    '    for r in range(20):',
    '        s = ""',
    '        for i in range(40000):',
    '            s += "x"',
    '    print(len(s))',
    '    return 0')
Clean-Artifacts "bench"
$p2 = Start-Process -FilePath $exe -ArgumentList @("build", $src, "-o", "$dir\bench.exe") -NoNewWindow -Wait -PassThru `
      -RedirectStandardOutput "$dir\bench.build.out" -RedirectStandardError "$dir\bench.build.err"
if ($p2.ExitCode -ne 0) { Write-Host "bench BUILD-FAIL"; Get-Content "$dir\bench.build.err" -First 10; $fail = 1 }
else {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $o1 = & $exe run $src 2>&1
    $sw.Stop()
    $tv = $sw.ElapsedMilliseconds
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $o3 = & "$dir\bench.exe" 2>&1
    $sw.Stop()
    $ta = $sw.ElapsedMilliseconds
    $ok = (($o1 | Out-String) -ceq ($o3 | Out-String))
    $ratio = [math]::Round($tv / [math]::Max($ta, 1), 2)
    if ($ok) { Write-Host ("bench IDENTICAL  vm={0}ms aot={1}ms  vm/aot=x{2}" -f $tv, $ta, $ratio) }
    else { Write-Host "bench OUTPUT-DIFF vm=[$($o1)] aot=[$($o3)]"; $fail = 1 }
}

if ($fail -eq 0) { Write-Host "STRGROW PASS" } else { exit 1 }
