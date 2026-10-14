#include <stdio.h>
#include <windows.h>
typedef int CUresult; typedef unsigned long long CUdeviceptr;
int main(void){
  HMODULE h = LoadLibraryA("nvcuda.dll");
  char *names[] = {"cuMemAlloc","cuMemAlloc_v2","cuMemcpyHtoD","cuMemcpyHtoD_v2","cuMemcpyDtoH","cuMemcpyDtoH_v2","cuMemFree","cuMemFree_v2","cuDevicePrimaryCtxRetain","cuCtxSetCurrent",0};
  for (int i=0;names[i];++i) printf("%-26s %p\n", names[i], (void*)GetProcAddress(h,names[i]));
  return 0;
}
