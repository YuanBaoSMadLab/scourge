;; sg_kernels.ll — Scourge CUDA 第 1 层 GPU kernel（f64/int 静态形状覆盖集）
;; 十四个 kernel，形状全参数化（一个 kernel 服务所有静态形状）：
;;   sg_ew_tt(out, a, b, n, op)        逐元素 tt：out[i] = a[i] op b[i]，op 0/1/2
;;   sg_ew_ts(out, a, s, n, op, sleft) 逐元素 ts/st：sleft=0 → a[i] op s；1 → s op a[i]
;;   sg_ew_ti(out, a, b, n, op, st)    int tt：+ - * 溢出仿真，sadd/ssub/smul
;;                                     .with.overflow 与 CPU 内联展开同构，
;;                                     溢出经 atomicrmw or 上报 st（0=正常）
;;   sg_ew_tsi(out, a, s, n, op, sl,st) int ts/st：标量 i64，溢出上报同上
;;   sg_mm(out, a, b, m, n, k)         矩乘：out[i*n+j] = sum_p a[i*k+p]*b[p*n+j]（p 升序）
;;   sg_sum(out, a, n)                 归约：单线程升序 acc = fadd(acc, a[k])
;;   sg_sum_i(out, st, a, n)           int 归约：单线程升序 sadd.with.overflow，
;;                                     溢出 store st=1（单写者免原子）
;;   sg_tr(out, a, m, n)               2D 转置：out[i*m+j] = a[j*n+i]（a 为 m×n）
;;   sg_red_f(out, a, n, isMax)        f64 max/min：单线程 k 升序，与 CPU
;;                                     sg_tensor_reduce_f 同一比较序（三元
;;                                     select 左偏，NaN/±0 语义逐位一致）
;;   sg_red_i(out, a, n, isMax)        int max/min：同上（icmp sgt/slt）
;;   sg_un_f(out, a, n, fnid)          f64 一元：fnid 0=abs（fabs 逐位）1=sqrt
;;                                     （sqrt.rn.f64 正确舍入，与 libm 一致；
;;                                     exp/log 无硬件精确实现 → 不覆盖）
;;   sg_un_ai(out, a, n)               int abs：x<0?-x:x（与 CPU 同，含回绕）
;;   sg_un_si(out, a, n)               int 入 → sitofp rn → sqrt（f64 出）
;;   sg_cpy(out, a, off, n)            连续 8 字节元素拷贝（slice 设备内搬移，
;;                                     dtype 无关；覆盖 get 不需要的场景）
;; 确定性红线：所有浮点/整数运算顺序与 CPU 路径（sg_tensor_* / affine 内联）
;; 逐位一致——mm/sum 串行升序归约，逐元素每元素恰一次 IEEE RN 运算；int 溢出
;; 判定与 CPU __builtin_*_overflow（LangInlineIntRewrite 同款 intrinsic）逐位
;; 一致，kernel 照常写回回绕值，panic 由运行时回读状态后以同一 sg_panic 终止。
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

; ---------------------------------------------------------------- sg_ew_ti
; int tt 逐元素：+ - * 溢出仿真（与 CPU LangInlineIntRewrite / sg_add_i64 的
; __builtin_*_overflow 同一 intrinsic 语义）。溢出线程 atomicrmw or 置状态，
; 结果照常写回（回绕值随 panic 丢弃，不外泄）。
define ptx_kernel void @sg_ew_ti(ptr %out, ptr %a, ptr %b, i64 %n, i32 %op,
                                 ptr %status) {
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
  %pa = getelementptr i64, ptr %a, i64 %idx
  %pb = getelementptr i64, ptr %b, i64 %idx
  %va = load i64, ptr %pa, align 8
  %vb = load i64, ptr %pb, align 8
  switch i32 %op, label %exit [
    i32 0, label %add
    i32 1, label %sub
    i32 2, label %mul
  ]

add:
  %s0 = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %va, i64 %vb)
  br label %chk

sub:
  %s1 = call { i64, i1 } @llvm.ssub.with.overflow.i64(i64 %va, i64 %vb)
  br label %chk

mul:
  %s2 = call { i64, i1 } @llvm.smul.with.overflow.i64(i64 %va, i64 %vb)
  br label %chk

chk:
  %ov = phi { i64, i1 } [ %s0, %add ], [ %s1, %sub ], [ %s2, %mul ]
  %r = extractvalue { i64, i1 } %ov, 0
  %f = extractvalue { i64, i1 } %ov, 1
  br i1 %f, label %ovf, label %wr

ovf:
  atomicrmw or ptr %status, i32 1 monotonic
  br label %wr

wr:
  %po = getelementptr i64, ptr %out, i64 %idx
  store i64 %r, ptr %po, align 8
  br label %exit

exit:
  ret void
}

; --------------------------------------------------------------- sg_ew_tsi
; int ts/st 逐元素：标量为 i64；sleft=0 → a[i] op s（ts），1 → s op a[i]（st）。
; 溢出上报与 sg_ew_ti 同构。
define ptx_kernel void @sg_ew_tsi(ptr %out, ptr %a, i64 %s, i64 %n, i32 %op,
                                  i32 %sleft, ptr %status) {
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
  %pa = getelementptr i64, ptr %a, i64 %idx
  %va = load i64, ptr %pa, align 8
  %sl = icmp ne i32 %sleft, 0
  ; sleft=0（ts）：x=a[i], y=s；sleft=1（st）：x=s, y=a[i]
  %x = select i1 %sl, i64 %s, i64 %va
  %y = select i1 %sl, i64 %va, i64 %s
  switch i32 %op, label %exit [
    i32 0, label %add
    i32 1, label %sub
    i32 2, label %mul
  ]

add:
  %s0 = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %x, i64 %y)
  br label %chk

sub:
  %s1 = call { i64, i1 } @llvm.ssub.with.overflow.i64(i64 %x, i64 %y)
  br label %chk

mul:
  %s2 = call { i64, i1 } @llvm.smul.with.overflow.i64(i64 %x, i64 %y)
  br label %chk

chk:
  %ov = phi { i64, i1 } [ %s0, %add ], [ %s1, %sub ], [ %s2, %mul ]
  %r = extractvalue { i64, i1 } %ov, 0
  %f = extractvalue { i64, i1 } %ov, 1
  br i1 %f, label %ovf, label %wr

ovf:
  atomicrmw or ptr %status, i32 1 monotonic
  br label %wr

wr:
  %po = getelementptr i64, ptr %out, i64 %idx
  store i64 %r, ptr %po, align 8
  br label %exit

exit:
  ret void
}

; ---------------------------------------------------------------- sg_mm
; 矩乘 shared-memory tiling 32×32：block 32×32 线程、grid=(ceil(n/32), ceil(m/32))，
; block 协作把 a 的 32×32 片（行 row、p 列）与 b 的 32×32 片（p 行、列 col）搬入
; shared memory 后块内累加。确定性：每输出元素仍按 p=0..k-1 升序
; fadd(acc, a[i][p]*b[p][j])——tiling 只改数据搬运不改运算序，与 CPU
; sg_tensor_matmul 逐位一致；p>=k 的 padding 乘加结果经 select 丢弃
; （保 -0.0 符号）；tile 装载越界格读 (0,0) 安全地址、写 0.0（不参与有效累加）。
@sg_mm_at = internal addrspace(3) global [1024 x double] zeroinitializer
@sg_mm_bt = internal addrspace(3) global [1024 x double] zeroinitializer

define ptx_kernel void @sg_mm(ptr %out, ptr %a, ptr %b, i64 %m, i64 %n, i64 %k) {
entry:
  %tid   = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  %tidy  = call i32 @llvm.nvvm.read.ptx.sreg.tid.y()
  %ctax  = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
  %ctay  = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.y()
  %tx    = zext i32 %tid to i64
  %ty    = zext i32 %tidy to i64
  %cx    = zext i32 %ctax to i64
  %cy    = zext i32 %ctay to i64
  %cx32  = mul i64 %cx, 32
  %cy32  = mul i64 %cy, 32
  %row   = add i64 %cy32, %ty
  %col   = add i64 %cx32, %tx
  %rowok = icmp slt i64 %row, %m
  %colok = icmp slt i64 %col, %n
  %ok    = and i1 %rowok, %colok
  %tidx  = mul i64 %ty, 32
  %tidx2 = add i64 %tidx, %tx
  br label %blk

blk:
  %p0   = phi i64 [ 0, %entry ], [ %pn, %bar2 ]
  %acc0 = phi double [ 0.000000e+00, %entry ], [ %acc1, %bar2 ]
  %more = icmp slt i64 %p0, %k
  br i1 %more, label %load, label %done

load:
  %ac    = add i64 %p0, %tx
  %arok  = icmp slt i64 %row, %m
  %acok  = icmp slt i64 %ac, %k
  %al    = and i1 %arok, %acok
  %aidx  = mul i64 %row, %k
  %aidx2 = add i64 %aidx, %ac
  %asafe = select i1 %al, i64 %aidx2, i64 0
  %pa    = getelementptr double, ptr %a, i64 %asafe
  %va    = load double, ptr %pa, align 8
  %va0   = select i1 %al, double %va, double 0.000000e+00
  %brw   = add i64 %p0, %ty
  %bc    = add i64 %cx32, %tx
  %brok  = icmp slt i64 %brw, %k
  %bcok  = icmp slt i64 %bc, %n
  %bl    = and i1 %brok, %bcok
  %bidx  = mul i64 %brw, %n
  %bidx2 = add i64 %bidx, %bc
  %bsafe = select i1 %bl, i64 %bidx2, i64 0
  %pb    = getelementptr double, ptr %b, i64 %bsafe
  %vb    = load double, ptr %pb, align 8
  %vb0   = select i1 %bl, double %vb, double 0.000000e+00
  %pta   = getelementptr double, ptr addrspace(3) @sg_mm_at, i64 %tidx2
  store double %va0, ptr addrspace(3) %pta, align 8
  %ptb   = getelementptr double, ptr addrspace(3) @sg_mm_bt, i64 %tidx2
  store double %vb0, ptr addrspace(3) %ptb, align 8
  call void @llvm.nvvm.barrier0()
  br label %inner

inner:
  %p2   = phi i64 [ 0, %load ], [ %p2n, %inner ]
  %acc  = phi double [ %acc0, %load ], [ %acc1, %inner ]
  %p    = add i64 %p0, %p2
  %u3   = add i64 %tidx, %p2
  %qa   = getelementptr double, ptr addrspace(3) @sg_mm_at, i64 %u3
  %va2  = load double, ptr addrspace(3) %qa, align 8 ; a_tile[ty][p2]
  %q2   = mul i64 %p2, 32
  %q3   = add i64 %q2, %tx
  %qb   = getelementptr double, ptr addrspace(3) @sg_mm_bt, i64 %q3
  %vb2  = load double, ptr addrspace(3) %qb, align 8  ; b_tile[p2][tx]
  %pr   = fmul double %va2, %vb2
  ; CPU 同序：acc += a[i][p] * b[p][j]（fadd(acc, prod)，p 升序）；
  ; p >= k 的 padding 乘加经 select 丢弃，保 -0.0 符号逐位一致
  %f    = fadd double %acc, %pr
  %in   = icmp slt i64 %p, %k
  %acc1 = select i1 %in, double %f, double %acc
  %p2n  = add i64 %p2, 1
  %c2   = icmp ne i64 %p2n, 32
  br i1 %c2, label %inner, label %bar2

bar2:
  call void @llvm.nvvm.barrier0()
  %pn = add i64 %p0, 32
  br label %blk

done:
  %po2  = mul i64 %row, %n
  %po3  = add i64 %po2, %col
  %pout = getelementptr double, ptr %out, i64 %po3
  br i1 %ok, label %wr, label %exit

wr:
  store double %acc0, ptr %pout, align 8
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

; --------------------------------------------------------------- sg_sum_i
; int 归约：单线程 k 升序 sadd.with.overflow 累加（与 sg_tensor_sum_i 的
; __builtin_add_overflow 逐位一致）；溢出置状态后以回绕值继续（结果随
; panic 丢弃）。单写者普通 store 免原子。
define ptx_kernel void @sg_sum_i(ptr %out, ptr %status, ptr %a, i64 %n) {
entry:
  %tid   = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  %ctaid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
  %t0    = icmp eq i32 %tid, 0
  %c0    = icmp eq i32 %ctaid, 0
  %go    = and i1 %t0, %c0
  br i1 %go, label %lp, label %exit

lp:
  %k   = phi i64 [ 0, %entry ], [ %k1, %cont ]
  %acc = phi i64 [ 0, %entry ], [ %acc1, %cont ]
  %cnd = icmp slt i64 %k, %n
  br i1 %cnd, label %body, label %done

body:
  %pk = getelementptr i64, ptr %a, i64 %k
  %v  = load i64, ptr %pk, align 8
  ; CPU 同序：acc += t->i64[k]，溢出判定与 __builtin_add_overflow 一致
  %s  = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %acc, i64 %v)
  %acc1 = extractvalue { i64, i1 } %s, 0
  %f    = extractvalue { i64, i1 } %s, 1
  br i1 %f, label %ovf, label %cont

ovf:
  store i32 1, ptr %status, align 4
  br label %cont

cont:
  %k1 = add i64 %k, 1
  br label %lp

done:
  store i64 %acc, ptr %out, align 8
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

; ---------------------------------------------------------------- sg_red_f
; f64 max/min 归约：单线程 k 升序，与 CPU sg_tensor_reduce_f 同一比较序——
; CPU 语义 best = isMax ? (x > best ? x : best) : (x < best ? x : best)；
; fcmp ogt/olt 与 C >/ < 同为 NaN false，select 左偏与三元一致（NaN 仅在
; 首元素时驻留，±0 平局保左，逐位一致）；两路比较均无副作用，isMax 选择。
define ptx_kernel void @sg_red_f(ptr %out, ptr %a, i64 %n, i32 %isMax) {
entry:
  %tid   = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  %ctaid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
  %t0    = icmp eq i32 %tid, 0
  %c0    = icmp eq i32 %ctaid, 0
  %go    = and i1 %t0, %c0
  br i1 %go, label %init, label %exit

init:
  %pa0 = getelementptr double, ptr %a, i64 0
  %b0  = load double, ptr %pa0, align 8
  %mx  = icmp ne i32 %isMax, 0
  br label %lp

lp:
  %k    = phi i64 [ 1, %init ], [ %k1, %body ]
  %best = phi double [ %b0, %init ], [ %nb, %body ]
  %cnd  = icmp slt i64 %k, %n
  br i1 %cnd, label %body, label %done

body:
  %pk = getelementptr double, ptr %a, i64 %k
  %v  = load double, ptr %pk, align 8
  %gt = fcmp ogt double %v, %best
  %c1 = select i1 %gt, double %v, double %best
  %lt = fcmp olt double %v, %best
  %c2 = select i1 %lt, double %v, double %best
  %nb = select i1 %mx, double %c1, double %c2
  %k1 = add i64 %k, 1
  br label %lp

done:
  store double %best, ptr %out, align 8
  br label %exit

exit:
  ret void
}

; ---------------------------------------------------------------- sg_red_i
; int max/min 归约：单线程 k 升序 icmp sgt/slt，与 sg_tensor_reduce_i 一致。
define ptx_kernel void @sg_red_i(ptr %out, ptr %a, i64 %n, i32 %isMax) {
entry:
  %tid   = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  %ctaid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
  %t0    = icmp eq i32 %tid, 0
  %c0    = icmp eq i32 %ctaid, 0
  %go    = and i1 %t0, %c0
  br i1 %go, label %init, label %exit

init:
  %pa0 = getelementptr i64, ptr %a, i64 0
  %b0  = load i64, ptr %pa0, align 8
  %mx  = icmp ne i32 %isMax, 0
  br label %lp

lp:
  %k    = phi i64 [ 1, %init ], [ %k1, %body ]
  %best = phi i64 [ %b0, %init ], [ %nb, %body ]
  %cnd  = icmp slt i64 %k, %n
  br i1 %cnd, label %body, label %done

body:
  %pk = getelementptr i64, ptr %a, i64 %k
  %v  = load i64, ptr %pk, align 8
  %gt = icmp sgt i64 %v, %best
  %c1 = select i1 %gt, i64 %v, i64 %best
  %lt = icmp slt i64 %v, %best
  %c2 = select i1 %lt, i64 %v, i64 %best
  %nb = select i1 %mx, i64 %c1, i64 %c2
  %k1 = add i64 %k, 1
  br label %lp

done:
  store i64 %best, ptr %out, align 8
  br label %exit

exit:
  ret void
}

; ---------------------------------------------------------------- sg_un_f
; f64 一元（fnid 0=abs 1=sqrt）：逐元素恰一次运算。fabs（abs.f64 逐位清符）
; 与 sqrt.rn.f64（正确舍入，CPU libm sqrt 同为 IEEE 正确舍入）均逐位一致。
define ptx_kernel void @sg_un_f(ptr %out, ptr %a, i64 %n, i32 %fnid) {
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
  %v  = load double, ptr %pa, align 8
  %ab = call double @llvm.fabs.f64(double %v)
  %sq = call double @llvm.sqrt.f64(double %v)
  %sel = icmp eq i32 %fnid, 0
  %r  = select i1 %sel, double %ab, double %sq
  %po = getelementptr double, ptr %out, i64 %idx
  store double %r, ptr %po, align 8
  br label %exit

exit:
  ret void
}

; ---------------------------------------------------------------- sg_un_ai
; int abs：x < 0 ? -x : x（与 CPU sg_tensor_un 同式；INT64_MIN 回绕行为与
; CPU 负指令一致，无溢出判定）。
define ptx_kernel void @sg_un_ai(ptr %out, ptr %a, i64 %n) {
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
  %pa  = getelementptr i64, ptr %a, i64 %idx
  %v   = load i64, ptr %pa, align 8
  %neg = sub i64 0, %v
  %lt  = icmp slt i64 %v, 0
  %r   = select i1 %lt, i64 %neg, i64 %v
  %po  = getelementptr i64, ptr %out, i64 %idx
  store i64 %r, ptr %po, align 8
  br label %exit

exit:
  ret void
}

; ---------------------------------------------------------------- sg_un_si
; int 入 → f64 出：sitofp（cvt.rn.f64.s64 正确舍入，与 CPU (double)x 的
; cvtsi2sd 一致）后 sqrt；对应 CPU 覆盖形态 fnid=1 + int 张量。
define ptx_kernel void @sg_un_si(ptr %out, ptr %a, i64 %n) {
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
  %pa = getelementptr i64, ptr %a, i64 %idx
  %v  = load i64, ptr %pa, align 8
  %vf = sitofp i64 %v to double
  %r  = call double @llvm.sqrt.f64(double %vf)
  %po = getelementptr double, ptr %out, i64 %idx
  store double %r, ptr %po, align 8
  br label %exit

exit:
  ret void
}

; ---------------------------------------------------------------- sg_cpy
; 连续 8 字节元素拷贝：out[i] = a[off + i]（slice 设备内搬移；i64 视图读写
; 对 f64 位型透明，dtype 无关）。
define ptx_kernel void @sg_cpy(ptr %out, ptr %a, i64 %off, i64 %n) {
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
  %ai = add i64 %idx, %off
  %pa = getelementptr i64, ptr %a, i64 %ai
  %v  = load i64, ptr %pa, align 8
  %po = getelementptr i64, ptr %out, i64 %idx
  store i64 %v, ptr %po, align 8
  br label %exit

exit:
  ret void
}

declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
declare i32 @llvm.nvvm.read.ptx.sreg.ntid.x()
declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)
declare { i64, i1 } @llvm.ssub.with.overflow.i64(i64, i64)
declare { i64, i1 } @llvm.smul.with.overflow.i64(i64, i64)
