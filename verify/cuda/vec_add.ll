;; vec_add.ll — 最小 NVVM kernel 探针（第 1 层链路验证，不入 git）
;; c[i] = a[i] + b[i]，i64 索引，f64 数据；kernel 标注 = 调用约定 ptx_kernel
;;（LLVM 23 isKernelFunction 判 CallingConv::PTX_Kernel，NVVMProperties.h:34；
;; 旧 !nvvm.annotations 元数据与 "nvvm.kernel" 属性均已不消费）。
target triple = "nvptx64-nvidia-cuda"

define ptx_kernel void @vec_add(ptr %a, ptr %b, ptr %c, i64 %n) {
entry:
  %tid   = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  %ntid  = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  %ctaid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
  %t64   = zext i32 %tid to i64
  %n64   = zext i32 %ntid to i64
  %c64   = zext i32 %ctaid to i64
  %base  = mul i64 %c64, %n64
  %idx   = add i64 %base, %t64
  %cond  = icmp slt i64 %idx, %n
  br i1 %cond, label %do, label %exit

do:
  %pa = getelementptr double, ptr %a, i64 %idx
  %pb = getelementptr double, ptr %b, i64 %idx
  %pc = getelementptr double, ptr %c, i64 %idx
  %va = load double, ptr %pa, align 8
  %vb = load double, ptr %pb, align 8
  %vc = fadd double %va, %vb
  store double %vc, ptr %pc, align 8
  br label %exit

exit:
  ret void
}

declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
declare i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
