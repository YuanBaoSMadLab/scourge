# 模拟 AOT 侧 LLVM vararg 发射的精确寄存器状态调用 msvcrt printf：
# rcx=fmt、rdx=42、r8=3.5 位模式、xmm2=3.5、xmm0/1/3=0、AL=函数地址低字节。
# sim(rcx = printf 地址, rdx = fmt)
	.text
	.globl	sim
sim:
	subq	$0x28, %rsp
	movq	%rcx, %rax              # rax = printf 地址（AL = 低字节垃圾，与 AOT 同）
	movq	%rdx, %rcx              # rcx = fmt
	movl	$42, %edx
	movsd	c35(%rip), %xmm2        # 位置 2 的 xmm 槽
	movq	%xmm2, %r8              # 位置 2 的 GP 影子槽（位模式）
	xorps	%xmm0, %xmm0            # 模拟 LLVM 未发 xmm0（垃圾/0）
	xorps	%xmm1, %xmm1
	xorps	%xmm3, %xmm3
	callq	*%rax
	addq	$0x28, %rsp
	ret
	.section	.rdata,"dr"
c35:
	.quad	0x400c000000000000      # double 3.5
