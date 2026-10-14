#include <stdio.h>
#include <windows.h>
typedef int CUresult; typedef struct CUctx_st *CUcontext; typedef unsigned long long CUdeviceptr;
#define CK(x) do{int r_=(x); if(r_){printf("err %d @ %d\n", r_, __LINE__); return 1;}}while(0)
int main(void){
  HMODULE h = LoadLibraryA("nvcuda.dll");
  CUresult (*cuInit)(unsigned)=(void*)GetProcAddress(h,"cuInit");
  CUresult (*cuDeviceGet)(int*,int)=(void*)GetProcAddress(h,"cuDeviceGet");
  CUresult (*cuCtxRetain)(CUcontext*,int)=(void*)GetProcAddress(h,"cuDevicePrimaryCtxRetain");
  CUresult (*cuSetCtx)(CUcontext)=(void*)GetProcAddress(h,"cuCtxSetCurrent");
  CUresult (*cuMemA)(CUdeviceptr*,unsigned long long)=(void*)GetProcAddress(h,"cuMemAlloc_v2");
  CUresult (*cuMemF)(CUdeviceptr)=(void*)GetProcAddress(h,"cuMemFree_v2");
  CUdeviceptr p=0;
  CK(cuInit(0));
  int dev=0; CK(cuDeviceGet(&dev,0));
  CUcontext ctx=0; CK(cuCtxRetain(&ctx,dev)); CK(cuSetCtx(ctx));
  CUresult r = cuMemA(&p, 64);
  printf("MemAlloc_v2 ret=%d p=%llx\n", (int)r, p);
  if (r==0) cuMemF(p);
  return r!=0;
}
