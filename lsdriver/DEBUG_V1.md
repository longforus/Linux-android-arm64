# lsdriver 调试扩展 v1

本目录实现能力查询、断点命中事件、命中现场 FP 链和主线程软件单步 Trace。用户态的逐页读取完整性由匹配的 Driver/HTTP 层提供。完整 RPC 与 MCP 用法见主仓库根目录的 DEBUG_RPC_V1.md，XSettings fork 也提供同名文档。

## 协议与能力

- `request_op_monitor_log_read=26`，追加 `request_op_debug_v1=27`。
- 扩展消息使用既有 `vmemrw_info.user_buffer`（4096 字节），不扩大 `request_obj` 或改变旧编号。
- wire 定义：`io_version/debug_wire.h`；采集：`debug_capture.h`；分发与退出清理：`lsdriver.c`；单步接入：`arm64_stepdbg.h`。
- `LS_DEBUG_INFO` 返回 ABI、版本、页大小和功能位。版本唯一来源是 `io_version/version.h` 的 `LSDRIVER_VERSION`，不要在客户端写死。
- FP 支持取决于 `copy_from_user_nofault`；单步能力位仅检查所需符号前提，不保证 hook 安装或目标代码执行必然成功。
- 旧内核未知扩展请求保留用户态预置 EOPNOTSUPP，客户端应降级而不是将空数据当作支持。

| wire 命令 | 用途 |
|---|---|
| INFO | 查询能力 |
| EVENTS_START / EVENTS_STOP | 启停命中采集 |
| READ / STATUS | 分页读取 / 查询状态 |
| TRACE_START / TRACE_STOP | 启停独立主线程 Trace 配置 |

## 事件和 Trace 语义

事件环为 256 条，每页最多 8 条，读取不消费。保留 seq、时间、进程创建时间、TGID/TID 与 PC/LR/SP/FP；stream 区分会话，过期返回 ESTALE，越过末尾的游标返回 ERANGE。使用进程创建时间校验防止 PID 复用误命中。

断点命中时在异常上下文做 nofault FP 读取，最多 8 层，不分配内存或触发换页；失败、非法 FP 与预算耗尽分别标记。返回原始 FP/返回地址，不提供完整 DWARF/Java 展开或 PAC 剥离。

Trace 仅作用于主线程，使用独立断点配置，与已有断点和事件流互斥。从安装起开始单步，PC 到达 start_pc 后才入环；因此等待触发也有开销。start_pc 必须为对齐的用户态 AArch64 地址，max_steps=1..1000000。达到步数上限停止继续单步，客户端仍须 TRACE_STOP 释放配置。

duration_ms=1..60000 的期限与文件保存由用户态服务管理。客户端退出时驱动清理单步配置；目标退出结束采集。停止、退出和 PID 生命周期校验应一并维护，不能只关闭文件而遗留线程单步。

## 用户态调用与同步

事件：选目标 → breakpoint.events.start → breakpoint.set → events.read / backtrace → breakpoint.clear → events.stop。安装断点和开始采集是独立操作。

Trace：选目标 → trace.configure → trace.start → trace.status → trace.stop → trace.list / trace.read。目录归当前工具 UID 所有且权限 0700，进程重启后重新 configure。

修改协议时必须同步主仓库 `android/jni/driver/` 与 XSettings `LS_KTool/jni/driver/` 的枚举和 wire。内核树镜像主仓库 `lsdriver/`；XSettings 是独立 fork，按文件同步。

构建使用匹配的 Pixel 6 内核配置；需部署时直接增量构建 bootimage。仅编译驱动对象不能代替完整内核链接或设备行为检查。软件单步不是 ETM，不保证无事件丢失；旧通信等待在驱动失联时仍可能阻塞。
