# 2026-11-14 上游选择性吸收

后续跟进起点统一记录在 [UPSTREAM_SYNC.md](UPSTREAM_SYNC.md)，从检查点之后的新增上游提交开始审查。

审查基准：本地 `22b23fd`、上游 `lsnbm/main@a7f7da248a9b0a4b4615fd5219d47ee89e3d91f6`，共同基点 `e6e1da5`。

本次吸收并适配：

- `3f9a050a63`：用户态驱动请求用 `std::mutex` 替换持续原子交换的自旋锁，保留本地协议、握手和读取完整性功能。
- `95194a51b0`：扫描值解析独立到 `scan_value.h`，浮点匹配模式和自定义对齐；扫描器注入内存读取器，支持无设备主机回归。优先扫描可读 maps，失败回退驱动区域。
- `d954193b75`：借鉴应用服务分层，将本地业务逻辑移到 `ApplicationService`，共享运行时请求锁。保留 57 个 MCP 工具、断点事件、回溯、trace、读取完整性及 Python/Windows 客户端。
- MCP HTTP 校验媒体类型（忽略大小写、精确匹配），JSON 输出替换非法 UTF-8，继续支持三个现有协议版本。
- `e2d86e659a`：模块隐藏延后；重新实现失败回滚。内存分配、两个休眠 kthread 的创建均在安装 hook 之前，失败时先 `kthread_stop` 等待线程退出，再释放物理映射；线程只在成功路径最后唤醒。hook 安装失败保持原有非致命行为。
- `161eb3a109`：Vulkan 使用长边正方形 buffer，保留原图层名称和共享库 inline 状态。
- `2234f0f73f` / `f8c123803f` 中的独立编解码扩展：IC IVAU 解码和 UDF 编码。修正上游 UDF 立即数误放 `[20:5]` 的问题，实际编码使用 `[15:0]`，并用 LLVM 汇编器校对。

未吸收：vivo vermagic、旧 CFI 厂商兼容（用户明确排除）、PAC/BTI/LTO 构建配置变更、CPU/DRM 渲染、进程隐藏扩展、硬编码外网 ping、未落地的模拟访存保护和缓存同步重写。未带入上游二进制、日志或临时文件。

验证命令：

```bash
python3 tools/generate_mcp_catalog.py windows/LuckyStarMcp.py --check
g++ -std=c++20 -I android/jni/json/single_include tools/mcp_protocol_test.cpp -o /tmp/xst-mcp-test
/tmp/xst-mcp-test
g++ -std=c++20 -pthread tools/scan_contract_test.cpp -o /tmp/scan-contract-test
/tmp/scan-contract-test
python3 tools/kernel_init_test.py
make -C lsdriver/arm64_tests/decoder contract-test
make -C lsdriver/arm64_tests/encoder contract-test
```

XSettings 同步时保留 JNI、AMS/su PID 兜底、诊断日志和共享库配置；PID 兜底从 `http_server.h` 随业务操作迁移到 `application/service.h`。

已完成验证：上述主机测试通过；主仓库 NDK 29 ARM64 `main.cpp` 编译通过；XSettings `:app:assembleRelease` 成功（含 JNI 共享库链接）；Pixel 6 现有配置的 `drivers/lsdriver/` 内置目录编译成功，生成 `built-in.a`。内核编译在现有 `aosp-build` 容器中执行，驱动目录目标跳过无关的 `resolve_btfids` 工具重建，未修改内核配置。后续已完成完整 OTA 编译、刷机、26 项设备 API 回归及旋转/重启验证，见 [全量刷机报告](reports/upstream-20261114/REPORT.md)。
