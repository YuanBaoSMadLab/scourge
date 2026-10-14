/* cuda_probe.c — CUDA driver API 最小探针（动态加载 nvcuda.dll，零链接依赖）
 * 加载 vec_add.cubin，2 块 x 4 线程算 c=a+b（n=8），验证 f64 结果。
 */
#include <stdio.h>
#include <windows.h>

typedef int CUresult;
typedef struct CUctx_st *CUcontext;
typedef struct CUmod_st *CUmodule;
typedef struct CUfunc_st *CUfunction;
typedef unsigned long long CUdeviceptr;

#define CK(x)                                                          \
    do {                                                               \
        CUresult r_ = (x);                                             \
        if (r_ != 0) {                                                 \
            printf("FAIL CUDA error %d @ line %d\n", (int)r_,          \
                   __LINE__);                                          \
            return 1;                                                  \
        }                                                              \
    } while (0)

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: cuda_probe <cubin>\n");
        return 2;
    }
    HMODULE h = LoadLibraryA("nvcuda.dll");
    if (!h) {
        printf("FAIL: nvcuda.dll not found\n");
        return 1;
    }
    CUresult (*cuInit)(unsigned) =
        (void *)GetProcAddress(h, "cuInit");
    CUresult (*cuDeviceGet)(int *, int) =
        (void *)GetProcAddress(h, "cuDeviceGet");
    CUresult (*cuDeviceGetName)(char *, int, int) =
        (void *)GetProcAddress(h, "cuDeviceGetName");
    CUresult (*cuDevicePrimaryCtxRetain)(CUcontext *, int) =
        (void *)GetProcAddress(h, "cuDevicePrimaryCtxRetain");
    CUresult (*cuCtxSetCurrent)(CUcontext) =
        (void *)GetProcAddress(h, "cuCtxSetCurrent");
    CUresult (*cuCtxSynchronize)(void) =
        (void *)GetProcAddress(h, "cuCtxSynchronize");
    CUresult (*cuModuleLoadData)(CUmodule *, const void *) =
        (void *)GetProcAddress(h, "cuModuleLoadData");
    CUresult (*cuModuleGetFunction)(CUfunction *, CUmodule,
                                    const char *) =
        (void *)GetProcAddress(h, "cuModuleGetFunction");
    /* 内存/拷贝类必须用 _v2 导出：无后缀别名是 3.2 时代 32 位 CUdeviceptr
     * 遗留桩，在 >4GB GPU VA 上返回 CUDA_ERROR_INVALID_CONTEXT(201)。 */
    CUresult (*cuMemAlloc)(CUdeviceptr *, size_t) =
        (void *)GetProcAddress(h, "cuMemAlloc_v2");
    CUresult (*cuMemFree)(CUdeviceptr) =
        (void *)GetProcAddress(h, "cuMemFree_v2");
    CUresult (*cuMemcpyHtoD)(CUdeviceptr, const void *, size_t) =
        (void *)GetProcAddress(h, "cuMemcpyHtoD_v2");
    CUresult (*cuMemcpyDtoH)(void *, CUdeviceptr, size_t) =
        (void *)GetProcAddress(h, "cuMemcpyDtoH_v2");
    CUresult (*cuLaunchKernel)(CUfunction, unsigned, unsigned,
                               unsigned, unsigned, unsigned, unsigned,
                               unsigned, CUdeviceptr, void **, void ***) =
        (void *)GetProcAddress(h, "cuLaunchKernel");
    CUresult (*cuModuleUnload)(CUmodule) =
        (void *)GetProcAddress(h, "cuModuleUnload");
    CUresult (*cuDevicePrimaryCtxRelease)(int) =
        (void *)GetProcAddress(h, "cuDevicePrimaryCtxRelease");
    if (!cuInit || !cuDeviceGet || !cuModuleLoadData || !cuLaunchKernel ||
        !cuMemAlloc || !cuMemcpyHtoD || !cuMemcpyDtoH) {
        printf("FAIL: driver API symbols missing\n");
        return 1;
    }

    CK(cuInit(0));
    int dev = 0;
    CK(cuDeviceGet(&dev, 0));
    char name[256] = {0};
    cuDeviceGetName(name, sizeof(name), dev);
    printf("device: %s\n", name);

    CUcontext ctx = 0;
    CUresult rr = cuDevicePrimaryCtxRetain(&ctx, dev);
    printf("retain ret=%d ctx=%p\n", (int)rr, (void *)ctx);
    CK(rr);
    CUresult (*cuCtxGetCurrent)(CUcontext *) =
        (void *)GetProcAddress(h, "cuCtxGetCurrent");
    CK(cuCtxSetCurrent(ctx));
    CUcontext cur = 0;
    cuCtxGetCurrent(&cur);
    printf("setcurrent ok, current=%p\n", (void *)cur);

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        printf("FAIL: cannot open cubin\n");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *img = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)sz);
    if (!img || fread(img, 1, (size_t)sz, f) != (size_t)sz) {
        printf("FAIL: read cubin\n");
        return 1;
    }
    fclose(f);

    CUmodule mod = 0;
    /* JIT 路线：传 PTX 文本（<file>.ptx）；cubin 路线：传二进制。 */
    const char *dot = strrchr(argv[1], '.');
    int isPtx = dot && _stricmp(dot, ".ptx") == 0;
    if (isPtx) {
        FILE *tf = fopen(argv[1], "rb");
        if (!tf) {
            printf("FAIL: cannot open ptx\n");
            return 1;
        }
        fseek(tf, 0, SEEK_END);
        long tsz = ftell(tf);
        fseek(tf, 0, SEEK_SET);
        char *txt = (char *)HeapAlloc(GetProcessHeap(), 0x08 /*ZERO*/, (size_t)tsz + 1);
        if (!txt || fread(txt, 1, (size_t)tsz, tf) != (size_t)tsz) {
            printf("FAIL: read ptx\n");
            return 1;
        }
        fclose(tf);
        CK(cuModuleLoadData(&mod, txt));
    } else {
        CK(cuModuleLoadData(&mod, img));
    }
    CUfunction fn = 0;
    CK(cuModuleGetFunction(&fn, mod, "vec_add"));

    const long n = 8;
    double ha[8], hb[8], hc[8];
    for (long i = 0; i < n; ++i) {
        ha[i] = (double)i + 1.5;
        hb[i] = (double)i * 10.0;
    }
    CUdeviceptr da = 0, db = 0, dc = 0;
    CK(cuMemAlloc(&da, sizeof(ha)));
    CK(cuMemAlloc(&db, sizeof(hb)));
    CK(cuMemAlloc(&dc, sizeof(hc)));
    CK(cuMemcpyHtoD(da, ha, sizeof(ha)));
    CK(cuMemcpyHtoD(db, hb, sizeof(hb)));

    void *params[4] = {&da, &db, &dc, (void *)&n};
    CK(cuLaunchKernel(fn, 2, 1, 1, 4, 1, 1, 0, (CUdeviceptr)0, params, 0));
    CK(cuCtxSynchronize());
    CK(cuMemcpyDtoH(hc, dc, sizeof(hc)));

    int bad = 0;
    for (long i = 0; i < n; ++i) {
        double want = ha[i] + hb[i];
        if (hc[i] != want) {
            printf("FAIL c[%ld]=%f want %f\n", i, hc[i], want);
            bad = 1;
        }
    }
    if (!bad) {
        printf("PASS vec_add on GPU (n=8, 2x4 threads)\n");
        for (long i = 0; i < n; ++i) printf("  c[%ld] = %f\n", i, hc[i]);
    }

    cuMemFree(da);
    cuMemFree(db);
    cuMemFree(dc);
    cuModuleUnload(mod);
    cuDevicePrimaryCtxRelease(dev);
    return bad;
}
