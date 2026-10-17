# 构建并运行 02_web_server（可从任意目录调用）。
# 产物布局：最终可执行 -> out\main.exe；中间产物/缓存 -> .sgbuild\。
# 程序绑定 127.0.0.1:18099，固定处理 8 个请求后自动退出（便于自动化验证）。
$ErrorActionPreference = "Stop"
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

Push-Location (Split-Path (Split-Path $here))
& $exe build (Join-Path $here "main.sg")
$build = $LASTEXITCODE
Pop-Location
if ($build -ne 0) { exit $build }

& (Join-Path $here "out\main.exe")
exit $LASTEXITCODE
