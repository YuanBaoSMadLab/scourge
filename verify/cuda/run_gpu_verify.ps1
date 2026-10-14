# run_gpu_verify.ps1 - five-mode byte-identical verification for the GPU layer.
# Modes per probe: (1) scourge run  (2) build CPU  (3) build --gpu (cuda default)
#                  (4) build --gpu with SCOURGE_NO_GPU=1 (cpu fallback)
#                  (5) build --gpu with SCOURGE_GPU_BACKEND=opencl
# Compares stdout/stderr bytes (SHA256) and exit codes across all five modes;
# also compares .mlir between CPU/GPU builds.
# Layout: probes (source assets) live in d:\Scourge\verify\cuda (git-tracked);
#         all generated artifacts go to d:\Scourge\build-rel\verify\cuda (git-ignored).
# ASCII only (PowerShell 5.1 GBK lesson).
$ErrorActionPreference = "Stop"
$scourge = "d:\Scourge\build-rel\scourge.exe"
$src     = "d:\Scourge\verify\cuda"
$dir     = "d:\Scourge\build-rel\verify\cuda"
New-Item -ItemType Directory -Force $dir | Out-Null
$probes  = @("gpu_ew", "gpu_mm", "gpu_mm_tile", "gpu_sum_tr", "gpu_panic", "gpu_dyn", "gpu_int", "gpu_chain")

# $mode: "run" | "cpu" | "gpu" | "fb" | "ocl"
function Invoke-Net([string]$exe, [string]$argStr, [string]$tag, [string]$mode) {
    $outF = Join-Path $dir ("{0}.out" -f $tag)
    $errF = Join-Path $dir ("{0}.err" -f $tag)
    Remove-Item $outF, $errF -ErrorAction SilentlyContinue
    Remove-Item Env:SCOURGE_NO_GPU -ErrorAction SilentlyContinue
    Remove-Item Env:SCOURGE_GPU_BACKEND -ErrorAction SilentlyContinue
    if ($mode -eq "fb") { $env:SCOURGE_NO_GPU = "1" }
    elseif ($mode -eq "ocl") { $env:SCOURGE_GPU_BACKEND = "opencl" }
    if ($argStr) {
        $p = Start-Process -FilePath $exe -ArgumentList $argStr -RedirectStandardOutput $outF -RedirectStandardError $errF -NoNewWindow -Wait -PassThru
    } else {
        $p = Start-Process -FilePath $exe -RedirectStandardOutput $outF -RedirectStandardError $errF -NoNewWindow -Wait -PassThru
    }
    Remove-Item Env:SCOURGE_NO_GPU -ErrorAction SilentlyContinue
    Remove-Item Env:SCOURGE_GPU_BACKEND -ErrorAction SilentlyContinue
    return @($p.ExitCode, $outF, $errF)
}

function Get-Hash2([string]$f) {
    if (-not (Test-Path $f)) { return "<MISSING>" }
    return (Get-FileHash $f -Algorithm SHA256).Hash
}

$fail = 0
foreach ($name in $probes) {
    $sg = Join-Path $src ("{0}.sg" -f $name)
    $cpuExe = Join-Path $dir ("{0}_cpu.exe" -f $name)
    $gpuExe = Join-Path $dir ("{0}_gpu.exe" -f $name)
    $cpuMlir = Join-Path $dir ("{0}_cpu.mlir" -f $name)
    $gpuMlir = Join-Path $dir ("{0}_gpu.mlir" -f $name)
    $gpuLl = Join-Path $dir ("{0}_gpu.ll" -f $name)

    # clean stale artifacts
    Remove-Item $cpuExe, $gpuExe, $cpuMlir, $gpuMlir, $gpuLl -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $dir ("{0}_cpu_rt.c" -f $name)) -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $dir ("{0}_gpu_rt.c" -f $name)) -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $dir ("{0}.sgbc" -f $name)) -ErrorAction SilentlyContinue

    # mode 1: scourge run (VM)
    $r1 = Invoke-Net $scourge ("run `"{0}`"" -f $sg) ("{0}_run" -f $name) "run"

    # mode 2: build CPU (--no-cache: full pipeline must re-dump .mlir/.ll artifacts
    #          every run; obj-cache hit path skips artifact writing by design)
    $rb = Invoke-Net $scourge ("build --no-cache `"{0}`" -o `"{1}`"" -f $sg, $cpuExe) ("{0}_bcpu" -f $name) "run"
    if ($rb[0] -ne 0) { Write-Host "[$name] BUILD-CPU FAILED exit=$($rb[0])"; Get-Content ($rb[2]); $fail++; continue }
    $r2 = Invoke-Net $cpuExe "" ("{0}_cpu" -f $name) "cpu"

    # mode 3: build --gpu (default backend: cuda)
    $rg = Invoke-Net $scourge ("build --gpu --no-cache `"{0}`" -o `"{1}`"" -f $sg, $gpuExe) ("{0}_bgpu" -f $name) "run"
    if ($rg[0] -ne 0) { Write-Host "[$name] BUILD-GPU FAILED exit=$($rg[0])"; Get-Content ($rg[2]); $fail++; continue }
    $r3 = Invoke-Net $gpuExe "" ("{0}_gpu" -f $name) "gpu"

    # mode 4: build --gpu + SCOURGE_NO_GPU=1 (runtime CPU fallback)
    $r4 = Invoke-Net $gpuExe "" ("{0}_gpufb" -f $name) "fb"

    # mode 5: build --gpu + SCOURGE_GPU_BACKEND=opencl (OpenCL backend)
    $r5 = Invoke-Net $gpuExe "" ("{0}_gpuaocl" -f $name) "ocl"

    # .mlir must be byte-identical between CPU and GPU builds (missing = FAIL,
    # e.g. stale obj cache skipped regeneration)
    $mlirSame = (Test-Path $cpuMlir) -and (Test-Path $gpuMlir) -and `
        ((Get-Hash2 $cpuMlir) -eq (Get-Hash2 $gpuMlir))

    # GPU exe must actually reference sg_gpu_* symbols
    $llTxt = ""
    if (Test-Path $gpuLl) { $llTxt = (Get-Content $gpuLl -Raw) }
    $hasGpuSym = $llTxt -match "sg_gpu_"

    $ok = $true
    foreach ($pair in @($r1, $r2, $r3, $r4, $r5)) {
        if ($pair[0] -ne $r1[0]) { $ok = $false }
        for ($k = 1; $k -le 2; $k++) {
            if ((Get-Hash2 $pair[$k]) -ne (Get-Hash2 $r1[$k])) { $ok = $false }
        }
    }
    if (-not $mlirSame) { $ok = $false }

    $status = "PASS"
    if (-not $ok) { $status = "DIFFER"; $fail++ }
    Write-Host ("[{0}] {1} exit={2} mlir_same={3} gpu_syms={4}" -f $name, $status, $r3[0], $mlirSame, $hasGpuSym)
    if (-not $ok) {
        $tags = @("run", "cpu", "gpu", "fb", "ocl")
        $all = @($r1, $r2, $r3, $r4, $r5)
        for ($k = 1; $k -le 2; $k++) {
            $hs = ($tags | ForEach-Object { $i = $tags.IndexOf($_); ("{0}={1}" -f $_, (Get-Hash2 $all[$i][$k])) }) -join " "
            Write-Host ("  hashes {0}: {1}" -f $(if ($k -eq 1) { "out" } else { "err" }), $hs)
        }
    }
}

Write-Host ""
if ($fail -eq 0) { Write-Host "ALL PASS" } else { Write-Host "FAILURES: $fail" }
exit $fail
