# 上游跟进检查点

## 当前进度（2026-11-28）

| 项目 | 当前记录 |
| --- | --- |
| 上游仓库 / 分支 | `lsnbm/Linux-android-arm64` / `main` |
| 本轮对照的上游快照 | **`dc99e8dc7a228520f0dbd5cd288355dbbc17f433`** |
| 上游快照标题 | `fix(kernel): 修复进程目录 hook 安装期间的空指针调用` |
| 审查范围 | 定向审查与选择性移植；**不是该快照之前全部差异均已审完或吸收** |
| 本轮本地起点 | `myPixel6@b90379f` |
| 实现与同步提交 | 见下方提交记录；上游快照不是本地合并提交 |
| 部署状态 | 已编译、标准 OTA 刷机并完成设备回归，系统 `20.0-20261128-RELEASE-oriole` / B 槽 |
| 审查与取舍 | [2026-11-28 审查报告](reports/upstream-20261128-review.md) |
| 实施与验证证据 | [2026-11-28 验证报告](reports/upstream-20261128/README.md) |

**上游已经重写部分历史。** 旧检查点 `a7f7da248a9b0a4b4615fd5219d47ee89e3d91f6` 与当前上游分叉；当前历史中同标题、同日期的 CPU 时钟提交为 `330e94db`。用户要求以当前上游状态为准，**不要恢复已被上游丢弃的提交，也不要继续用旧检查点的双点范围列举“新增”**。本地 `main` 不是本轮审查依据。

## 本轮已完成，不要重复移植

- HTTP（参考 `a46b79ba` / `87f71394` 的当前结果）：独立请求解析器、整请求 15 秒截止时间、严格版本/头部校验、拒绝 Transfer-Encoding、正确区分头部与 body 大小、连接 fd 清理、发送超时和排队限制。保留本地 RPC/MCP、62 工具、health 能力、共享请求锁及 JNI 日志。
- Hook（参考 `73bc33dd` / `2238012e` 的当前结果）：BTI JC + 五条指令入口及回放、B/BL/literal 重定位、literal 与入口覆盖区相交检查和范围溢出拒绝。**保留本地条件分支近端 branch island**；未改 Pixel 6 架构特性守卫或 PAC/LTO 构建配置。
- LS 误重连（参考 `2c3eee13`）：RCU 下检查 real_parent，跳过已连接客户端的直接 fork 子进程；既有进程扫描的整体 task 引用生命周期未重构。
- 已同步：内核树整体镜像（去除 byte_stream）；xsettings 按文件同步 HTTP 两个文件，保留独立 fork 适配；新版 APK 已放入 ROM 源码并核对打包与设备哈希。协议结构/枚举未变更。
- 已验证：主机 HTTP、encoder、生产跳板布局/重定位、初始化故障注入、扫描、MCP/catalog；APK、boot 和完整 OTA 构建；刷机后 26 项 API、HTTP 非法请求/并发/超时、20 项 wxshadow、服务重启、普通重启全部通过。无 panic/Oops/坏页日志。

## 仍待专项评估 / 实施

以下是截至当前上游快照的明确待办，**下次不能只看快照之后的新增提交而漏掉这些项**：

| 待办 | 参考提交 / 当前判断 |
| --- | --- |
| Surface 私有 ABI 与释放兼容 | `ccf0907a`、`77653f1c`、`2238012e`；需与 renderer 生命周期一起适配并验证旋转、录屏镜像 |
| 资源与异步任务所有权 | `a46b79ba`、`ef9ae3b3`、`87f71394`；保留本地 DebugService、JNI 单例、共享线程池与 fork 适配，不能整体覆盖 runtime/service |
| 触摸线程与锁优化 | `d546e3ca`；依赖上游 TouchInput 类，需与触摸生命周期改造一起做 |
| DPTDBG 生命周期、映射更新与 mprotect 同步 | 当前上游仍长期持有 mm_users，且改用不可中断 mmap 写锁；先解决退出/Binder 风险，再验证并发卸载及与 wxshadow 共存，不能直接移植 |
| LS 进程扫描引用生命周期 | 本轮只修直接子进程误重连；原有遍历/task 引用安全仍待独立审查 |

## 当前无须重复吸收 / 默认排除

- Vulkan SUBOPTIMAL（`d2429c20`）、UDF 编码（`31543d60`）、禁用初始化 ping（`151b773e`）：本地已有等价行为。
- 最新 proc_pid_readdir 空回放指针修复（`dc99e8dc`）：本地仍用 filldir64 hook，不受该特定缺陷影响；以后更换隐藏实现时必须保留最终修复。
- 用户明确排除：**vivo vermagic、旧 CFI 厂商兼容**，除非用户改变要求。
- 继续暂不吸收：CPU/DRM 渲染、进程隐藏扩展、PAC/BTI/LTO **构建配置**重写、缓存同步重写及未落地的模拟访存保护。注意 BTI **hook 入口修复已吸收**，不要与构建配置排除项混淆。
- PAGE_PINNER、未导出符号等特定外部模块兼容：Pixel 6 内置构建无必要为此覆盖配置或改变锁等待语义。
- 不带入上游预编译 ko、installer 二进制载荷、panic 原始 dumps、临时日志和 pycache；不重新引入外网 ping。

## 下次从哪里开始

1. 先读本文件的“仍待专项评估 / 实施”，确认当前工作树和三处提交状态。
2. 拉取当前上游，先判断本轮快照是否仍是祖先：

```bash
git fetch https://github.com/lsnbm/Linux-android-arm64.git main
git merge-base --is-ancestor dc99e8dc7a228520f0dbd5cd288355dbbc17f433 FETCH_HEAD
```

仅在祖先检查返回 0 时，以下命令才代表本轮快照之后的新增内容：

```bash
git log --reverse --oneline dc99e8dc7a228520f0dbd5cd288355dbbc17f433..FETCH_HEAD
git diff --stat dc99e8dc7a228520f0dbd5cd288355dbbc17f433 FETCH_HEAD
```

若祖先检查失败，按用户要求比较**当前代码状态**，结合本文件的已完成/待办清单重新确定范围；不能将分叉另一侧全部提交当作新增，也不能将未完成项标记为已合并。移植必须保留本地协议、wxshadow、Pixel 6 与 xsettings 适配。

构建严格遵守 `/home/void/lineageos/pixel6_20/AGENT.md`：使用 `distrobox enter aosp-build`，加载 envsetup、`croot`、`lunch lineage_oriole-user`，确认通用输出目录 `/mnt/sdwd/android_build/out/pixel6_20`。不要直接用 podman exec 替代，也不要在源码树生成 out。

## 提交记录

| 项目 | 本轮提交 |
| --- | --- |
| xsettings HTTP 同步 | `480cdc88dc9d014c7314d58f7f1d0dbf34bab499` |
| Pixel 6 内核树功能同步 | `a7f6c2e4f8a42a61b857c42edb73fd66cd34730c` |
| Pixel 6 内核树源码版本更新 | `68d0b6ac4f53d3b9fa89a6ef4d59d8616d6bee85` |
| 主仓库实现、测试及本检查点 | 本文件与实现同次提交，使用下方命令定位 |

**源码版本号已按用户要求对齐为 `3.5.11` 并同步内核树，未重新编译/刷机。当前手机与本轮测试记录仍读出 `2.4.0`，下次修改时统一构建再生效。** 版本号不能作为“已全量合并上游”的判断依据。

主仓库无需在本文件中自引用提交哈希：

```bash
git log -1 --format='%H %s' -- UPSTREAM_SYNC.md
```

## 历史记录：2026-11-14

| 项目 | 历史记录 |
| --- | --- |
| 当时已审查上游提交 | `a7f7da248a9b0a4b4615fd5219d47ee89e3d91f6`（当前已分叉，不再用于新增范围） |
| 本仓库实现 | `eaf8e3e0eb9766a0a3231c4eb905dcf55fafb8ab` |
| xsettings 同步 | `d2625410c5d01142007410775f75457228bed618` |
| Pixel 6 内核树同步 | `f7782c1cc5ad9bfd6484b810b0cd9af7937d2f73` |
| 说明与验证 | [移植说明](UPSTREAM_INTEGRATION.md)、[验证报告](reports/upstream-20261114/REPORT.md) |

每轮更新必须同时记录当前上游快照、实际移植范围、待办、三处同步状态与验证结果；选择性移植不代表整体合并上游。
