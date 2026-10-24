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
rm -rf /home/void/lineageos/pixel6_20/kernel/google/gs201/private/gs-google/drivers/lsdriver/byte_stream
```

- `--delete` 会删除源中已删除的文件（如旧 `io_struct.h`、`lsdriver_log.h`），**必须保留**；同步后删掉 `byte_stream/`（预编译 `.ko` 无需放入内核树）。
- 内核树集成点：`drivers/Makefile:195` `obj-y += lsdriver/` + `drivers/Kconfig:244` `source "drivers/lsdriver/Kconfig"` + `CONFIG_LSDRIVER=y`（内置进内核镜像，非 .ko）。
- 同步后需重新编译内核（`mka bootimage` / `m bacon`）。

### 同步到 xsettings/LS_KTool（按文件，勿整体镜像）

LS_KTool 是**独立 fork**，带自己的 app 适配（`jni_bridge.cpp`、`BUILD_SHARED_LIBRARY`、`-std=c++2b` 等），**绝不能 `rsync --delete`**。注意上游已将原 `include/memory_tool.h` 等拆分为：
- `include/application/runtime.h`（`ApplicationRuntime` 托管 `Config::ThreadPools` 与各子模块资源）
- `include/architecture/disassembler.h`
- `include/base/{config,memory_utils,system_log,types}.h`
- `include/memory/{mem_scanner,memory_viewer,pointer_manager,saved_address_manager,signature_scanner}.h`
- `include/service/http_server.h`
- `include/test/{read_write_test,touch_test,gyro_test,gnss_test}.h`
- `driver/{driver,driver_loader}.h`、`logger/logger.h`

同步时需按模块复制变更文件，并在 `/home/void/AsfpProjects/xsettings` 用 `git diff LS_KTool/jni/` 审查，**还原被覆盖的 fork 专属修改**（完整清单见 `xsettings/AGENTS.md`），并核对协议枚举。

### 关键约束与历次修复记录

1. **用户态-内核协议必须一致**：`android/jni/driver/driver.h` 内嵌的 `request_op` 枚举、`request_obj` 及其嵌套结构体（`bp_point`/`break_point`/`virtual_*`/`env_params` 等）必须与 `lsdriver/io_version/io_struct.h` 完全一致。内核新增枚举项（如 `request_op_dptdbg_set/remove`、`request_op_env_get_params`、`request_op_monitor_log_read`）时，用户态 `driver.h` 必须同步补上，否则枚举值错位。
2. **Makefile 双模式与 Full LTO 架构特性约束**（`lsdriver/Makefile`）：
   - 双模式构建（勿改回单一行）：
     ```make
     ifeq ($(KBUILD_EXTMOD),)
     obj-$(CONFIG_LSDRIVER) += lsdriver.o   # in-tree：跟随 Kconfig（y=内置/m=模块）
     else
     obj-m += lsdriver.o                     # 外部 M=... modules 构建：必须 obj-m 才产出 .ko
     endif
     ```
   - **禁止 `-march=armv8.5-a`**：Pixel 6 (Tensor GS101) 为 ARMv8.2，若开 `armv8.5-a` 在内核 `CONFIG_ARM64_PTR_AUTH_KERNEL=y` 下会让编译器生成非 HINT 的 `pacia`/`retaa` 指令导致 `SIGILL` 崩溃。必须保持 `-march=armv8.2-a+bf16+i8mm+fp16fml+fp16+lse+rcpc+crc+dotprod`，并配合 `-Xclang -target-feature -Xclang +complxnum -Xclang -target-feature -Xclang +rcpc-immo` 使 `arm64_emulate_hw_templates.h` 中 `always_inline` 的 `ARM64_HW_TEMPLATE`（含 `fcmla`、`stlurb`/`ldapurb`）在 Full LTO (`-flto`) 下跨编译单元内联到 `emulate_inst.c` 时编译通过且不触发 PAC 指令生成。
3. **ARMv8.3+/v8.6 模板运行时守卫**：`arm64_emulate_hw_templates.h` 的 usdot/bfdot/smmla/ummla/fcmla/ldapurb 等模板在 Pixel 6 上由 `emulate_inst.c` 的 CPU 特性守卫（`arm64_current_cpu_has_bf16()` / `has_i8mm()` / `has_fcma()` / `has_lrcpc2()`）保护，不支持时返回 `EMU_INST_SKIP`。**勿改守卫**。
4. **`virtual_memory_rw.h` 的 `cached_mm` 引用计数规范（防目标进程退出时 Binder 僵死）**：
   - `virtual_memory_rw()` 缓存 `cached_mm` 时，**严禁跨请求长期持有 `mm_users`**（即不能把 `get_mm_by_pid()` 返回的 `mm_users` 引用一直留到下次切换 PID 才 `mmput`）。
   - 若长期持有 `mm_users`，当被附加的目标 App 退出或被 `force-stop` 时，内核 `do_exit() -> exit_mm()` 中 `mm_users` 无法归零，`__mmput() -> exit_mmap()` 不会执行，导致目标进程的 `/dev/binder` VMA 无法关闭（`/dev/binderfs/binder_logs/proc/<pid>` 残留），`system_server` 收不到 `binderDied` 通知，再次启动该 App 时 `ActivityManager` 会报 `refused to die` 并卡死在启动界面。
   - 正确做法（已实现）：`cached_mm` 仅通过 `mmgrab()`/`mmdrop()` 持有结构体本身的 `mm_count` 引用；每次 `virtual_memory_rw()` 调用期间通过 `mmget_not_zero(cached_mm)` 临时获取 `mm_users`，并在函数返回前立即 `mmput(mm)`。
5. **移除硬编码外网连通性校验**：`lsdriver/lsdriver.c` 的 `lsdriver_init()` 中已移除上游的 `ipv4_ping("8.211.158.255", ...)` 校验，且 `hide_myself()` 用 `#ifdef MODULE` 保护（适配 `CONFIG_LSDRIVER=y` 内置编译与离线启动）。
6. **`lsdriver/arm64_ptedbg.h` 不可加入 `.gitignore`**：上游曾误将该头文件加入 `.gitignore` 并删除，但 `break_point.h` 仍 `#include "arm64_ptedbg.h"`，必须在仓库中保留跟踪。
7. **多编译单元共享库符号与生命周期（`android/jni/` & `xsettings`）**：
   - `android_imgui_input/imgui_touch_input.h` 与 `android_draw/vulkan_imgui_renderer.h` 中的全局状态变量和函数必须使用 `inline` 修饰，防止 `xsettings` 将 `main.cpp` 与 `jni_bridge.cpp` 同时编译进 `libLS_KTool.so` 时产生多重定义错误或跨 TU 状态分裂。
   - `driver/driver.h` 提供 `EnsureSubsystems()`、`include/application/runtime.h` 提供 `shutdownGraphics()`，供 `jni_bridge.cpp` 以进程级单例安全复用 `Driver` 与 `ApplicationRuntime`。

### 构建与清理

- 外部模块构建（Pixel 6 / 5.10-Android13）：`./build_5.10-Android13.sh`（需 `KERNEL_OUT` 与 clang 路径，脚本会临时隐藏 `Module.symvers` 避 CRC、并处理 Android 13 空 `__version_ext_names` bug）。
- 清理编译产物：`git clean -fdx lsdriver/`（会保留仓库跟踪的 `arm64_tests/executor/*.ko`）。
- xsettings 侧构建：`cd /home/void/AsfpProjects/xsettings && ./gradlew :app:assembleRelease`。
