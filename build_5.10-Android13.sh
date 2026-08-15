#!/bin/bash

# ==============================================================
#  Android 内核驱动 (lsdriver.ko) 编译脚本
#  定制环境: Pixel 6 (oriole) / 5.10-Android13 
#  依赖: LineageOS 完整系统编译 out 目录 (最快、最稳定的方式)
# ==============================================================

set -euo pipefail

BUILD_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# =================== 环境变量配置 ===================
# LineageOS 源码树顶层目录
AOSP_ROOT="/home/void/lineageos/pixel6_20"

# 您自己设置的完整系统产物输出目录下的 KERNEL_OBJ (请确认此目录下有 .config 和 Module.symvers)
KERNEL_OUT="/mnt/sdwd/android_build/out/pixel6_20/target/product/oriole/obj/KERNEL_OBJ"

# 驱动源码路径 
DRIVER_SRC="/home/void/reverse/Linux-android-arm64/lsdriver"

# Clang 工具链路径
CLANG_PATH="$AOSP_ROOT/prebuilts/clang/host/linux-x86/clang-r450784d"
# ====================================================

GREEN='\e[32m'
RED='\e[31m'
YELLOW='\e[33m'
BLUE='\e[34m'
NC='\e[0m'

log_info()  { echo -e "${GREEN}$*${NC}"; }
log_warn()  { echo -e "${YELLOW}$*${NC}"; }
log_error() { echo -e "${RED}$*${NC}"; }
log_title() { echo -e "${BLUE}====================================================${NC}"; }

clean_driver_build() {
    if [[ ! -d "$DRIVER_SRC" ]]; then
        log_error "错误: 找不到驱动目录 $DRIVER_SRC"
        return 1
    fi
    find "$DRIVER_SRC" -mindepth 1 \( \
        -type f ! \( \
            -name 'Makefile' -o \
            -name 'Kconfig' -o \
            -name '*.c' -o \
            -name '*.h' -o \
            -name '*.lds' -o \
            -name '*.S' -o \
            -name '*.s' -o \
            -name '*.ko' \
        \) -o \
        -type d -empty \
    \) -delete
}

fix_empty_ext_modversions() {
    local mod_c="$DRIVER_SRC/lsdriver.mod.c"
    if [[ ! -f "$mod_c" ]]; then return 1; fi
    if grep -q '__section("__version_ext_names")' "$mod_c" && \
       grep -q '^[[:space:]]*;[[:space:]]*$' "$mod_c"; then
        perl -0pi -e 's/(__used __section\("__version_ext_names"\) =\n);/$1"";/' "$mod_c"
        return 0
    fi
    return 1
}

build_kernel() {
    log_title
    log_warn "正在开始编译驱动模块: 5.10-Android13 (Pixel 6)"

    if [[ ! -d "$KERNEL_OUT" ]]; then
        log_error "错误: 找不到内核产物目录 $KERNEL_OUT"
        log_warn "请确认目标目录下存在 KERNEL_OBJ 文件夹。"
        return 1
    fi

    log_warn "正在清理旧的构建产物..."
    clean_driver_build

    # --- 编译外部模块 ---
    log_info "执行 Make 编译驱动模块 (利用现成系统编译产物)..."

    local symvers_file="$KERNEL_OUT/Module.symvers"
    local symvers_backup=""
    local modpost_warn_param="KBUILD_MODPOST_WARN=1 CONFIG_EXTENDED_MODVERSIONS=n"

    # 隐藏 Module.symvers 避免 CRC 问题
    if [[ -f "$symvers_file" ]]; then
        symvers_backup="$symvers_file.no_crc_bak.$$"
        log_warn "临时隐藏 $symvers_file，避免 modpost 导入 CRC"
        mv "$symvers_file" "$symvers_backup"
    else
        log_warn "警告: 在 $KERNEL_OUT 未找到 Module.symvers，跳过隐藏步骤"
    fi

    # 直接使用 KERNEL_OUT 作为编译上下文
    set +e
    env PATH="$CLANG_PATH/bin:$PATH" \
        make -C "$KERNEL_OUT" \
            M="$DRIVER_SRC" \
            ARCH=arm64 \
            LLVM=1 \
            LLVM_IAS=1 \
            CROSS_COMPILE="aarch64-linux-gnu-" \
            LLVM_TOOLCHAIN_PATH="$CLANG_PATH" \
            $modpost_warn_param \
            modules -j"$(nproc)"
    local make_status=$?

    # 处理 Android 13 空 __version_ext_names 的 bug
    if [[ $make_status -ne 0 ]] && fix_empty_ext_modversions; then
        log_warn "检测到空 __version_ext_names，已修补 lsdriver.mod.c 并重试最终链接"
        env PATH="$CLANG_PATH/bin:$PATH" \
            make -C "$KERNEL_OUT" \
                M="$DRIVER_SRC" \
                ARCH=arm64 \
                LLVM=1 \
                LLVM_IAS=1 \
                CROSS_COMPILE="aarch64-linux-gnu-" \
                LLVM_TOOLCHAIN_PATH="$CLANG_PATH" \
                $modpost_warn_param \
                modules -j"$(nproc)"
        make_status=$?
    fi
    set -e

    if [[ -n "$symvers_backup" ]]; then
        mv "$symvers_backup" "$symvers_file"
        log_info "已恢复 $symvers_file"
    fi

    if [[ $make_status -ne 0 ]]; then
        log_error "❌ 编译失败"
        return "$make_status"
    fi

    # --- 处理产物 ---
    local source_ko="$DRIVER_SRC/lsdriver.ko"
    local target_ko="$DRIVER_SRC/5.10-Android13.ko"

    if [[ ! -f "$source_ko" ]]; then
        log_error "❌ 编译失败! (未生成 ko 文件)"
        return 1
    fi

    if [[ "$STRIP_CHOICE" == "y" || "$STRIP_CHOICE" == "Y" ]]; then
        local strip_cmd="$CLANG_PATH/bin/llvm-strip"
        if [[ -x "$strip_cmd" ]]; then
            log_info "正在剥离符号..."
            "$strip_cmd" --strip-debug -o "$target_ko" "$source_ko"
        else
            log_warn "未找到 llvm-strip，跳过剥离"
            cp "$source_ko" "$target_ko"
        fi
    else
        log_info "保留符号，创建副本..."
        cp "$source_ko" "$target_ko"
    fi

    log_info "✅ 生成完成: $target_ko"
}

main() {
    log_warn "是否需要剥离(strip)符号？"
    echo -e "  输入 ${GREEN}'y'${NC} 进行剥离 (减小体积)"
    echo -e "  输入 ${GREEN}'n'${NC} 不剥离 (保留调试符号)"
    read -rp "请输入 (y/n): " STRIP_CHOICE

    if [[ "$STRIP_CHOICE" != "y" && "$STRIP_CHOICE" != "Y" && \
          "$STRIP_CHOICE" != "n" && "$STRIP_CHOICE" != "N" ]]; then
        log_warn "无效输入，默认不剥离"
        STRIP_CHOICE="n"
    fi
    readonly STRIP_CHOICE

    build_kernel

    echo -e "${BLUE}产物列表:${NC}"
    ls -lh "$DRIVER_SRC"/5.10-Android13*.ko 2>/dev/null || log_error "未找到任何 .ko 文件"
    log_title
}

main "$@"
