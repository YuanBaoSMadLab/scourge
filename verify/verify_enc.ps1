# Encoding adaptation verification (Task B).
# ASCII-only script: all Chinese text built from code points (PS 5.1 safety).
# Groups:
#   T1 (GBK-encodable Chinese) in 6 encodings/newline variants -> identical stdout
#   T2 (non-BMP emoji, GBK cannot encode) in 3 encodings       -> identical stdout
#   AOT: build one T1 probe, run exe, stdout identical to run mode
$ErrorActionPreference = 'Stop'
$root = 'd:\Scourge\build-rel\verify\enc'
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

$t1 = -join @([char]0x4F60, [char]0x597D, [char]0x2C, [char]0x4E16, [char]0x754C, [char]0x41, [char]0x42, [char]0x43)
$t2 = [string][char]0x4F60 + [string][char]0x597D + [string][char]0xD83C + [string][char]0xDF89 + [string][char]0x21

$utf8nb = New-Object System.Text.UTF8Encoding($false)
$utf8b  = New-Object System.Text.UTF8Encoding($true)
$u16le  = New-Object System.Text.UnicodeEncoding($false, $true)
$u16be  = [System.Text.Encoding]::BigEndianUnicode
$gbk    = [System.Text.Encoding]::GetEncoding(936)

function Write-Probe([string]$name, [string]$text, [System.Text.Encoding]$enc, [bool]$crlf) {
    $nl = "`n"
    if ($crlf) { $nl = "`r`n" }
    $src = 'def main() -> int:' + $nl + '    print("' + $text + '")' + $nl + '    return 0' + $nl
    $p = Join-Path $root ($name + '.sg')
    [System.IO.File]::WriteAllText($p, $src, $enc)
    return $p
}

function Run-Probe([string]$sg, [string]$tag) {
    $outF = Join-Path $root ($tag + '.out')
    $errF = Join-Path $root ($tag + '.err')
    if (Test-Path $outF) { Remove-Item -Force $outF }
    if (Test-Path $errF) { Remove-Item -Force $errF }
    $p = Start-Process -FilePath $exe -ArgumentList @('run', '--no-cache', $sg) -RedirectStandardOutput $outF -RedirectStandardError $errF -Wait -PassThru -NoNewWindow
    if ($p.ExitCode -ne 0) {
        Write-Output ("FAIL run exit " + $tag + " code=" + $p.ExitCode)
        Get-Content $errF | ForEach-Object { Write-Output $_ }
        exit 1
    }
    return $outF
}

function Hash-Of([string]$f) { (Get-FileHash -Algorithm SHA256 $f).Hash }

function Compare-Group([string]$groupName, [string[]]$files) {
    $base = Hash-Of $files[0]
    foreach ($f in $files[1..($files.Count - 1)]) {
        if ((Hash-Of $f) -ne $base) {
            Write-Output ("FAIL " + $groupName + ": output bytes differ")
            exit 1
        }
    }
    Write-Output ("PASS " + $groupName + " (all " + $files.Count + " identical)")
}

# ---- Group 1: same logical text (GBK-encodable) across encodings/newlines ----
$f1 = Run-Probe (Write-Probe 'enc_utf8nb' $t1 $utf8nb $false) 'g1_utf8nb'
$f2 = Run-Probe (Write-Probe 'enc_utf8bom' $t1 $utf8b $false) 'g1_utf8bom'
$f3 = Run-Probe (Write-Probe 'enc_u16le' $t1 $u16le $false) 'g1_u16le'
$f4 = Run-Probe (Write-Probe 'enc_u16be' $t1 $u16be $false) 'g1_u16be'
$f5 = Run-Probe (Write-Probe 'enc_gbk' $t1 $gbk $false) 'g1_gbk'
$f6 = Run-Probe (Write-Probe 'enc_utf8nb_crlf' $t1 $utf8nb $true) 'g1_crlf'
Compare-Group 'T1: utf8nb/utf8bom/u16le/u16be/gbk/crlf' @($f1, $f2, $f3, $f4, $f5, $f6)

# ---- Group 2: non-BMP emoji (surrogate decode path) ----
$h1 = Run-Probe (Write-Probe 'enc2_utf8nb' $t2 $utf8nb $false) 'g2_utf8nb'
$h2 = Run-Probe (Write-Probe 'enc2_utf8bom' $t2 $utf8b $false) 'g2_utf8bom'
$h3 = Run-Probe (Write-Probe 'enc2_u16le' $t2 $u16le $false) 'g2_u16le'
Compare-Group 'T2: utf8nb/utf8bom/u16le (emoji U+1F389)' @($h1, $h2, $h3)

# ---- AOT: build + exe output identical to run mode ----
# Env note: drop lowercase proxy duplicates (case-insensitive .NET dict).
foreach ($n in @('http_proxy', 'https_proxy', 'no_proxy')) {
    $up = $n.ToUpperInvariant()
    if ([Environment]::GetEnvironmentVariable($n, 'Process') -ne $null -and
        [Environment]::GetEnvironmentVariable($up, 'Process') -ne $null) {
        [Environment]::SetEnvironmentVariable($n, $null, 'Process')
    }
}
$exe = Join-Path $root 'enc_aot.exe'
if (Test-Path $exe) { Remove-Item -Force $exe }
$exeOut = Join-Path $root 'aot.out'
$exeErr = Join-Path $root 'aot.err'
if (Test-Path $exeOut) { Remove-Item -Force $exeOut }
if (Test-Path $exeErr) { Remove-Item -Force $exeErr }
$sg = Join-Path $root 'enc_utf8bom.sg'
$p = Start-Process -FilePath $exe -ArgumentList @('build', $sg, '-o', $exe) -RedirectStandardOutput (Join-Path $root 'aot_build.out') -RedirectStandardError (Join-Path $root 'aot_build.err') -Wait -PassThru -NoNewWindow
if ($p.ExitCode -ne 0) {
    Write-Output "FAIL build exit"
    Get-Content (Join-Path $root 'aot_build.err') | ForEach-Object { Write-Output $_ }
    exit 1
}
$p = Start-Process -FilePath $exe -RedirectStandardOutput $exeOut -RedirectStandardError $exeErr -Wait -PassThru -NoNewWindow
if ($p.ExitCode -ne 0) {
    Write-Output ("FAIL aot exe exit code=" + $p.ExitCode)
    exit 1
}
if ((Hash-Of $exeOut) -ne (Hash-Of $f1)) {
    Write-Output "FAIL AOT vs run: output bytes differ"
    exit 1
}
Write-Output "PASS AOT exe output identical to run mode"

# Show actual decoded bytes for eyeball check.
Write-Output "--- stdout content (T1) ---"
[System.IO.File]::ReadAllBytes($f1) | ForEach-Object { $_.ToString('X2') } | Out-Null
$bytes = [System.IO.File]::ReadAllBytes($f1)
Write-Output ("bytes: " + (($bytes | ForEach-Object { $_.ToString('X2') }) -join ' '))
Write-Output 'ALL PASS'
