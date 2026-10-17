// ============================================================================
//  Scourge 编译器 — AOT C 运行时 · GPU 分派层 · 后端抽象与设备管理
//  （Builder 分块专用）
// ----------------------------------------------------------------------------
//  职责边界：kGpuRuntimeC 第 1 段——后端抽象（SgGpuBe vtbl 六原语 alloc/
//  mfree/h2d/d2h/launch1d/launch2d）+ 双后端——CUDA（nvcuda.dll driver API
//  + 内嵌 PTX JIT）与 OpenCL（OpenCL.dll ICD loader + 内嵌 OpenCL C 源码
//  JIT，kernel 与 sg_kernels.ll 一一镜像、.ll 为语义权威）。
//  SCOURGE_GPU_BACKEND=cuda|opencl|cpu 选择（缺省自适应：先 CUDA 后 OpenCL
//  fp64 GPU）；SCOURGE_NO_GPU=1 强制 CPU（逃生门兼回退路径测试钩子）。
//  任何初始化/launch 失败：sg_gpu_state=0 永久回退 CPU 实现（语义一致仅
//  性能降级）。覆盖集 try 函数与 CPU 桩在 kGpuOpsRuntimeC（第 2 段）。
// ============================================================================
#pragma once

namespace scourge {

inline const char kGpuRuntimeC[] = R"GP(/* ==== GPU 分派第 1 层（后端抽象 + CUDA/OpenCL 覆盖集） ==== */
#ifdef _WIN32
typedef int CUresult;
typedef struct CUctx_st *CUcontext;
typedef struct CUmod_st *CUmodule;
typedef struct CUfunc_st *CUfunction;
typedef unsigned long long CUdeviceptr;

static CUcontext sg_gpu_ctx = NULL;
static CUmodule sg_gpu_mod = NULL;
/* kernel 句柄数组（后端无关索引；CUDA 存 CUfunction） */
enum {
    SG_K_TT = 0, SG_K_TS, SG_K_TI, SG_K_TSI, SG_K_MM, SG_K_SUM, SG_K_SUMI,
    SG_K_TR, SG_K_REDF, SG_K_REDI, SG_K_UNF, SG_K_UNAI, SG_K_UNSI, SG_K_CPY,
    SG_K_N
};
static CUfunction sg_cu_fn[SG_K_N];
static CUresult (*sg_cuMemAlloc)(CUdeviceptr *, size_t);
static CUresult (*sg_cuMemFree)(CUdeviceptr);
static CUresult (*sg_cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);
static CUresult (*sg_cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
static CUresult (*sg_cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned,
                                     unsigned, unsigned, unsigned, unsigned,
                                     CUdeviceptr, void **, void ***);
static CUresult (*sg_cuCtxSynchronize)(void);
static int sg_gpu_state = -1; /* -1 未初始化 0 不可用 1 就绪 */

/* ---- 后端原语 vtbl：CUDA/OpenCL 各自填充，try 函数与缓存层后端无关 ----
 * dev 句柄统一 void*（CUDA CUdeviceptr / OpenCL cl_mem，x64 上均 8 字节）；
 * launch 自带完成同步（CUDA CtxSynchronize / OpenCL clFinish），一次调用
 * 返回即 kernel 已完成——try 函数读回/panic 时序与缓存逐出假设依赖此点。 */
typedef struct {
    int (*alloc)(void **out, size_t bytes);
    void (*mfree)(void *p);
    int (*h2d)(void *dev, const void *host, size_t bytes);
    int (*d2h)(void *host, const void *dev, size_t bytes);
    int (*launch1d)(int k, int64_t n, void **args);
    int (*launch2d)(int k, int64_t gx, int64_t gy, void **args);
} SgGpuBe;
static SgGpuBe sg_be;

/* 设备缓冲钩子实现（定义见后段注册表块；sg_gpu_init 安装） */
static void sg_tsync_impl(SgTensor *t);
static void sg_tdirty_impl(SgTensor *t);

static void sg_gpu_fail(void) { sg_gpu_state = 0; }

/* ---- CUDA 后端（nvcuda.dll driver API + 内嵌 PTX JIT；原路径） ---- */
static int sg_cuda_init(void) {
    HMODULE h = LoadLibraryA("nvcuda.dll");
    if (!h) return 1;
    CUresult (*cuInit)(unsigned) = (void *)GetProcAddress(h, "cuInit");
    CUresult (*cuDeviceGet)(int *, int) =
        (void *)GetProcAddress(h, "cuDeviceGet");
    CUresult (*cuRetain)(CUcontext *, int) =
        (void *)GetProcAddress(h, "cuDevicePrimaryCtxRetain");
    CUresult (*cuSetCtx)(CUcontext) =
        (void *)GetProcAddress(h, "cuCtxSetCurrent");
    sg_cuCtxSynchronize = (void *)GetProcAddress(h, "cuCtxSynchronize");
    CUresult (*cuModLoad)(CUmodule *, const void *) =
        (void *)GetProcAddress(h, "cuModuleLoadData");
    CUresult (*cuModFn)(CUfunction *, CUmodule, const char *) =
        (void *)GetProcAddress(h, "cuModuleGetFunction");
    sg_cuMemAlloc = (void *)GetProcAddress(h, "cuMemAlloc_v2");
    sg_cuMemFree = (void *)GetProcAddress(h, "cuMemFree_v2");
    sg_cuMemcpyHtoD = (void *)GetProcAddress(h, "cuMemcpyHtoD_v2");
    sg_cuMemcpyDtoH = (void *)GetProcAddress(h, "cuMemcpyDtoH_v2");
    sg_cuLaunchKernel = (void *)GetProcAddress(h, "cuLaunchKernel");
    if (!cuInit || !cuDeviceGet || !cuRetain || !cuSetCtx ||
        !sg_cuCtxSynchronize || !cuModLoad || !cuModFn || !sg_cuMemAlloc ||
        !sg_cuMemFree || !sg_cuMemcpyHtoD || !sg_cuMemcpyDtoH ||
        !sg_cuLaunchKernel)
        return 1;
    if (cuInit(0)) return 1;
    int dev = 0;
    if (cuDeviceGet(&dev, 0)) return 1;
    if (cuRetain(&sg_gpu_ctx, dev)) return 1;
    if (cuSetCtx(sg_gpu_ctx)) return 1;
    if (cuModLoad(&sg_gpu_mod, sg_gpu_ptx)) return 1;
    if (cuModFn(&sg_cu_fn[SG_K_TT], sg_gpu_mod, "sg_ew_tt") ||
        cuModFn(&sg_cu_fn[SG_K_TS], sg_gpu_mod, "sg_ew_ts") ||
        cuModFn(&sg_cu_fn[SG_K_TI], sg_gpu_mod, "sg_ew_ti") ||
        cuModFn(&sg_cu_fn[SG_K_TSI], sg_gpu_mod, "sg_ew_tsi") ||
        cuModFn(&sg_cu_fn[SG_K_MM], sg_gpu_mod, "sg_mm") ||
        cuModFn(&sg_cu_fn[SG_K_SUM], sg_gpu_mod, "sg_sum") ||
        cuModFn(&sg_cu_fn[SG_K_SUMI], sg_gpu_mod, "sg_sum_i") ||
        cuModFn(&sg_cu_fn[SG_K_TR], sg_gpu_mod, "sg_tr") ||
        cuModFn(&sg_cu_fn[SG_K_REDF], sg_gpu_mod, "sg_red_f") ||
        cuModFn(&sg_cu_fn[SG_K_REDI], sg_gpu_mod, "sg_red_i") ||
        cuModFn(&sg_cu_fn[SG_K_UNF], sg_gpu_mod, "sg_un_f") ||
        cuModFn(&sg_cu_fn[SG_K_UNAI], sg_gpu_mod, "sg_un_ai") ||
        cuModFn(&sg_cu_fn[SG_K_UNSI], sg_gpu_mod, "sg_un_si") ||
        cuModFn(&sg_cu_fn[SG_K_CPY], sg_gpu_mod, "sg_cpy"))
        return 1;
    return 0;
}

/* ---- CUDA vtbl 实现（launch 自带 CtxSynchronize，返回即 kernel 完成） ---- */
static int sg_cu_be_alloc(void **out, size_t b) {
    CUdeviceptr d = 0;
    if (sg_cuMemAlloc(&d, b)) return 1;
    *out = (void *)d;
    return 0;
}
static void sg_cu_be_free(void *p) { sg_cuMemFree((CUdeviceptr)p); }
static int sg_cu_be_h2d(void *d, const void *h, size_t b) {
    return sg_cuMemcpyHtoD((CUdeviceptr)d, h, b);
}
static int sg_cu_be_d2h(void *h, const void *d, size_t b) {
    return sg_cuMemcpyDtoH(h, (CUdeviceptr)d, b);
}
static int sg_cu_be_launch1d(int k, int64_t n, void **args) {
    unsigned th = 256, bl = (unsigned)((n + 255) / 256);
    if (bl == 0) bl = 1;
    if (sg_cuLaunchKernel(sg_cu_fn[k], bl, 1, 1, th, 1, 1, 0, 0, args, NULL))
        return 1;
    return sg_cuCtxSynchronize();
}
static int sg_cu_be_launch2d(int k, int64_t gx, int64_t gy, void **args) {
    if (sg_cuLaunchKernel(sg_cu_fn[k], (unsigned)gx, (unsigned)gy, 1, 32, 32,
                          1, 0, 0, args, NULL))
        return 1;
    return sg_cuCtxSynchronize();
}

/* ---- OpenCL 后端（OpenCL.dll ICD loader + OpenCL C 源码运行时 JIT；
 * kernel 源 kGpuOpenCLC 与 sg_kernels.ll 一一镜像，.ll 为语义权威） ---- */
typedef struct _cl_platform_id *cl_platform_id;
typedef struct _cl_device_id *cl_device_id;
typedef struct _cl_context *cl_context;
typedef struct _cl_command_queue *cl_command_queue;
typedef struct _cl_program *cl_program;
typedef struct _cl_mem *cl_mem;
typedef struct _cl_kernel *cl_kernel;
static cl_context sg_ocl_ctx = NULL;
static cl_command_queue sg_ocl_q = NULL;
static cl_kernel sg_ocl_kn[SG_K_N];
/* 各 kernel 参数尺寸表（clSetKernelArg 用；指针=cl_mem 8B、u64/f64 8B、
 * u32 4B——与 try 函数 args 数组的实参类型逐一对应） */
static const unsigned char sg_ocl_asz[SG_K_N][8] = {
    {8, 8, 8, 8, 4, 0, 0, 0}, /* TT:   out,a,b,n,op */
    {8, 8, 8, 8, 4, 4, 0, 0}, /* TS:   out,a,s,n,op,sleft */
    {8, 8, 8, 8, 4, 8, 0, 0}, /* TI:   out,a,b,n,op,status */
    {8, 8, 8, 8, 4, 4, 8, 0}, /* TSI:  out,a,s,n,op,sleft,status */
    {8, 8, 8, 8, 8, 8, 0, 0}, /* MM:   out,a,b,m,n,k */
    {8, 8, 8, 0, 0, 0, 0, 0}, /* SUM:  out,a,n */
    {8, 8, 8, 8, 0, 0, 0, 0}, /* SUMI: out,status,a,n */
    {8, 8, 8, 8, 0, 0, 0, 0}, /* TR:   out,a,m,n */
    {8, 8, 8, 4, 0, 0, 0, 0}, /* REDF: out,a,n,isMax */
    {8, 8, 8, 4, 0, 0, 0, 0}, /* REDI: out,a,n,isMax */
    {8, 8, 8, 4, 0, 0, 0, 0}, /* UNF:  out,a,n,fnid */
    {8, 8, 8, 0, 0, 0, 0, 0}, /* UNAI: out,a,n */
    {8, 8, 8, 0, 0, 0, 0, 0}, /* UNSI: out,a,n */
    {8, 8, 8, 8, 0, 0, 0, 0}  /* CPY:  out,a,off,n */
};
static const unsigned char sg_ocl_an[SG_K_N] = {5, 6, 6, 7, 6, 3, 4, 4,
                                                4, 4, 4, 3, 3, 4};
static int (*sg_clGetPlatformIDs)(unsigned, cl_platform_id *, unsigned *);
static int (*sg_clGetDeviceIDs)(cl_platform_id, unsigned long long, unsigned,
                                cl_device_id *, unsigned *);
static int (*sg_clGetDeviceInfo)(cl_device_id, unsigned, size_t, void *,
                                 size_t *);
static cl_context (*sg_clCreateContext)(const intptr_t *, unsigned,
                                        cl_device_id *, void *, void *, int *);
static cl_command_queue (*sg_clCreateCommandQueue)(cl_context, cl_device_id,
                                                   unsigned long long, int *);
static cl_program (*sg_clCreateProgramWithSource)(cl_context, unsigned,
                                                  const char **, const size_t *,
                                                  int *);
static int (*sg_clBuildProgram)(cl_program, unsigned, cl_device_id *,
                                const char *, void *, void *);
static cl_kernel (*sg_clCreateKernel)(cl_program, const char *, int *);
static int (*sg_clSetKernelArg)(cl_kernel, unsigned, size_t, const void *);
static int (*sg_clEnqueueNDRangeKernel)(cl_command_queue, cl_kernel, unsigned,
                                        const size_t *, const size_t *,
                                        const size_t *, unsigned, void *,
                                        void *);
static int (*sg_clEnqueueWriteBuffer)(cl_command_queue, cl_mem, unsigned,
                                      size_t, size_t, const void *, unsigned,
                                      void *, void *);
static int (*sg_clEnqueueReadBuffer)(cl_command_queue, cl_mem, unsigned,
                                     size_t, size_t, void *, unsigned, void *,
                                     void *);
static int (*sg_clFinish)(cl_command_queue);
static int (*sg_clReleaseMemObject)(cl_mem);
static cl_mem (*sg_clCreateBuffer)(cl_context, unsigned long long, size_t,
                                   void *, int *);

static int sg_ocl_be_alloc(void **out, size_t b) {
    int err = 0;
    cl_mem m = sg_clCreateBuffer(sg_ocl_ctx, 4ull /*CL_MEM_READ_WRITE*/,
                                 b ? b : 1, NULL, &err);
    if (!m || err) return 1;
    *out = (void *)m;
    return 0;
}
static void sg_ocl_be_free(void *p) { sg_clReleaseMemObject((cl_mem)p); }
static int sg_ocl_be_h2d(void *d, const void *h, size_t b) {
    /* 阻塞传输（CL_TRUE）：与 vtbl「调用返回即数据就位」纪律一致 */
    return sg_clEnqueueWriteBuffer(sg_ocl_q, (cl_mem)d, 1, 0, b, h, 0, NULL,
                                   NULL)
               ? 1
               : 0;
}
static int sg_ocl_be_d2h(void *h, const void *d, size_t b) {
    return sg_clEnqueueReadBuffer(sg_ocl_q, (cl_mem)d, 1, 0, b, h, 0, NULL,
                                  NULL)
               ? 1
               : 0;
}
static int sg_ocl_launch(int k, unsigned dim, const size_t *gws,
                         const size_t *lws, void **args) {
    cl_kernel kn = sg_ocl_kn[k];
    for (int i = 0; i < sg_ocl_an[k]; ++i)
        if (sg_clSetKernelArg(kn, (unsigned)i, sg_ocl_asz[k][i], args[i]))
            return 1;
    if (sg_clEnqueueNDRangeKernel(sg_ocl_q, kn, dim, NULL, gws, lws, 0, NULL,
                                  NULL))
        return 1;
    return sg_clFinish(sg_ocl_q) ? 1 : 0;
}
static int sg_ocl_be_launch1d(int k, int64_t n, void **args) {
    size_t g = (size_t)((n + 255) / 256) * 256;
    if (g == 0) g = 256; /* kernel 侧 idx>=n 守卫，多余线程空转 */
    return sg_ocl_launch(k, 1, &g, NULL, args);
}
static int sg_ocl_be_launch2d(int k, int64_t gx, int64_t gy, void **args) {
    size_t g[2] = {(size_t)gx * 32, (size_t)gy * 32};
    size_t l[2] = {32, 32}; /* sg_mm tiling：local 必须 32×32 */
    return sg_ocl_launch(k, 2, g, l, args);
}

static void sg_opencl_init(void) {
    HMODULE h = LoadLibraryA("OpenCL.dll");
    if (!h) return;
    sg_clGetPlatformIDs = (void *)GetProcAddress(h, "clGetPlatformIDs");
    sg_clGetDeviceIDs = (void *)GetProcAddress(h, "clGetDeviceIDs");
    sg_clGetDeviceInfo = (void *)GetProcAddress(h, "clGetDeviceInfo");
    sg_clCreateContext = (void *)GetProcAddress(h, "clCreateContext");
    sg_clCreateCommandQueue = (void *)GetProcAddress(h, "clCreateCommandQueue");
    sg_clCreateProgramWithSource =
        (void *)GetProcAddress(h, "clCreateProgramWithSource");
    sg_clBuildProgram = (void *)GetProcAddress(h, "clBuildProgram");
    sg_clCreateKernel = (void *)GetProcAddress(h, "clCreateKernel");
    sg_clSetKernelArg = (void *)GetProcAddress(h, "clSetKernelArg");
    sg_clEnqueueNDRangeKernel =
        (void *)GetProcAddress(h, "clEnqueueNDRangeKernel");
    sg_clEnqueueWriteBuffer = (void *)GetProcAddress(h, "clEnqueueWriteBuffer");
    sg_clEnqueueReadBuffer = (void *)GetProcAddress(h, "clEnqueueReadBuffer");
    sg_clFinish = (void *)GetProcAddress(h, "clFinish");
    sg_clReleaseMemObject = (void *)GetProcAddress(h, "clReleaseMemObject");
    sg_clCreateBuffer = (void *)GetProcAddress(h, "clCreateBuffer");
    if (!sg_clGetPlatformIDs || !sg_clGetDeviceIDs || !sg_clGetDeviceInfo ||
        !sg_clCreateContext || !sg_clCreateCommandQueue ||
        !sg_clCreateProgramWithSource || !sg_clBuildProgram ||
        !sg_clCreateKernel || !sg_clSetKernelArg || !sg_clEnqueueNDRangeKernel ||
        !sg_clEnqueueWriteBuffer || !sg_clEnqueueReadBuffer || !sg_clFinish ||
        !sg_clReleaseMemObject || !sg_clCreateBuffer)
        return;
    unsigned np = 0;
    if (sg_clGetPlatformIDs(0, NULL, &np) || np == 0) return;
    cl_platform_id *plats =
        (cl_platform_id *)malloc(sizeof(cl_platform_id) * np);
    if (!plats) return;
    if (sg_clGetPlatformIDs(np, plats, NULL)) {
        free(plats);
        return;
    }
    /* 取首个带 fp64 的 GPU 设备（CL_DEVICE_TYPE_GPU 过滤 CPU 设备；
     * 无 fp64 → 覆盖集需 f64 → CPU 回退） */
    cl_device_id dev = NULL;
    for (unsigned i = 0; i < np && !dev; ++i) {
        cl_device_id d = NULL;
        unsigned nd = 0;
        if (sg_clGetDeviceIDs(plats[i], 4 /*CL_DEVICE_TYPE_GPU*/, 1, &d, &nd) ||
            nd == 0)
            continue;
        unsigned long long fp64 = 0;
        if (sg_clGetDeviceInfo(d, 0x0F02 /*CL_DEVICE_DOUBLE_FP_CONFIG*/,
                               sizeof(fp64), &fp64, NULL) || !fp64)
            continue;
        dev = d;
    }
    free(plats);
    if (!dev) return;
    int err = 0;
    sg_ocl_ctx = sg_clCreateContext(NULL, 1, &dev, NULL, NULL, &err);
    if (!sg_ocl_ctx || err) return;
    sg_ocl_q = sg_clCreateCommandQueue(sg_ocl_ctx, dev, 0, &err);
    if (!sg_ocl_q || err) return;
    const char *src = sg_gpu_oclc;
    size_t srclen = strlen(src);
    cl_program prog =
        sg_clCreateProgramWithSource(sg_ocl_ctx, 1, &src, &srclen, &err);
    if (!prog || err) return;
    if (sg_clBuildProgram(prog, 1, &dev, "-cl-std=CL1.2 -cl-fmad=false", NULL,
                          NULL))
        return;
    {
        static const char *kn[SG_K_N] = {"sg_ew_tt", "sg_ew_ts", "sg_ew_ti",
                                         "sg_ew_tsi", "sg_mm", "sg_sum",
                                         "sg_sum_i", "sg_tr", "sg_red_f",
                                         "sg_red_i", "sg_un_f", "sg_un_ai",
                                         "sg_un_si", "sg_cpy"};
        for (int i = 0; i < SG_K_N; ++i) {
            sg_ocl_kn[i] = sg_clCreateKernel(prog, kn[i], &err);
            if (!sg_ocl_kn[i]) return;
        }
    }
    sg_be.alloc = sg_ocl_be_alloc;
    sg_be.mfree = sg_ocl_be_free;
    sg_be.h2d = sg_ocl_be_h2d;
    sg_be.d2h = sg_ocl_be_d2h;
    sg_be.launch1d = sg_ocl_be_launch1d;
    sg_be.launch2d = sg_ocl_be_launch2d;
    sg_tsync_fn = sg_tsync_impl;
    sg_tdirty_fn = sg_tdirty_impl;
    sg_gpu_state = 1;
}

/* 后端选择：SCOURGE_GPU_BACKEND=cuda|opencl|cpu（缺省自适应——NVIDIA 优先
 * CUDA PTX 路径，失败/缺失落 OpenCL fp64 GPU，再失败 CPU）；SCOURGE_NO_GPU=1
 * 恒 CPU（逃生门兼回退测试钩子）。后端选择发生在运行时，不进构建缓存键。 */
static void sg_gpu_init(void) {
    sg_gpu_state = 0;
    if (getenv("SCOURGE_NO_GPU")) return;
    const char *be = getenv("SCOURGE_GPU_BACKEND");
    int tryCuda = 1, tryOcl = 1;
    if (be) {
        if (!strcmp(be, "cuda"))
            tryOcl = 0;
        else if (!strcmp(be, "opencl"))
            tryCuda = 0;
        else if (!strcmp(be, "cpu"))
            tryCuda = tryOcl = 0; /* 其他值：按缺省自适应 */
    }
    if (tryCuda && !sg_cuda_init()) {
        sg_be.alloc = sg_cu_be_alloc;
        sg_be.mfree = sg_cu_be_free;
        sg_be.h2d = sg_cu_be_h2d;
        sg_be.d2h = sg_cu_be_d2h;
        sg_be.launch1d = sg_cu_be_launch1d;
        sg_be.launch2d = sg_cu_be_launch2d;
        sg_tsync_fn = sg_tsync_impl;
        sg_tdirty_fn = sg_tdirty_impl;
        sg_gpu_state = 1;
        return;
    }
    if (tryOcl) sg_opencl_init();
}

static int sg_gpu_ready(void) {
    if (sg_gpu_state < 0) sg_gpu_init();
    return sg_gpu_state;
}

/* ---- 设备缓冲复用 + 惰性回拷（LRU 容量管理） ----
 * SgTensor.dev 非 0 = 设备驻留副本；hostStale=1 = 设备比 host 新（GPU 输出
 * 延迟回拷，host 读入口经 sg_tsync_impl 回拷）。不变量：dev 非 0 时设备副本
 * 恒为最新——host 写入走 sg_tdirty_impl 整体失效释放，故输入复用无需比对。
 * 注册表只做容量管理：驻留总量超 SCOURGE_GPU_CACHE_MB（默认 512MB）按 LRU
 * 逐出（hostStale 条目先回拷——数据唯一副本在设备）。释放纪律与 host arena
 * 一致：进程生命周期持有，仅容量/失效路径释放（进程退出由驱动统一回收）。 */
typedef struct {
    SgTensor *t;
    void *d;
    size_t bytes;
    uint64_t use;
} DevEnt;
static DevEnt *sg_devtab = NULL;
static int sg_devn = 0, sg_devcap = 0;
static size_t sg_devbytes = 0;
static uint64_t sg_devclk = 0;

/* 张量数据字节数（i64/f64 元素均 8 字节）；空张量占位 8 字节哑分配 */
static size_t sg_dev_bytes(const SgTensor *t) {
    size_t b = (size_t)sg_tsize(t) * 8;
    return b ? b : 8;
}

/* 预算：SCOURGE_GPU_CACHE_MB（MB），非法/未设取 512 */
static size_t sg_dev_budget(void) {
    static size_t budget = 0;
    if (!budget) {
        const char *e = getenv("SCOURGE_GPU_CACHE_MB");
        long v = e ? atol(e) : 512;
        if (v <= 0) v = 512;
        budget = (size_t)v * 1024 * 1024;
    }
    return budget;
}

/* LRU 逐出：驻留总量 + need 超预算时逐出最旧条目直至达标；p1/p2 为保护
 * 张量（本轮仍需其设备块的输入，NULL 可省）——受保护条目全时软预算放行。 */
static void sg_dev_evict(size_t need, const SgTensor *p1, const SgTensor *p2) {
    size_t budget = sg_dev_budget();
    while (sg_devn > 0 && sg_devbytes + need > budget) {
        int old = -1;
        for (int k = 0; k < sg_devn; ++k) {
            SgTensor *tk = sg_devtab[k].t;
            if (tk == p1 || tk == p2) continue;
            if (old < 0 || sg_devtab[k].use < sg_devtab[old].use) old = k;
        }
        if (old < 0) return;
        SgTensor *t = sg_devtab[old].t;
        void *d = sg_devtab[old].d;
        if (t->hostStale) { /* 数据唯一副本在设备：先回拷再释放 */
            void *h = t->isInt ? (void *)t->i64 : (void *)t->f64;
            if (sg_be.d2h(h, d, sg_devtab[old].bytes))
                sg_gpu_fail(); /* 上下文整体已坏（拷贝通道随 launch 级失败
                                  存活，实际不可达）； arena 纪律不恢复 */
            else
                t->hostStale = 0;
        }
        sg_be.mfree(d);
        t->dev = NULL;
        sg_devbytes -= sg_devtab[old].bytes;
        sg_devtab[old] = sg_devtab[--sg_devn];
    }
}

/* 登记（dget/devown 共用）：容量翻倍扩表，失败返回 0（调用方释放设备块） */
static int sg_dev_reg(SgTensor *t, void *d, size_t bytes, int stale) {
    if (sg_devn == sg_devcap) {
        int cap = sg_devcap ? sg_devcap * 2 : 64;
        DevEnt *nt = (DevEnt *)realloc(sg_devtab, (size_t)cap * sizeof(DevEnt));
        if (!nt) return 0;
        sg_devtab = nt;
        sg_devcap = cap;
    }
    sg_devtab[sg_devn].t = t;
    sg_devtab[sg_devn].d = d;
    sg_devtab[sg_devn].bytes = bytes;
    sg_devtab[sg_devn].use = ++sg_devclk;
    ++sg_devn;
    sg_devbytes += bytes;
    t->dev = d;
    t->hostStale = stale;
    return 1;
}

/* dget：取输入张量当前设备块——已驻留直接复用（含 hostStale：设备仍最新，
 * 免拷贝吃链），未驻留上传并登记。prot 为本轮已取得、kernel 尚未运行、
 * 不可被逐出的输入张量（NULL 可省）；失败返回 0 → 调用方整体回退 CPU。 */
static void *sg_gpu_dget(SgTensor *t, const SgTensor *prot) {
    size_t bytes = sg_dev_bytes(t);
    if (t->dev) {
        for (int k = 0; k < sg_devn; ++k)
            if (sg_devtab[k].t == t) sg_devtab[k].use = ++sg_devclk;
        return t->dev;
    }
    sg_dev_evict(bytes, prot, NULL);
    void *h = t->isInt ? (void *)t->i64 : (void *)t->f64;
    void *d = NULL;
    if (sg_be.alloc(&d, bytes) || sg_be.h2d(d, h, bytes)) {
        if (d) sg_be.mfree(d);
        return 0;
    }
    if (!sg_dev_reg(t, d, bytes, 0)) {
        sg_be.mfree(d);
        return 0;
    }
    return d;
}

/* devown：GPU 输出设备块归属张量（hostStale=1 惰性回拷）。失败释放并返回
 * 0 → 调用方回退 CPU 重算（此刻 kernel 已同步完成，输入设备块仍有效缓存）。 */
static int sg_gpu_devown(SgTensor *t, void *d, size_t bytes) {
    sg_dev_evict(bytes, NULL, NULL); /* kernel 已同步：输入设备块可逐出 */
    if (!sg_dev_reg(t, d, bytes, 1)) {
        sg_be.mfree(d);
        return 0;
    }
    return 1;
}

/* tsync 实现：host 读入口回拷（仅 hostStale 时有一次 DtoH，其余零开销） */
static void sg_tsync_impl(SgTensor *t) {
    if (!t->dev || !t->hostStale) return;
    void *h = t->isInt ? (void *)t->i64 : (void *)t->f64;
    if (sg_be.d2h(h, t->dev, sg_dev_bytes(t)))
        sg_gpu_fail(); /* 同上：上下文整体已坏，实际不可达 */
    else
        t->hostStale = 0;
}

/* tdirty 实现：host 写入口整体失效（host 为新权威，设备副本释放） */
static void sg_tdirty_impl(SgTensor *t) {
    if (!t->dev) return;
    sg_be.mfree(t->dev);
    for (int k = 0; k < sg_devn; ++k) {
        if (sg_devtab[k].t == t) {
            sg_devbytes -= sg_devtab[k].bytes;
            sg_devtab[k] = sg_devtab[--sg_devn];
            break;
        }
    }
    t->dev = NULL;
    t->hostStale = 0;
}

)GP";

} // namespace scourge
