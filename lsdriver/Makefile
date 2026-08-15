# ==============================================================
# 适配 In-Tree 静态编译或模块编译
# 当 Kconfig 中选择 y 时，编译进内核映像；选 m 时，编译为 ko
# ==============================================================
obj-$(CONFIG_LSDRIVER) += lsdriver.o

lsdriver-y := lsdriver_main.o \
              arm64_encode/arm64_encode.o \
              arm64_decode/arm64_decode.o \
              arm64_decode/arm64_decode_base.o \
              arm64_decode/arm64_decode_ldst.o \
              arm64_decode/arm64_decode_branch.o \
              arm64_decode/arm64_decode_simd.o \
              arm64_decode/arm64_decode_sve.o \
              arm64_decode/arm64_decode_sme.o

$(obj)/lsdriver_main.o: $(src)/lsdriver.c FORCE
	$(call if_changed_dep,cc_o_c)

# 开启debug输出，有pr_debug输出
#ccflags-y += -DDEBUG

# O3 极速优化
ccflags-y += -O3

# 模拟器和 arm64_reg.h 在 lsdriver_main.o 中显式使用 FP/SIMD/FP16、LSE、RCpc 和 CRC 内联汇编。
# 统一启用对应指令集后，汇编字符串无需再重复声明局部架构扩展(.arch_extension)。
# 让 Clang 集成汇编器识别这些指令助记符。
ccflags-y += -march=armv8.2-a+fp16+lse+rcpc+crc

# ARM64 Kbuild 默认添加 -mgeneral-regs-only，它也会禁止显式 FP/SIMD 内联汇编；
# 仅从包含模拟器代码的 lsdriver_main.o 中移除此限制，不影响模块的其他目标文件。
CFLAGS_REMOVE_lsdriver_main.o += -mgeneral-regs-only

# 移除 -mgeneral-regs-only 后，仍禁止编译器自动生成或自动向量化为 FP/SIMD 指令；
# FP/SIMD 寄存器只允许由源码中可审计的显式内联汇编使用。
CFLAGS_lsdriver_main.o += -mno-implicit-float

# 将所有警告降级，不再视为错误
ccflags-y += -Wno-error

# 【新增】强行解除内核针对 C99 "变量声明在执行语句之后" 的报错拦截
ccflags-y += -Wno-declaration-after-statement

# 驱动源码使用 C99 使用点声明与 for 初始化子句变量；旧内核默认 GNU89，需显式启用现代 GNU C 方言。
ccflags-y += -std=gnu11

# 禁用栈溢出保护
ccflags-y += -fno-stack-protector

# 废弃帧指针,释放 FP (x29) 寄存器作为通用计算寄存器，减少压栈动作
ccflags-y += -fomit-frame-pointer

# 循环展开: 牺牲二进制体积，减少 while/for 循环的跳转和判断开销
ccflags-y += -funroll-loops

# 严格别名优化: 允许编译器假设不同类型的指针指向不同内存，从而进行更激进的指令重排
ccflags-y += -fstrict-aliasing

# 死代码消除辅助: 将每个函数和数据放入独立段，配合链接器剔除无用代码
ccflags-y += -ffunction-sections -fdata-sections

# ==========================================
# 下面编译配置如果使用会出现意外，按需注释
# ==========================================

# # 强制内联: 消除函数调用 (call/ret) 和参数压栈的开销
# ccflags-y += -finline-functions

# # 彻底关闭针对该模块的内存杀毒软件 (KASAN) 和未定义行为检查 (UBSAN)
# KASAN_SANITIZE := n
# UBSAN_SANITIZE := n
# KCSAN_SANITIZE := n

# # 关闭针对该模块的代码覆盖率统计和性能追踪
# GCOV_PROFILE := n
# KCOV_INSTRUMENT := n

# # 剔除 Ftrace (函数调用追踪) 插桩
# CFLAGS_REMOVE_lsdriver.o += $(CC_FLAGS_FTRACE)

# # 强制 64 字节对齐 (适配 ARM64 的 Cache Line)
# ccflags-y += -falign-functions=64
# ccflags-y += -falign-loops=64

# # 暴力循环展开与内联 (Clang 参数)
# ccflags-y += -funroll-loops
# ccflags-y += -finline-functions
# # 强行拉高 LLVM 的内联和展开阈值 (让函数扁平化)
# ccflags-y += -mllvm -inline-threshold=10000
# ccflags-y += -mllvm -unroll-threshold=10000