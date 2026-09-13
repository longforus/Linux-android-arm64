# Agent Instructions for Linux-android-arm64

This project builds `lsdriver.ko`, a kernel module for memory debugging and input assistance on Android ARM64 devices.

## Initial Setup

The repository uses Git submodules for several dependencies. After cloning, you must initialize them:

```bash
git submodule update --init --remote --recursive
```

## Build Process

The primary build method is the `build_all.sh` script, which compiles the driver for multiple Android kernel versions.

### Prerequisites

The build script has two critical, non-obvious requirements:

1.  **Kernel Sources Location:** The script expects pre-downloaded Android kernel source trees to be organized in a specific directory structure. Each version's source code must be in a subdirectory under a common root, for example:
    - `/root/6.1-Android14`
    - `/root/5.15-Android13`
    - `/root/5.10-Android12`
    The default root is `/root`, configurable via the `KERNELS_ROOT` variable in `build_all.sh`.

2.  **Project Source Path (CRITICAL):** The script contains a hardcoded path for the driver source code:
    - `DRIVER_SRC="/mnt/e/1.CodeRepository/Android/Kernel/lsdriver"`
    For the build to succeed, the project **must** be located at this exact path. If your project is elsewhere, create a symbolic link:
    ```bash
    # Example: if the project is in /home/void/reverse/Linux-android-arm64
    sudo mkdir -p /mnt/e/1.CodeRepository/Android/Kernel
    sudo ln -s /home/void/reverse/Linux-android-arm64 /mnt/e/1.CodeRepository/Android/Kernel/lsdriver
    ```

### Running the Build

-   To build all supported kernel module versions:
    ```bash
    ./build_all.sh
    ```
-   To build for a single, specific version:
    ```bash
    ./build_all.sh <version_name>
    # Example: ./build_all.sh 6.1-Android14
    ```

### Build Output

The script generates two main types of artifacts:

1.  **Versioned Kernel Modules:** Individual `.ko` files for each kernel version, placed in the `lsdriver/` directory (e.g., `lsdriver/6.1-Android14.ko`).
2.  **Installer Script:** `install_driver.sh` is created in the project root. This script bundles all the versioned `.ko` files and is designed to be run on the target Android device to automatically load the correct driver.

## Manual Development Build

For development or testing against a single kernel version without using the batch script, you can use the standard kbuild command. This is useful if you have the kernel source available at an arbitrary path (`<KDIR>`).

From the project root, run:
```bash
make -C <KDIR> M=$PWD/lsdriver ARCH=arm64 LLVM=1 modules
```
Replace `<KDIR>` with the absolute path to your target kernel's source tree.

## 三处项目联动与同步

本仓库是 lsdriver 驱动源码 + `android/jni` 用户态源码 + `windows/` PC 工具的**主开发仓库**。改动后需同步到另外两个项目才能生效：

| 位置 | 作用 | 与本仓库对应关系 |
|------|------|------------------|
| `/home/void/reverse/Linux-android-arm64`（本仓库） | lsdriver 驱动 + 用户态源码 | 单一事实来源 |
| `/home/void/lineageos/pixel6_20/kernel/google/gs201/private/gs-google/drivers/lsdriver/` | LineageOS 内核树内的 lsdriver | **整体镜像** `lsdriver/` |
| `/home/void/AsfpProjects/xsettings/LS_KTool/jni/` | xsettings app 的原生工具 | **按文件**同步 `android/jni/` 变更 |

### 同步到内核树（整体镜像）

```bash
rsync -a --delete /home/void/reverse/Linux-android-arm64/lsdriver/ \
    /home/void/lineageos/pixel6_20/kernel/google/gs201/private/gs-google/drivers/lsdriver/
```

- `--delete` 会删除源中已删除的文件（如 `arm64_decode_base.c`、`arm64_decode_internal.h`、`emulate_insn.h`），**必须保留**。
- 内核树集成点：`drivers/Makefile:195` `obj-y += lsdriver/` + `drivers/Kconfig:244` `source "drivers/lsdriver/Kconfig"` + `CONFIG_LSDRIVER=y`（内置进内核镜像，非 .ko）。
- 同步后需重新编译内核（`mka bootimage` / `m bacon`）。

### 同步到 xsettings/LS_KTool（按文件，勿整体镜像）

LS_KTool 是**独立 fork**，带自己的 app 适配（`jni_bridge.cpp`、`BUILD_SHARED_LIBRARY`、`-std=c++2b` 等），**绝不能 `rsync --delete`**。可能只需要复制以下 7 个变更文件：

```bash
SRC=/home/void/reverse/Linux-android-arm64/android/jni
DST=/home/void/AsfpProjects/xsettings/LS_KTool/jni
for rel in Android.mk android_surface/surface_control_manager.h driver/driver.h \
           include/http_server.h include/memory_tool.h include/read_write_test.h src/main.cpp; do
  cp "$SRC/$rel" "$DST/$rel"
done
```

复制后到 `/home/void/AsfpProjects/xsettings` 用 `git diff LS_KTool/jni/` 审查，**还原被覆盖的 fork 专属修改**（完整清单见 `xsettings/AGENTS.md`），并核对协议枚举。

### 关键约束

1. **用户态-内核协议必须一致**：`android/jni/driver/driver.h` 内嵌的 `request_op` 枚举、`request_obj` 及其嵌套结构体（`bp_point`/`break_point`/`virtual_*` 等）必须与 `lsdriver/io_struct.h` 完全一致。内核新增枚举项（如 `request_op_dptdbg_set/remove`）时，用户态 driver.h 必须同步补上，否则枚举值错位、`syscall_monitor` 及之后所有请求失效。
2. **Makefile 双模式**（合并后的形态，勿改回单一行）：
   ```make
   ifeq ($(KBUILD_EXTMOD),)
   obj-$(CONFIG_LSDRIVER) += lsdriver.o   # in-tree：跟随 Kconfig（y=内置/m=模块）
   else
   obj-m += lsdriver.o                     # 外部 M=... modules 构建：必须 obj-m 才产出 .ko
   endif
   ```
   kbuild 对外部模块只对 `obj-m` 产出 `.ko`；若用 `obj-$(CONFIG_LSDRIVER)` 而内核 `.config` 是 `CONFIG_LSDRIVER=y`，外部构建会退化 `obj-y` 导致不生成任何 ko。
3. **ARMv8.6 模板运行时守卫**：`arm64_emulate_hw_templates.h` 的 usdot/bfdot/smmla/ummla 等模板在 Pixel 6（Tensor，ARMv8.2）上不会执行——handler 均带 `arm64_current_cpu_has_bf16()` / `has_i8mm()` 守卫，不支持时返回 `EMU_INST_SKIP`（仅打日志，不崩溃）。**勿改守卫**。

### 构建与清理

- 外部模块构建（Pixel 6 / 5.10-Android13）：`./build_5.10-Android13.sh`（需 `KERNEL_OUT` 与 clang 路径，脚本会临时隐藏 `Module.symvers` 避 CRC、并处理 Android 13 空 `__version_ext_names` bug）。
- 清理编译产物：`git clean -fdx lsdriver/`（会保留仓库跟踪的 `arm64_tests/executor/*.ko`）。
- xsettings 侧构建：`cd /home/void/AsfpProjects/xsettings && ./gradlew :app:assembleRelease`。
