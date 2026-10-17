# Regression: O-section FFI wide-type domain dual-mode byte check.
# Positives: verify/probes/probe_o_{e,a2,a3,a,smoke,b,c,d,min,max20}.sg
#   variadic @extern (int/float/str args, 0-arg and 20-arg boundaries)
#   + data_ptr (tensor memset roundtrip / str strlen / readback).
# Sema negatives: probe_o_{neg_var,neg_list,neg21}.sg - both channels must
#   exit 1 with byte-identical stderr (diagnostic is the contract).
# Note: runtime empty-tensor panic (sg_data_ptr guard) is unreachable from
#   source (no construct yields an empty tensor) - defensive only, see docs.
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
$dir = "d:\Scourge\build-rel\verify"
New-Item -ItemType Directory -Force $dir | Out-Null
$fail = 0

# Cleanup helper: PS 5.1 pipeline Remove-Item intermittently NREs on
# redirected files - direct wildcard delete guarded by try/catch instead.
function Clean-Artifacts([string]$stem) {
    try { Remove-Item -Path "$dir\$stem.*" -Recurse -Force -Confirm:$false -ErrorAction SilentlyContinue } catch {}
}

# --- positives: run vs build+aot triple diff (byte identical) -------------
$pos = @("probe_o_e", "probe_o_a2", "probe_o_a3", "probe_o_a",
         "probe_o_smoke", "probe_o_b", "probe_o_c", "probe_o_d",
         "probe_o_min", "probe_o_max20")
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

# --- sema negatives: both channels exit 1, expected code in both stderr ----
# Note: run stderr carries a mode-named abort trailer ("run abort: ..."),
# build stderr does not - the diagnostic block itself is the shared contract,
# so byte-hash equality does not hold across channels here.
$neg = @("probe_o_neg_var", "probe_o_neg_list", "probe_o_neg21")
$negCode = @{ "probe_o_neg_var" = "E1007"; "probe_o_neg_list" = "E2001"; "probe_o_neg21" = "E2001" }
foreach ($n in $neg) {
    Clean-Artifacts $n

    $p1 = Start-Process -FilePath $exe -ArgumentList @("run", "d:\Scourge\verify\probes\$n.sg") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.run.out" -RedirectStandardError "$dir\$n.run.err"
    $p2 = Start-Process -FilePath $exe -ArgumentList @("build", "d:\Scourge\verify\probes\$n.sg", "-o", "$dir\$n.exe") -NoNewWindow -Wait -PassThru `
          -RedirectStandardOutput "$dir\$n.build.out" -RedirectStandardError "$dir\$n.build.err"
    $code = $negCode[$n]
    $rErr = (Get-Content "$dir\$n.run.err" -Raw) -match $code
    $bErr = (Get-Content "$dir\$n.build.err" -Raw) -match $code
    if ($p1.ExitCode -eq 1 -and $p2.ExitCode -eq 1 -and $rErr -and $bErr) {
        Write-Host "$n REJECTED ($code both channels)"
    } else {
        Write-Host "$n MISMATCH run=$($p1.ExitCode) build=$($p2.ExitCode) runHas=$rErr buildHas=$bErr"
        Get-Content "$dir\$n.run.err" -First 4
        $fail = 1
    }
}

if ($fail -eq 0) { Write-Host "O-WIDE PASS" } else { exit 1 }
