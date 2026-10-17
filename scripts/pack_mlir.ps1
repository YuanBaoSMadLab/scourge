# ============================================================================
#  pack_mlir.ps1 - LLVM/MLIR Library Pack Packaging (Windows, maintainer tool)
#
#  Solution: cmake --graphviz dependency graph BFS finds the MLIR*/LLVM*
#  closure (seeds = direct deps of scourge* targets, zero duplication with
#  CMakeLists); manually assemble the package (install requires the full
#  build tree, which a partial tree cannot satisfy):
#    include/  <- source-tree llvm/mlir includes + build-tree generated includes (robocopy merge)
#    lib/      <- only closure static libs (check each target maps to a real file)
#    lib/cmake <- copy build-tree cmake config + normalize absolute paths to package-relative ${_SCOURGE_PKG}
#    bin/      <- only keep mlir-tblgen.exe
#  Output: dist\scourge-mlir-libs-windows-x64.zip (+sha256)
#
#  Usage: scripts\pack_mlir.ps1 [-LlvmBuild E:\llvm\build]
#  Note: ASCII only. Windows PowerShell 5.1 parses BOM-less ps1 as ANSI.
# ============================================================================
param(
    [string]$LlvmBuild = "E:\llvm\build"
)
$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$OutName  = "scourge-mlir-libs-windows-x64"

# Source-tree includes are at the siblings of the build tree: E:\llvm\llvm, E:\llvm\mlir
$LlvmRoot = Split-Path -Parent $LlvmBuild

$Dot  = Join-Path $env:TEMP ("dep-" + [guid]::NewGuid().ToString("N").Substring(0,8) + ".dot")
$Work = Join-Path $env:TEMP ("mlirpkg-" + [guid]::NewGuid().ToString("N").Substring(0,8))
$Pkg  = Join-Path $Work "llvm-mlir"
New-Item -ItemType Directory -Force $Pkg | Out-Null
try {
    # ---- Closure -------------------------------------------------------------
    Write-Host "[pack] cmake --graphviz"
    & cmake --graphviz $Dot -S $RepoRoot -B (Join-Path $RepoRoot "build-rel") | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Error "graphviz generation failed" }

    # Edge comments directly read names: // A -> B
    $edges = @()
    foreach ($line in (Get-Content $Dot)) {
        if ($line -match '//\s*(\S+)\s*->\s*(\S+)\s*$') {
            $edges += [pscustomobject]@{ From = $Matches[1]; To = $Matches[2] }
        }
    }
    $seeds = ($edges | Where-Object { $_.From -match '^scourge' }).To | Sort-Object -Unique
    if (-not $seeds) { Write-Error "no seed edges found in dot" }

    Write-Host "[pack] BFS closure over MLIR*/LLVM* targets"
    $reach = @{}
    $front = @($seeds)
    while ($front.Count -gt 0) {
        $next = @()
        foreach ($x in $front) {
            if ($reach.ContainsKey($x)) { continue }
            $reach[$x] = $true
            $next += ($edges | Where-Object { $_.From -eq $x }).To
        }
        $front = $next | Sort-Object -Unique
    }
    # obj.* are OBJECT libraries with no .lib artifact; remove them
    $closed = $reach.Keys | Where-Object { $_ -match '^(MLIR|LLVM)' -and $_ -notlike 'obj.*' } | Sort-Object
    Write-Host ("[pack] closure targets: {0}" -f $closed.Count)
    if ($closed.Count -eq 0) { Write-Error "empty closure" }

    # ---- include (robocopy merge of four directories) ------------------------
    Write-Host "[pack] copy includes"
    foreach ($inc in @(
        (Join-Path $LlvmRoot "llvm\include"),
        (Join-Path $LlvmRoot "mlir\include"),
        (Join-Path $LlvmBuild "include"),
        (Join-Path $LlvmBuild "tools\mlir\include")
    )) {
        if (Test-Path $inc) {
            robocopy $inc (Join-Path $Pkg "include") /E /NFL /NDL /NJH /NJS /NP | Out-Null
            if ($LASTEXITCODE -ge 8) { Write-Error "robocopy failed: $inc" }
        } else {
            Write-Host "[pack] WARN include dir missing: $inc"
        }
    }

    # ---- lib (copy only those inside the closure; check each maps to a real file)
    Write-Host "[pack] copy closure libs"
    $libDir = Join-Path $LlvmBuild "Release\lib"
    $binDir = Join-Path $LlvmBuild "Release\bin"
    if (-not (Test-Path $libDir)) { $libDir = Join-Path $LlvmBuild "lib" }
    if (-not (Test-Path $binDir)) { $binDir = Join-Path $LlvmBuild "bin" }
    New-Item -ItemType Directory -Force (Join-Path $Pkg "lib") | Out-Null
    # INTERFACE IMPORTED target（纯头依赖，如 MLIRSparseTensorEnums）无 .lib 产物，
    # 从构建树 targets 文件识别并排除
    $tgtText = ""
    foreach ($tf in @("MLIRTargets.cmake", "LLVMTargets.cmake")) {
        $p = Join-Path $LlvmBuild "lib\cmake\$($tf -replace 'Targets\.cmake$','')\$tf"
        if (Test-Path $p) { $tgtText += [IO.File]::ReadAllText($p) }
    }
    $missing = @()
    foreach ($t in $closed) {
        $lib = Join-Path $libDir "$t.lib"
        if (Test-Path $lib) {
            Copy-Item $lib (Join-Path $Pkg "lib") -Force
        } elseif ($tgtText -match ('add_library\("?' + [regex]::Escape($t) + '"? INTERFACE')) {
            # INTERFACE library, no artifact
        } elseif (Test-Path (Join-Path $binDir "$t.exe")) {
            # Tool-type target (llvm-tblgen, etc.), no .lib, skip
        } else {
            $missing += $t
        }
    }
    if ($missing.Count -gt 0) {
        Write-Error ("closure libs missing in build tree: " + ($missing -join ', '))
    }
    Write-Host ("[pack] libs copied: {0}" -f (Get-ChildItem (Join-Path $Pkg "lib") -Filter *.lib).Count)

    # ---- lib/cmake (copy config + normalize absolute paths) ------------------
    # Replace paths from the build tree/source tree into package-relative ${_SCOURGE_PKG};
    # inject the variable definition at the top of targets/config files. Rules:
    #   E:/llvm/build/(Release|Debug)/ -> ${_SCOURGE_PKG}/
    #   E:/llvm/build/                 -> ${_SCOURGE_PKG}/
    #   E:/llvm/(llvm|mlir)/include/   -> ${_SCOURGE_PKG}/include/
    Write-Host "[pack] copy cmake config (relocatable rewrite)"
    foreach ($c in @("llvm", "mlir")) {
        $srcC = Join-Path $LlvmBuild "lib\cmake\$c"
        $dstC = Join-Path $Pkg "lib\cmake\$c"
        if (-not (Test-Path $srcC)) { Write-Error "cmake config missing: $srcC" }
        New-Item -ItemType Directory -Force $dstC | Out-Null
        foreach ($f in (Get-ChildItem $srcC -File)) {
            $txt = [IO.File]::ReadAllText($f.FullName)
            # In -replace substitution strings $ is special (group reference), $$ means literal $
            # Order matters: replace specific paths first, then generic ones. $(Configuration) in PS double quotes
            # is a subexpression, needs `$ escape. Both slash flavors: file content is
            # forward-slashed while $LlvmBuild/$LlvmRoot come in backslashed from param.
            $bf = $LlvmBuild -replace '\\', '/'      # forward-slashed build dir
            $rf = $LlvmRoot -replace '\\', '/'       # forward-slashed source root
            $txt = $txt -replace [regex]::Escape("$bf/`$(Configuration)/"), '$${_SCOURGE_PKG}/'
            $txt = $txt -replace [regex]::Escape("$LlvmBuild\`$(Configuration)\"), '$${_SCOURGE_PKG}\'
            $txt = $txt -replace [regex]::Escape("$bf/Release/"), '$${_SCOURGE_PKG}/'
            $txt = $txt -replace [regex]::Escape("$LlvmBuild\Release\"), '$${_SCOURGE_PKG}\'
            $txt = $txt -replace [regex]::Escape("$bf/Debug/"),   '$${_SCOURGE_PKG}/'
            $txt = $txt -replace [regex]::Escape("$LlvmBuild\Debug\"),   '$${_SCOURGE_PKG}\'
            $txt = $txt -replace [regex]::Escape("$bf/./"),       '$${_SCOURGE_PKG}/'
            $txt = $txt -replace [regex]::Escape("$LlvmBuild\.\\"),      '$${_SCOURGE_PKG}\'
            $txt = $txt -replace [regex]::Escape("$LlvmBuild\"),         '$${_SCOURGE_PKG}\'
            $txt = $txt -replace [regex]::Escape("$bf/"),         '$${_SCOURGE_PKG}/'
            # Bare paths without trailing separator (set(LLVM_BINARY_DIR "E:/llvm/build"))
            # must run after all separator rules and BEFORE bare source root, so
            # "E:/llvm/build..." is fully consumed before "E:/llvm" could match it.
            $txt = $txt -replace [regex]::Escape("$bf"),          '$${_SCOURGE_PKG}'
            $txt = $txt -replace [regex]::Escape("$LlvmBuild"),   '$${_SCOURGE_PKG}'
            # Source-tree paths: whole-tree prefix rewrite. Special-case cmake/modules
            # first (points into the packaged cmake dir), then each source subtree.
            # Non-include source dirs (utils/TableGen etc.) resolve to nonexistent
            # package paths - harmless: scourge never builds upstream tablegen tools.
            $txt = $txt -replace [regex]::Escape("$rf/llvm/cmake/modules"), '$${_SCOURGE_PKG}/lib/cmake/llvm'
            $txt = $txt -replace [regex]::Escape("$LlvmRoot\llvm\cmake\modules"), '$${_SCOURGE_PKG}\lib\cmake\llvm'
            foreach ($s in @("llvm", "mlir", "third-party", "libc", "cmake")) {
                $txt = $txt -replace [regex]::Escape("$rf/$s"), '$${_SCOURGE_PKG}'
                $txt = $txt -replace [regex]::Escape("$LlvmRoot\$s"), '$${_SCOURGE_PKG}'
            }
            $txt = $txt -replace [regex]::Escape("$rf"),          '$${_SCOURGE_PKG}'
            $txt = $txt -replace [regex]::Escape("$LlvmRoot"),    '$${_SCOURGE_PKG}'
            $hdr = 'set(_SCOURGE_PKG "${CMAKE_CURRENT_LIST_DIR}/../../..")' + "`r`n"
            [IO.File]::WriteAllText((Join-Path $dstC $f.Name), $hdr + $txt)
        }
    }

    # ---- bin + LICENSE -------------------------------------------------------
    New-Item -ItemType Directory -Force (Join-Path $Pkg "bin") | Out-Null
    $tblgen = Join-Path $binDir "mlir-tblgen.exe"
    if (Test-Path $tblgen) { Copy-Item $tblgen (Join-Path $Pkg "bin") -Force }
    else { Write-Error "mlir-tblgen.exe not found: $tblgen" }

    $lic = Join-Path $LlvmRoot "llvm\LICENSE.TXT"
    if (Test-Path $lic) { Copy-Item $lic (Join-Path $Pkg "LICENSE.TXT") }
    else { Write-Host "[pack] WARN LICENSE.TXT missing: $lic" }

    # Record LLVM version: bootstrap reads it as the AOT clang version gate
    # (MLIR-generated IR carries this version's attributes; older clang fails).
    # Note: LLVMConfig.cmake line is unquoted: set(LLVM_PACKAGE_VERSION 23.1.3)
    $llvmCfg = Join-Path $Pkg "lib\cmake\llvm\LLVMConfig.cmake"
    $cfgText = [IO.File]::ReadAllText($llvmCfg)
    if ($cfgText -match 'LLVM_PACKAGE_VERSION\s+"?([0-9][0-9.]*)') {
        [IO.File]::WriteAllText((Join-Path $Pkg "LLVM_VERSION.txt"), $Matches[1] + "`r`n")
    } else { Write-Host "[pack] WARN LLVM_PACKAGE_VERSION not found in LLVMConfig.cmake" }

    # ---- residue check（兜底：包内不得残留打包机专属路径）----------------------
    Write-Host "[pack] residue check (machine-specific paths)"
    $badFiles = @()
    foreach ($f in (Get-ChildItem (Join-Path $Pkg "lib\cmake") -Recurse -Filter *.cmake)) {
        $t = [IO.File]::ReadAllText($f.FullName)
        if ($t -match [regex]::Escape("$bf") -or $t -match [regex]::Escape("$rf") -or
            $t -match [regex]::Escape("$LlvmBuild") -or $t -match [regex]::Escape("$LlvmRoot")) {
            $badFiles += $f.Name
        }
    }
    if ($badFiles.Count -gt 0) {
        Write-Error ("machine paths remain in: " + ($badFiles -join ', '))
    }

    # ---- zip -------------------------------------------------------------------
    Write-Host "[pack] zip"
    New-Item -ItemType Directory -Force (Join-Path $RepoRoot "dist") | Out-Null
    $zip = Join-Path $RepoRoot "dist\$OutName.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path $Pkg -DestinationPath $zip -CompressionLevel Optimal
    $sz = "{0:N1} MB" -f ((Get-Item $zip).Length / 1MB)
    $sha = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host "[pack] done: dist\$OutName.zip  size=$sz  sha256=$sha"
} finally {
    Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
    Remove-Item $Dot -ErrorAction SilentlyContinue
}
