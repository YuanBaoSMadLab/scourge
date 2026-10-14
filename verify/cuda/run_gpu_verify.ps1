# run_gpu_verify.ps1 - three-mode byte-identical verification for CUDA layer 1.
# Modes per probe: (1) scourge run  (2) build CPU  (3) build --gpu  (4) build --gpu with SCOURGE_NO_GPU=1.
# Compares stdout/stderr bytes (SHA256) and exit codes; also compares .mlir between CPU/GPU builds.
# Layout: probes (source assets) live in d:\Scourge\verify\cuda (git-tracked);
#         all generated artifacts go to d:\Scourge\build-rel\verify\cuda (git-ignored).
# ASCII only (PowerShell 5.1 GBK lesson).
$ErrorActionPreference = "Stop"
$scourge = "d:\Scourge\build-rel\scourge.exe"
$src     = "d:\Scourge\verify\cuda"
$dir     = "d:\Scourge\build-rel\verify\cuda"
New-Item -ItemType Directory -Force $dir | Out-Null
$probes  = @("gpu_ew", "gpu_mm", "gpu_sum_tr", "gpu_panic")

function Invoke-Net([string]$exe, [string]$argStr, [string]$tag, [bool]$noGpu) {
    $outF = Join-Path $dir ("{0}.out" -f $tag)
    $errF = Join-Path $dir ("{0}.err" -f $tag)
    Remove-Item $outF, $errF -ErrorAction SilentlyContinue
    if ($noGpu) { $env:SCOURGE_NO_GPU = "1" } else { Remove-Item Env:SCOURGE_NO_GPU -ErrorAction SilentlyContinue }
    if ($argStr) {
        $p = Start-Process -FilePath $exe -ArgumentList $argStr -RedirectStandardOutput $outF -RedirectStandardError $errF -NoNewWindow -Wait -PassThru
    } else {
        $p = Start-Process -FilePath $exe -RedirectStandardOutput $outF -RedirectStandardError $errF -NoNewWindow -Wait -PassThru
    }
    if ($noGpu) { Remove-Item Env:SCOURGE_NO_GPU -ErrorAction SilentlyContinue }
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
    $r1 = Invoke-Net $scourge ("run `"{0}`"" -f $sg) ("{0}_run" -f $name) $false

    # mode 2: build CPU
    $rb = Invoke-Net $scourge ("build `"{0}`" -o `"{1}`"" -f $sg, $cpuExe) ("{0}_bcpu" -f $name) $false
    if ($rb[0] -ne 0) { Write-Host "[$name] BUILD-CPU FAILED exit=$($rb[0])"; Get-Content ($rb[2]); $fail++; continue }
    $r2 = Invoke-Net $cpuExe "" ("{0}_cpu" -f $name) $false

    # mode 3: build --gpu
    $rg = Invoke-Net $scourge ("build --gpu `"{0}`" -o `"{1}`"" -f $sg, $gpuExe) ("{0}_bgpu" -f $name) $false
    if ($rg[0] -ne 0) { Write-Host "[$name] BUILD-GPU FAILED exit=$($rg[0])"; Get-Content ($rg[2]); $fail++; continue }
    $r3 = Invoke-Net $gpuExe "" ("{0}_gpu" -f $name) $false

    # mode 4: build --gpu + SCOURGE_NO_GPU=1 (runtime CPU fallback)
    $r4 = Invoke-Net $gpuExe "" ("{0}_gpufb" -f $name) $true

    # .mlir must be byte-identical between CPU and GPU builds
    $mlirSame = (Get-Hash2 $cpuMlir) -eq (Get-Hash2 $gpuMlir)

    # GPU exe must actually reference sg_gpu_* symbols
    $llTxt = ""
    if (Test-Path $gpuLl) { $llTxt = (Get-Content $gpuLl -Raw) }
    $hasGpuSym = $llTxt -match "sg_gpu_"

    $ok = $true
    if ($r1[0] -ne $r2[0] -or $r2[0] -ne $r3[0] -or $r3[0] -ne $r4[0]) { $ok = $false }
    foreach ($k in 1,2) {
        $h1 = Get-Hash2 $r1[$k]; $h2 = Get-Hash2 $r2[$k]; $h3 = Get-Hash2 $r3[$k]; $h4 = Get-Hash2 $r4[$k]
        if ($h1 -ne $h2 -or $h2 -ne $h3 -or $h3 -ne $h4) { $ok = $false }
    }
    if (-not $mlirSame) { $ok = $false }

    $status = "PASS"
    if (-not $ok) { $status = "DIFFER"; $fail++ }
    Write-Host ("[{0}] {1} exit={2} mlir_same={3} gpu_syms={4}" -f $name, $status, $r3[0], $mlirSame, $hasGpuSym)
    if (-not $ok) {
        Write-Host ("  hashes: run_out={0} cpu_out={1} gpu_out={2} fb_out={3}" -f (Get-Hash2 $r1[1]), (Get-Hash2 $r2[1]), (Get-Hash2 $r3[1]), (Get-Hash2 $r4[1]))
        Write-Host ("  hashes: run_err={0} cpu_err={1} gpu_err={2} fb_err={3}" -f (Get-Hash2 $r1[2]), (Get-Hash2 $r2[2]), (Get-Hash2 $r3[2]), (Get-Hash2 $r4[2]))
    }
}

Write-Host ""
if ($fail -eq 0) { Write-Host "ALL PASS" } else { Write-Host "FAILURES: $fail" }
exit $fail
