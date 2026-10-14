;; sg_kernels.ll — Scourge CUDA 第 1 层 GPU kernel（f64 静态形状覆盖集）
;; 五个 kernel，形状全参数化（一个 kernel 服务所有静态形状）：
;;   sg_ew_tt(out, a, b, n, op)        逐元素 tt：out[i] = a[i] op b[i]，op 0/1/2
;;   sg_ew_ts(out, a, s, n, op, sleft) 逐元素 ts/st：sleft=0 → a[i] op s；1 → s op a[i]
;;   sg_mm(out, a, b, m, n, k)         矩乘：out[i*n+j] = sum_p a[i*k+p]*b[p*n+j]（p 升序）
;;   sg_sum(out, a, n)                 归约：单线程升序 acc = fadd(acc, a[k])
;;   sg_tr(out, a, m, n)               2D 转置：out[i*m+j] = a[j*n+i]（a 为 m×n）
;; 确定性红线：所有浮点运算顺序与 CPU 路径（sg_tensor_* / affine 内联）逐位
;; 一致——mm/sum 串行升序归约，逐元素每元素恰一次 IEEE RN 运算。
;; kernel 标注 = 调用约定 ptx_kernel（LLVM 23 isKernelFunction 判定正解）。
target triple = "nvptx64-nvidia-cuda"

; ---------------------------------------------------------------- sg_ew_tt
define ptx_kernel void @sg_ew_tt(ptr %out, ptr %a, ptr %b, i64 %n, i32 %op) {
entry:
  %tid   = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  %ntid  = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  %ctaid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
  %t64   = zext i32 %tid to i64
  %n64   = zext i32 %ntid to i64
  %c64   = zext i32 %ctaid to i64
  %g     = mul i64 %c64, %n64
  %idx   = add i64 %g, %t64
  %cond  = icmp slt i64 %idx, %n
  br i1 %cond, label %do, label %exit

do:
  %pa = getelementptr double, ptr %a, i64 %idx
  %pb = getelementptr double, ptr %b, i64 %idx
  %va = load double, ptr %pa, align 8
  %vb = load double, ptr %pb, align 8
  switch i32 %op, label %exit [
    i32 0, label %add
    i32 1, label %sub
    i32 2, label %mul
  ]

add:
  %r0 = fadd double %va, %vb
  br label %wr

sub:
  %r1 = fsub double %va, %vb
  br label %wr

mul:
  %r2 = fmul double %va, %vb
  br label %wr

wr:
  %r = phi double [ %r0, %add ], [ %r1, %sub ], [ %r2, %mul ]
  %po = getelementptr double, ptr %out, i64 %idx
  store double %r, ptr %po, align 8
  br label %exit

exit:
  ret void
}

; ---------------------------------------------------------------- sg_ew_ts
define ptx_kernel void @sg_ew_ts(ptr %out, ptr %a, double %s, i64 %n, i32 %op, i32 %sleft) {
entry:
  %tid   = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  %ntid  = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  %ctaid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
  %t64   = zext i32 %tid to i64
  %n64   = zext i32 %ntid to i64
  %c64   = zext i32 %ctaid to i64
  %g     = mul i64 %c64, %n64
  %idx   = add i64 %g, %t64
  %cond  = icmp slt i64 %idx, %n
  br i1 %cond, label %do, label %exit

do:
  %pa = getelementptr double, ptr %a, i64 %idx
  %va = load double, ptr %pa, align 8
  %sl = icmp ne i32 %sleft, 0
  ; sleft=0（ts）：x=a[i], y=s；sleft=1（st）：x=s, y=a[i]
  %x = select i1 %sl, double %s, double %va
  %y = select i1 %sl, double %va, double %s
  switch i32 %op, label %exit [
    i32 0, label %add
    i32 1, label %sub
    i32 2, label %mul
  ]

add:
  %r0 = fadd double %x, %y
  br label %wr

sub:
  %r1 = fsub double %x, %y
  br label %wr

mul:
  %r2 = fmul double %x, %y
  br label %wr

wr:
  %r = phi double [ %r0, %add ], [ %r1, %sub ], [ %r2, %mul ]
  %po = getelementptr double, ptr %out, i64 %idx
  store double %r, ptr %po, align 8
  br label %exit

exit:
  ret void
}

; ---------------------------------------------------------------- sg_mm
define ptx_kernel void @sg_mm(ptr %out, ptr %a, ptr %b, i64 %m, i64 %n, i64 %k) {
entry:
  %tid   = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  %ntid  = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  %ctaid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
  %t64   = zext i32 %tid to i64
  %n64   = zext i32 %ntid to i64
  %c64   = zext i32 %ctaid to i64
  %g     = mul i64 %c64, %n64
  %idx   = add i64 %g, %t64
  %cnt   = mul i64 %m, %n
  %cond  = icmp slt i64 %idx, %cnt
  br i1 %cond, label %do, label %exit

do:
  %i = sdiv i64 %idx, %n
  %j = srem i64 %idx, %n
  br label %lp

lp:
  %p   = phi i64 [ 0, %do ], [ %p1, %body ]
  %acc = phi double [ 0.000000e+00, %do ], [ %acc1, %body ]
  %cnd = icmp slt i64 %p, %k
  br i1 %cnd, label %body, label %done

body:
  %ai  = mul i64 %i, %k
  %ai2 = add i64 %ai, %p
  %pa  = getelementptr double, ptr %a, i64 %ai2
  %va  = load double, ptr %pa, align 8
  %bi  = mul i64 %p, %n
  %bi2 = add i64 %bi, %j
  %pb  = getelementptr double, ptr %b, i64 %bi2
  %vb  = load double, ptr %pb, align 8
  %pr  = fmul double %va, %vb
  ; CPU 同序：acc += a[i][p] * b[p][j]（fadd(acc, prod)）
  %acc1 = fadd double %acc, %pr
  %p1   = add i64 %p, 1
  br label %lp

done:
  %po = getelementptr double, ptr %out, i64 %idx
  store double %acc, ptr %po, align 8
  br label %exit

exit:
  ret void
}

; ---------------------------------------------------------------- sg_sum
define ptx_kernel void @sg_sum(ptr %out, ptr %a, i64 %n) {
entry:
  %tid   = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  %ctaid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
  %t0    = icmp eq i32 %tid, 0
  %c0    = icmp eq i32 %ctaid, 0
  %go    = and i1 %t0, %c0
  br i1 %go, label %lp, label %exit

lp:
  %k   = phi i64 [ 0, %entry ], [ %k1, %body ]
  %acc = phi double [ 0.000000e+00, %entry ], [ %acc1, %body ]
  %cnd = icmp slt i64 %k, %n
  br i1 %cnd, label %body, label %done

body:
  %pk   = getelementptr double, ptr %a, i64 %k
  %v    = load double, ptr %pk, align 8
  ; CPU 同序：acc += t->f64[k]（fadd(acc, v)）
  %acc1 = fadd double %acc, %v
  %k1   = add i64 %k, 1
  br label %lp

done:
  store double %acc, ptr %out, align 8
  br label %exit

exit:
  ret void
}

; ---------------------------------------------------------------- sg_tr
define ptx_kernel void @sg_tr(ptr %out, ptr %a, i64 %m, i64 %n) {
entry:
  %tid   = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  %ntid  = call i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
  %ctaid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
  %t64   = zext i32 %tid to i64
  %n64   = zext i32 %ntid to i64
  %c64   = zext i32 %ctaid to i64
  %g     = mul i64 %c64, %n64
  %idx   = add i64 %g, %t64
  %cnt   = mul i64 %m, %n
  %cond  = icmp slt i64 %idx, %cnt
  br i1 %cond, label %do, label %exit

do:
  ; a 为 m×n 行主（m 行 n 列）；out 为 n×m。
  ; idx → i = idx/m（out 行 = 原列），j = idx%m（out 列 = 原行）
  %i  = sdiv i64 %idx, %m
  %j  = srem i64 %idx, %m
  %ai = mul i64 %j, %n
  %ai2 = add i64 %ai, %i
  %pa = getelementptr double, ptr %a, i64 %ai2
  %v  = load double, ptr %pa, align 8
  %po = getelementptr double, ptr %out, i64 %idx
  store double %v, ptr %po, align 8
  br label %exit

exit:
  ret void
}

declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
declare i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
