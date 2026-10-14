/* sg_probe.c — Scourge GPU kernel standalone probe (PTX JIT via nvcuda.dll)
 * Exercises all 5 kernels from sg_kernels.ptx with known data and verifies
 * results against plain C loops (bitwise compare for f64).
 * ASCII comments only (cicc/nvcc codepage lesson does not apply to clang,
 * but keep uniform discipline).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
            printf("FAIL CUDA error %d @ %d\n", (int)r_, __LINE__);    \
            return 1;                                                  \
        }                                                              \
    } while (0)

static CUmodule mod;
static CUfunction f_tt, f_ts, f_mm, f_sum, f_tr;
static CUresult (*cuMemAllocP)(CUdeviceptr *, size_t);
static CUresult (*cuMemFreeP)(CUdeviceptr);
static CUresult (*cuMemcpyHtoDP)(CUdeviceptr, const void *, size_t);
static CUresult (*cuMemcpyDtoHP)(void *, CUdeviceptr, size_t);
static CUresult (*cuLaunchKernelP)(CUfunction, unsigned, unsigned, unsigned,
                                   unsigned, unsigned, unsigned, unsigned,
                                   CUdeviceptr, void **, void ***);
static CUresult (*cuCtxSynchronizeP)(void);

/* grid = ceil(n/256) blocks x 256 threads; sync after launch */
static int run1d(CUfunction f, long long n, void **args) {
    unsigned th = 256;
    unsigned bl = (unsigned)((n + 255) / 256);
    if (bl == 0)
        bl = 1;
    if (cuLaunchKernelP(f, bl, 1, 1, th, 1, 1, 0, 0, args, NULL))
        return 1;
    return cuCtxSynchronizeP();
}

static double da_in[16], db_in[16], ds = 2.5, d_out[16];
static double dmm_a[12], dmm_b[8], dmm_out[6]; /* 3x4 @ 4x2 -> 3x2 */
static double dsum_in[1000], dsum_out[1];
static double dtr_in[6], dtr_out[6]; /* 2x3 -> 3x2 */

static int eq(double a, double b) {
    return memcmp(&a, &b, 8) == 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: sg_probe <ptx>\n");
        return 2;
    }
    HMODULE h = LoadLibraryA("nvcuda.dll");
    if (!h) {
        printf("FAIL: nvcuda.dll not found\n");
        return 1;
    }
    CUresult (*cuInit)(unsigned) = (void *)GetProcAddress(h, "cuInit");
    CUresult (*cuDeviceGet)(int *, int) =
        (void *)GetProcAddress(h, "cuDeviceGet");
    CUresult (*cuDevicePrimaryCtxRetain)(CUcontext *, int) =
        (void *)GetProcAddress(h, "cuDevicePrimaryCtxRetain");
    CUresult (*cuCtxSetCurrent)(CUcontext) =
        (void *)GetProcAddress(h, "cuCtxSetCurrent");
    CUresult (*cuModuleLoadData)(CUmodule *, const void *) =
        (void *)GetProcAddress(h, "cuModuleLoadData");
    CUresult (*cuModuleGetFunction)(CUfunction *, CUmodule, const char *) =
        (void *)GetProcAddress(h, "cuModuleGetFunction");
    cuMemAllocP = (void *)GetProcAddress(h, "cuMemAlloc_v2");
    cuMemFreeP = (void *)GetProcAddress(h, "cuMemFree_v2");
    cuMemcpyHtoDP = (void *)GetProcAddress(h, "cuMemcpyHtoD_v2");
    cuMemcpyDtoHP = (void *)GetProcAddress(h, "cuMemcpyDtoH_v2");
    cuLaunchKernelP = (void *)GetProcAddress(h, "cuLaunchKernel");
    cuCtxSynchronizeP = (void *)GetProcAddress(h, "cuCtxSynchronize");
    if (!cuInit || !cuDeviceGet || !cuDevicePrimaryCtxRetain ||
        !cuCtxSetCurrent || !cuModuleLoadData || !cuModuleGetFunction ||
        !cuMemAllocP || !cuMemFreeP || !cuMemcpyHtoDP || !cuMemcpyDtoHP ||
        !cuLaunchKernelP || !cuCtxSynchronizeP) {
        printf("FAIL: driver API symbols missing\n");
        return 1;
    }
    CK(cuInit(0));
    int dev = 0;
    CK(cuDeviceGet(&dev, 0));
    CUcontext ctx = 0;
    CK(cuDevicePrimaryCtxRetain(&ctx, dev));
    CK(cuCtxSetCurrent(ctx));

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        printf("FAIL: cannot open ptx\n");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *txt = (char *)malloc((size_t)sz + 1);
    if (!txt || fread(txt, 1, (size_t)sz, f) != (size_t)sz) {
        printf("FAIL: read ptx\n");
        return 1;
    }
    txt[sz] = 0;
    fclose(f);
    CK(cuModuleLoadData(&mod, txt));
    CK(cuModuleGetFunction(&f_tt, mod, "sg_ew_tt"));
    CK(cuModuleGetFunction(&f_ts, mod, "sg_ew_ts"));
    CK(cuModuleGetFunction(&f_mm, mod, "sg_mm"));
    CK(cuModuleGetFunction(&f_sum, mod, "sg_sum"));
    CK(cuModuleGetFunction(&f_tr, mod, "sg_tr"));

    /* ---- case 1: sg_ew_tt, n=16, all three ops ---- */
    for (int i = 0; i < 16; ++i) {
        da_in[i] = (double)i - 3.0;
        db_in[i] = 0.5 * (double)i;
    }
    for (int op = 0; op <= 2; ++op) {
        CUdeviceptr ga = 0, gb = 0, go = 0;
        int32_t op32 = op;
        long long n = 16;
        void *args[] = {&go, &ga, &gb, &n, &op32};
        CK(cuMemAllocP(&ga, 16 * 8));
        CK(cuMemAllocP(&gb, 16 * 8));
        CK(cuMemAllocP(&go, 16 * 8));
        CK(cuMemcpyHtoDP(ga, da_in, 16 * 8));
        CK(cuMemcpyHtoDP(gb, db_in, 16 * 8));
        CK(run1d(f_tt, n, args));
        CK(cuMemcpyDtoHP(d_out, go, 16 * 8));
        cuMemFreeP(ga);
        cuMemFreeP(gb);
        cuMemFreeP(go);
        for (int i = 0; i < 16; ++i) {
            double want = op == 0 ? da_in[i] + db_in[i]
                          : op == 1 ? da_in[i] - db_in[i]
                                    : da_in[i] * db_in[i];
            if (!eq(d_out[i], want)) {
                printf("FAIL ew_tt op=%d i=%d got %.17g want %.17g\n", op, i,
                       d_out[i], want);
                return 1;
            }
        }
    }
    printf("PASS sg_ew_tt\n");

    /* ---- case 2: sg_ew_ts, both directions ---- */
    for (int op = 0; op <= 2; ++op) {
        for (int sleft = 0; sleft <= 1; ++sleft) {
            CUdeviceptr ga = 0, go = 0;
            int32_t op32 = op, sl32 = sleft;
            long long n = 16;
            void *args[] = {&go, &ga, &ds, &n, &op32, &sl32};
            CK(cuMemAllocP(&ga, 16 * 8));
            CK(cuMemAllocP(&go, 16 * 8));
            CK(cuMemcpyHtoDP(ga, da_in, 16 * 8));
            CK(run1d(f_ts, n, args));
            CK(cuMemcpyDtoHP(d_out, go, 16 * 8));
            cuMemFreeP(ga);
            cuMemFreeP(go);
            for (int i = 0; i < 16; ++i) {
                double x = sleft ? ds : da_in[i];
                double y = sleft ? da_in[i] : ds;
                double want = op == 0 ? x + y : op == 1 ? x - y : x * y;
                if (!eq(d_out[i], want)) {
                    printf("FAIL ew_ts op=%d sl=%d i=%d got %.17g want "
                           "%.17g\n",
                           op, sleft, i, d_out[i], want);
                    return 1;
                }
            }
        }
    }
    printf("PASS sg_ew_ts\n");

    /* ---- case 3: sg_mm, 3x4 @ 4x2 ---- */
    for (int i = 0; i < 12; ++i)
        dmm_a[i] = 0.1 * (double)(i + 1);
    for (int i = 0; i < 8; ++i)
        dmm_b[i] = (double)(i % 3) - 1.0;
    {
        CUdeviceptr ga = 0, gb = 0, go = 0;
        long long m = 3, n = 2, k = 4;
        void *args[] = {&go, &ga, &gb, &m, &n, &k};
        CK(cuMemAllocP(&ga, 12 * 8));
        CK(cuMemAllocP(&gb, 8 * 8));
        CK(cuMemAllocP(&go, 6 * 8));
        CK(cuMemcpyHtoDP(ga, dmm_a, 12 * 8));
        CK(cuMemcpyHtoDP(gb, dmm_b, 8 * 8));
        CK(run1d(f_mm, 3 * 2, args));
        CK(cuMemcpyDtoHP(dmm_out, go, 6 * 8));
        cuMemFreeP(ga);
        cuMemFreeP(gb);
        cuMemFreeP(go);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 2; ++j) {
                double acc = 0.0;
                for (int p = 0; p < 4; ++p)
                    acc += dmm_a[i * 4 + p] * dmm_b[p * 2 + j];
                if (!eq(dmm_out[i * 2 + j], acc)) {
                    printf("FAIL mm i=%d j=%d got %.17g want %.17g\n", i, j,
                           dmm_out[i * 2 + j], acc);
                    return 1;
                }
            }
    }
    printf("PASS sg_mm\n");

    /* ---- case 4: sg_sum, n=1000 ---- */
    for (int i = 0; i < 1000; ++i)
        dsum_in[i] = 0.001 * (double)(i + 1);
    {
        CUdeviceptr ga = 0, go = 0;
        long long n = 1000;
        void *args[] = {&go, &ga, &n};
        CK(cuMemAllocP(&ga, 1000 * 8));
        CK(cuMemAllocP(&go, 8));
        CK(cuMemcpyHtoDP(ga, dsum_in, 1000 * 8));
        CK(run1d(f_sum, 1, args));
        CK(cuMemcpyDtoHP(dsum_out, go, 8));
        cuMemFreeP(ga);
        cuMemFreeP(go);
        double acc = 0.0;
        for (int i = 0; i < 1000; ++i)
            acc += dsum_in[i];
        if (!eq(dsum_out[0], acc)) {
            printf("FAIL sum got %.17g want %.17g\n", dsum_out[0], acc);
            return 1;
        }
    }
    printf("PASS sg_sum\n");

    /* ---- case 5: sg_tr, 2x3 -> 3x2 ---- */
    for (int i = 0; i < 6; ++i)
        dtr_in[i] = (double)i + 0.25;
    {
        CUdeviceptr ga = 0, go = 0;
        long long m = 2, n = 3;
        void *args[] = {&go, &ga, &m, &n};
        CK(cuMemAllocP(&ga, 6 * 8));
        CK(cuMemAllocP(&go, 6 * 8));
        CK(cuMemcpyHtoDP(ga, dtr_in, 6 * 8));
        CK(run1d(f_tr, 6, args));
        CK(cuMemcpyDtoHP(dtr_out, go, 6 * 8));
        cuMemFreeP(ga);
        cuMemFreeP(go);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 2; ++j) {
                double want = dtr_in[j * 3 + i];
                if (!eq(dtr_out[i * 2 + j], want)) {
                    printf("FAIL tr i=%d j=%d got %.17g want %.17g\n", i, j,
                           dtr_out[i * 2 + j], want);
                    return 1;
                }
            }
    }
    printf("PASS sg_tr\n");

    printf("ALL PASS\n");
    return 0;
}
