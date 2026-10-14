define void @vec_add(ptr %a, ptr %b, ptr %c, i64 %n) {
entry:
  %tid   = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
  %t64   = zext i32 %tid to i64
  %cond  = icmp slt i64 %t64, %n
  br i1 %cond, label %do, label %exit
do:
  %pa = getelementptr double, ptr %a, i64 %t64
  %va = load double, ptr %pa, align 8
  store volatile double %va, ptr %a, align 8
  br label %exit
exit:
  ret void
}
declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
