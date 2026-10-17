# verify_bindgen.ps1 — bindgen（Python 风格绑定 → Scourge 绑定 + C++ 代理 shim）端到端回归
# 用法: 'A' | powershell -ExecutionPolicy Bypass -File verify_bindgen.ps1
$ErrorActionPreference = "Stop"
$fail = 0
$root = "D:\Scourge"
$bg = "$root\examples\bindgen"
$pkg = "$bg\packages"
$sg = "$root\build-rel\scourge.exe"

function Fail($m) { Write-Host "$m FAIL"; $script:fail++ }
function Pass($m) { Write-Host "$m PASS" }

Push-Location $root
try {
    # ---- 0. bindgen 自举构建（Scourge 写的生态工具）----
    & $sg build "$bg\main.sg" -o "$bg\bindgen.exe" *> $null
    if ($LASTEXITCODE -ne 0) { Fail "bindgen-build"; Pop-Location; exit 1 }
    Pass "bindgen-build (AOT 自举)"

    # ---- 1. slice 内建（bindgen 依赖）双模 ----
    $t = "$env:TEMP\vb_slice.sg"
@"
def main() -> int:
    s: str = "hello, bindgen"
    print(s.slice(7, 14))
    print("[" + s.slice(5, 2) + "]")
    print("[" + s.slice(10, 999) + "]")
    return 0
"@ | Set-Content $t -Encoding UTF8
    $vm = & $sg run $t 2>&1
    & $sg build $t -o "$env:TEMP\vb_slice.exe" *> $null
    $aot = & "$env:TEMP\vb_slice.exe" 2>&1
    if (($vm -join "`n") -eq ($aot -join "`n") -and $aot -contains "bindgen") {
        Pass "slice 双模一致"
    } else { Fail "slice 双模 ($vm | $aot)" }

    # ---- 2. and/or 短路（VM/AOT 语义一致）----
    $t2 = "$root\verify\probes\probe_logic_shortcircuit.sg"
    $vm2 = & $sg run $t2 2>&1
    & $sg build $t2 -o "$env:TEMP\vb_sc_probe.exe" *> $null
    $aot2 = & "$env:TEMP\vb_sc_probe.exe" 2>&1
    if (($vm2 -join "`n") -eq ($aot2 -join "`n") -and ($vm2 -join "`n") -match "panic: True") {
        Pass "短路求值双模一致"
    } else { Fail "短路求值 ($vm2)" }

    # ---- 3. bindgen 生成（C++ 代理 + 纯 C 直通两路）----
    Remove-Item "$pkg\demo_cpp\shim", "$pkg\demo_cpp\demo_cpp.sg" -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item "$pkg\mathx\mathx.sg" -Force -ErrorAction SilentlyContinue
    $g = & "$bg\bindgen.exe" --all $pkg 2>&1
    if ($LASTEXITCODE -ne 0) { Fail "bindgen --all ($g)"; Pop-Location; exit 1 }
    if (-not (Test-Path "$pkg\demo_cpp\shim\shim.cpp") -or
        -not (Test-Path "$pkg\demo_cpp\shim\build_shim.bat") -or
        -not (Test-Path "$pkg\demo_cpp\demo_cpp.sg") -or
        -not (Test-Path "$pkg\mathx\mathx.sg")) { Fail "bindgen 产物缺失"; Pop-Location; exit 1 }
    if ((Get-Content "$pkg\demo_cpp\shim\shim.cpp" -Raw) -notmatch 'new demo::Counter') {
        Fail "shim.cpp 内容异常"
    } else { Pass "bindgen --all（demo_cpp 代理 + mathx 直通）" }

    # ---- 4. shim 编译（vendored 头文件集成）----
    Push-Location "$pkg\demo_cpp\shim"
    $cb = & cmd /c build_shim.bat 2>&1
    Pop-Location
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$pkg\demo_cpp\bin\demo_cpp_shim.dll")) {
        Fail "shim 编译 ($cb)"; Pop-Location; exit 1
    }
    Pass "C++ 代理 shim 编译（clang++ + vendored 头）"

    # ---- 5. demo_cpp 双模闭环（C++ 类经代理直调）----
    Remove-Item "$pkg\demo_cpp\demo_demo*" -Recurse -Force -Confirm:$false -ErrorAction SilentlyContinue
    Copy-Item "$pkg\demo_cpp\bin\demo_cpp_shim.dll" "$pkg\demo_cpp\" -Force
    Push-Location "$pkg\demo_cpp"
    $vm3 = & $sg run demo_cpp_demo.sg 2>&1
    & $sg build demo_cpp_demo.sg -o demo_demo.exe *> $null
    $aot3 = & .\demo_demo.exe 2>&1
    Pop-Location
    if (($vm3 -join "`n") -eq ($aot3 -join "`n") -and ($aot3 -join "`n") -match "after 5\+7 = 12") {
        Pass "demo_cpp 双模闭环（C++ 类经 shim）"
    } else { Fail "demo_cpp 双模 ($vm3 | $aot3)" }

    # ---- 6. mathx 纯 C 直通双模 ----
    Push-Location "$pkg\mathx"
    $vm4 = & $sg run mathx_demo.sg 2>&1
    & $sg build mathx_demo.sg -o mathx_demo.exe *> $null
    $aot4 = & .\mathx_demo.exe 2>&1
    Pop-Location
    if (($vm4 -join "`n") -eq ($aot4 -join "`n") -and ($aot4 -join "`n") -match "pow\(2,10\) = 1024") {
        Pass "mathx 直通双模（msvcrt）"
    } else { Fail "mathx 双模 ($vm4 | $aot4)" }

    # ---- 7. sget bind 集成 ----
    $sgetDir = "$root\examples\sget"
    & $sg build "$sgetDir\main.sg" -o "$sgetDir\sget.exe" *> $null
    Push-Location $sgetDir
    Remove-Item demo_cpp_shim.dll -Force -ErrorAction SilentlyContinue
    $b = & cmd /c "sget.exe bind demo_cpp 2>&1"
    Pop-Location
    if (-not (Test-Path "$sgetDir\demo_cpp_shim.dll")) { Fail "sget bind ($b)"; Pop-Location; exit 1 }
    Push-Location $sgetDir
    $vm5 = & $sg run bind_demo.sg 2>&1
    & $sg build bind_demo.sg -o bind_demo.exe *> $null
    $aot5 = & .\bind_demo.exe 2>&1
    Pop-Location
    if (($vm5 -join "`n") -eq ($aot5 -join "`n") -and ($aot5 -join "`n") -match "sget\+bindgen = 46") {
        Pass "sget bind 全闭环（生成→编译→部署→直调）"
    } else { Fail "sget bind 闭环 ($vm5 | $aot5)" }
} finally {
    Pop-Location
}

if ($fail -gt 0) { Write-Host "BINDGEN FAILED: $fail 项"; exit 1 }
Write-Host "BINDGEN ALL PASS"
