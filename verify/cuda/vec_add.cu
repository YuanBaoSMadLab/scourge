// vec_add.cu — GPU base verification (bypass LLVM, nvcc direct cubin).
// ASCII-only: cicc reads source in the system codepage, multibyte comments
// can swallow the trailing newline (same failure mode as PS 5.1 scripts).
extern "C" __global__ void vec_add(const double *a, const double *b,
                                   double *c, long long n) {
    long long i = (long long)blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        c[i] = a[i] + b[i];
}
