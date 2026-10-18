# ============================================================================
#  bootstrap.ps1 - Scourge 一键构建引导（Windows）
#
#  与 bootstrap.sh 同职责：工具链检查 → 获取平台预编译 MLIR 库包（或本机
#  LLVM/MLIR）→ cmake 配置 → 构建 build-rel\scg.exe。
#  Windows 侧 AOT build 运行时另需 clang（D:\LLVM 或任意 LLVM 安装的
#  bin\clang.exe 在 PATH）与 MSVC/SDK 链接环境（scripts\vsenv.ps1 注入）。
#  用法：
#    scripts\bootstrap.ps1                     # 全自动（下载库包→构建）
#    scripts\bootstrap.ps1 -Url <zip>          # 本地/自定库包
#    scripts\bootstrap.ps1 -LlvmRoot E:\llvm\build  # 本机 LLVM/MLIR
#    scripts\bootstrap.ps1 -Jobs 8
#  注：ASCII only。Windows PowerShell 5.1 以 ANSI 解析无 BOM 的 ps1。
# ============================================================================
$ErrorActionPreference = "Stop"

$RepoRoot  = Split-Path -Parent $PSScriptRoot
$DepsDir   = Join-Path $RepoRoot ".deps"
$MlirRoot  = Join-Path $DepsDir "llvm-mlir"
$OwnerRepo = $env:SCOURGE_RELEASE_REPO   # 例：your-name/scourge
$Tag       = if ($env:SCOURGE_MLIR_TAG) { $env:SCOURGE_MLIR_TAG } else { "mlir-libs-v1" }
$AssetPfx  = "scourge-mlir-libs"

$Url = ""; $LlvmRoot = ""; $Jobs = [Environment]::ProcessorCount
for ($i = 0; $i -lt $args.Count; $i++) {
    switch ($args[$i]) {
        "-Url"       { $Url = $args[$i+1]; $i++ }
        "-LlvmRoot"  { $LlvmRoot = $args[$i+1]; $i++ }
        "-Jobs"      { $Jobs = [int]$args[$i+1]; $i++ }
        default      { Write-Error "unknown arg: $($args[$i])" }
    }
}

Write-Host "[bootstrap] toolchain check"
foreach ($t in @("cmake", "ninja", "clang")) {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) {
        Write-Error "missing $t in PATH"
    }
}

if ($LlvmRoot) {
    $MlirRoot = $LlvmRoot
    Write-Host "[bootstrap] using local LLVM/MLIR: $MlirRoot"
} else {
    $asset = "$AssetPfx-windows-x64.zip"
    if (Test-Path (Join-Path $MlirRoot "lib\cmake\mlir")) {
        Write-Host "[bootstrap] lib package ready: $MlirRoot (delete dir to force re-download)"
    } else {
        if (-not $Url) {
            if (-not $OwnerRepo) {
                Write-Error "no package source. set SCOURGE_RELEASE_REPO env, or pass -Url <file>, or -LlvmRoot <dir>"
            }
            $Url = "https://github.com/$OwnerRepo/releases/download/$Tag/$asset"
        }
        Write-Host "[bootstrap] fetching $Url"
        New-Item -ItemType Directory -Force $DepsDir | Out-Null
        $pkg = Join-Path $DepsDir "pkg.zip"
        # Local paths go through Copy-Item: Invoke-WebRequest on a 236MB local
        # file crawls through the WebClient stack (small buffers) and can hang.
        if ($Url -match '^(https?|ftp)://') {
            Invoke-WebRequest -Uri $Url -OutFile $pkg -UseBasicParsing
        } else {
            Copy-Item $Url $pkg -Force
        }
        if (Test-Path $MlirRoot) { Remove-Item -Recurse -Force $MlirRoot }
        Expand-Archive -Path $pkg -DestinationPath $DepsDir -Force
        Remove-Item $pkg
        if (-not (Test-Path (Join-Path $MlirRoot "lib\cmake\mlir"))) {
            Write-Error "bad package layout (lib\cmake\mlir missing)"
        }
        Write-Host "[bootstrap] package extracted to $MlirRoot (upstream LICENSE.TXT included)"
    }
}

# ---- AOT clang version gate ------------------------------------------------
# MLIR-generated IR carries package-version attributes (e.g. nocreateundeforpoison);
# an older system clang fails with "unterminated attribute group" at scg build
# time. Export SCOURGE_LLVM_BIN when the package bundles clang; otherwise warn.
$pkgClang = Join-Path $MlirRoot "bin\clang.exe"
$verFile  = Join-Path $MlirRoot "LLVM_VERSION.txt"
if (Test-Path $pkgClang) {
    $env:SCOURGE_LLVM_BIN = $MlirRoot + "\bin"
    Write-Host "[bootstrap] using packaged clang: $pkgClang"
} elseif (Test-Path $verFile) {
    $pkgVer = (Get-Content $verFile -Raw).Trim()
    $clangOut = (clang --version 2>$null | Select-Object -First 1)
    if ($pkgVer -match '^(\d+)' -and $clangOut -match 'version (\d+)') {
        if ([int]$Matches[1] -lt [int]$pkgVer.Split('.')[0]) {
            Write-Warning ("clang {0} is older than MLIR package {1}: AOT build will fail." -f $Matches[1], $pkgVer)
            Write-Warning "install a matching clang first: https://apt.llvm.org (Linux) / https://github.com/llvm/llvm-project/releases (Windows)"
        }
    }
}

Write-Host "[bootstrap] cmake configure (build-rel)"
. (Join-Path $RepoRoot "scripts\vsenv.ps1")
# 注：PS 5.1 实测多行续行 native 调用中裸 token 的 $var 会被字面传递，
# 改用双引号串元素数组 splatting（插值可靠）
$CMakeArgs = @(
    "-S", $RepoRoot,
    "-B", (Join-Path $RepoRoot "build-rel"),
    "-DCMAKE_BUILD_TYPE=Release",
    # 源码使用 clang/GCC builtin（__builtin_*_overflow），MSVC cl 编不过，
    # 必须显式指定 clang（PATH 中的 clang，toolchain check 已确保存在）
    "-DCMAKE_C_COMPILER=clang",
    "-DCMAKE_CXX_COMPILER=clang++",
    "-DSCOURGE_LLVM_ROOT=$MlirRoot",
    "-G", "Ninja"
)
cmake @CMakeArgs

Write-Host "[bootstrap] build (-j$Jobs)"
# three deliverables: scg (compiler) + scourge-mcp (standalone MCP entry) + sget
# (bootstrapped package manager -- its success is itself the AOT smoke test;
# target name sget_bootstrap avoids the Linux target/file name clash);
# scourge_tests = CTest unit tests.
cmake --build (Join-Path $RepoRoot "build-rel") -j $Jobs --target scourge scourge-mcp sget_bootstrap scourge_tests

Write-Host "[bootstrap] smoke"
$Bin = Join-Path $RepoRoot "build-rel"
$hello = & (Join-Path $Bin "scg.exe") run (Join-Path $RepoRoot "examples\hello.sg") | Select-Object -Last 1
Write-Host "[bootstrap]   scg:    $hello"
$sgetOut = (& (Join-Path $Bin "sget.exe") list 2>$null | Select-Object -First 1)
Write-Host "[bootstrap]   sget:  $sgetOut"
$mcpInit = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
# PS 5.1 piped stdin injects a UTF-8 BOM in -File mode -> scourge-mcp JSON parse
# fails at offset 0. Feed via stdin file + Start-Process (same pattern as
# the verify scripts) to bypass the pipeline encoder entirely.
$initF = Join-Path $env:TEMP "scourge-mcp_init.json"
$outF  = Join-Path $env:TEMP "scourge-mcp_init.out"
[System.IO.File]::WriteAllText($initF, $mcpInit + "`n")
Start-Process -FilePath (Join-Path $Bin "scourge-mcp.exe") -RedirectStandardInput $initF -RedirectStandardOutput $outF -NoNewWindow -Wait | Out-Null
$mcpOut = [System.IO.File]::ReadAllText($outF)
if ($mcpOut -match 'serverInfo') {
    Write-Host "[bootstrap]   scourge-mcp: initialize OK"
} else {
    Write-Warning "[bootstrap]   scourge-mcp: no serverInfo in initialize response"
}
Write-Host ("[bootstrap] done: {0} (+ scourge-mcp.exe, sget.exe) -- ready to use" -f (Join-Path $Bin "scg.exe"))
Write-Host "[bootstrap]   scg run <file.sg>      # VM direct run"
Write-Host "[bootstrap]   scg build <file.sg>    # AOT standalone executable"
