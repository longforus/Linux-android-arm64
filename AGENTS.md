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
