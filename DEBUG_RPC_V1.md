# lsdriver 调试扩展 v1

新增功能沿用 `POST /api/rpc` 的 `{operation,params}` 请求和 `{ok,data,error}` 响应。通过现有 LuckyStar MCP 暴露，不增加 KPM 或云端依赖。本批只增加原生/RPC/MCP 能力，没有增加 Compose 按钮。

## 读取与能力

`bridge.capabilities`（`bridge.ping` 也附带这些字段）返回 debug ABI、驱动版本、内核页大小、事件/栈采集支持和单步 Trace 的符号前提检查。`trace_runtime_verified=false` 明确表示没有真机验证；`step_trace_prerequisites=true` 不保证 hook 能安装成功。旧内核返回 `debug_abi=0`、`debug_status=-EOPNOTSUPP`，保留已有功能。

`memory.read` 增加可选 `allow_partial`，默认 false 保留旧行为：读不完整即 RPC 失败。设置 true 时返回逐段结果，完整性必须检查 `complete` 与 `segments[].valid`：

```json
{"operation":"memory.read","params":{"address":"0x1000","size":12288,"allow_partial":true}}
```

结果包含 `requested_bytes/read_bytes/complete/segments`。有效区段有自身 `data_hex`；失败区段只有地址、offset、size、status、valid=false。顶层 `data_hex` 仅在全部有效时提供，缺口不会伪装成零值。

底层以 4K 边界细分，适配常见 4K/16K/64K 内核页。一个细分请求返回短计数时，因旧协议无法定位其有效位置，整段按未知处理。旧 `Driver::Read` 改为只返回连续有效前缀，首个缺口后停止，防止扫描器把跨缺口成功量当作前缀。读取失败的输出缓冲被清零，但清零不表示目标内存为零。

## 命中事件与回溯

1. 选中目标，调用 `breakpoint.events.start`，保存返回的非零 `stream`。
2. 沿用 `breakpoint.set` 安装现有断点，再触发受控目标。
3. 用 `breakpoint.events.read` 分页读取，`count=1..8`，初始 `after="0"`，之后使用 `next_after`。
4. `breakpoint.backtrace` 以 `stream` 和 `event_seq` 精确查询一次命中的已保存 FP 链。
5. 调用 `breakpoint.clear` 移除断点、`breakpoint.events.stop` 停止采集；这两个操作相互独立。

```json
{"operation":"breakpoint.events.read","params":{"stream":"1","after":"0","count":8}}
```

```json
{"operation":"breakpoint.backtrace","params":{"stream":"1","event_seq":"4"}}
```

事件有 `seq/timestamp_ns/process_start_ns/pid/tid/pc/lr/sp/fp/frames`。64 位序号、时间和地址使用字符串。`breakpoint.events.status` 也接收 stream。过期 stream 返回 ESTALE；请求的序号超过当前末尾返回 ERANGE；已覆盖的具体命中不能回溯。

事件环固定 256 条，读操作不消费。`ring_overwritten` 是环累计覆盖数量；`lost_before_page` 是相对于该游标缺失的条数，两者含义不同。

栈采集在命中现场进行，最多 8 条 FP frame，通过 nofault 读取，不分配内存、不触发换页。`unwind.method=captured_fp_chain`，`complete=false`，明确报告 frame 终止原因：0=链尾，1=预算耗尽，2=不可读，3=非法/非递增 FP。返回的是原始返回地址；没有冒充 DWARF 展开、Java 栈或已剥离 PAC/已符号化的完整调用栈。无 FP 或省略 frame pointer 的函数可能无法恢复。

## Trace 会话

首先选择一个**当前工具 UID 私有、0700 的目录**；父目录必须已存在。XSettings 可使用其 app files 下的目录：

```json
{"operation":"trace.configure","params":{"directory":"/data/user/0/com.android.xsettings/files/lsdriver-traces"}}
```

选择目标后启动：

```json
{"operation":"trace.start","params":{"start_pc":"0x12340000","max_steps":1000,"duration_ms":5000}}
```

- 首版仅主线程。复用现有 stepbp 后端，从安装时开始单步，PC 到达 start_pc 后才保存事件；不是等待执行断点后才开始单步。
- `max_steps=1..1000000`；`duration_ms=1..60000`，包含等待触发时间。start_pc 必须是对齐的用户态 AArch64 地址。
- Trace 与断点事件流、已有断点互斥。活动扫描/指针任务会阻止启动；Trace 活动期间不允许切换目标。
- 在单步异常中记录当时的用户 PC，属于软件单步观察，不是 ETM、无开销跟踪或“完整且无丢失”的指令流保证。
- `trace.status` 查看当前状态；`trace.stop` 停止、排空最后的环记录并刷新文件；达到预算、时限、目标退出时自动收尾。
- 若后端清理失败，`cleanup_pending=true`，继续阻止目标切换并允许再次 `trace.stop` 重试。不要把文件已关闭当作后端已解除单步。
- `trace.list` 枚举持久会话；`trace.read` 按 session_id、after、count（1..500）读取文件分页。

```json
{"operation":"trace.read","params":{"session_id":"trace-123456","after":"0","count":100}}
```

每个文件包含版本头、事件 JSONL 和结束状态。单文件事件预算 8 MiB，另保留有界 footer 空间，最多 32 个会话文件；不会自动删除旧证据。达到上限需先由操作者归档文件。拒绝会话 ID 路径穿越、末级符号链接、非普通文件和超大/无效文件。

结果报告 `captured/saved_events/saved_through_seq/lost/ring_overwritten/reason/stop_status`。`lost` 在结束时包括所有未保存的事件；采集中仅反映已观察到的缺口。主机重启后缺少结束记录的会话显示 interrupted。启动失败会清理新建的无效文件。

## 兼容和部署

扩展仅追加操作 27，复用既有 4096 字节 vmemrw 缓冲。原操作号、`request_obj` 大小和字段偏移保持不变；不扩大旧内核会 pin 的页数。旧内核的未知操作分支保持预置 EOPNOTSUPP，不能把空响应识别为支持。

此批不改变原有握手/共享内存生命周期，也没有给旧的 `IoCommitAndWait` 添加超时；驱动失去响应仍可能阻塞。这需要后续独立设计，不能在不知道请求是否完成时复用通信缓冲。

先应用主仓库补丁，再同步内核与 XSettings fork；fork 补丁保留 JNI、共享库构建、日志、单例、AMS pid 解析等差异。更新内核需要重建 bootimage/ROM；更新 app 必须使用匹配原生库，不能把本次静态验证视作已经部署。

Windows MCP 同时兼容当前项目使用的 MCPServer API 和本机安装的 SDK 1.x FastMCP API；保持原 streamable-http 启动方式。`.exe` 尚未重打包。
## MCP 工具与调用顺序

| MCP 工具 | RPC / 主要参数 |
|---|---|
| `android_bridge_capabilities()` | `bridge.capabilities` |
| `android_memory_read(address, size, allow_partial=false)` | `memory.read`；size=1..1048576 |
| `android_breakpoint_events_start()` / `android_breakpoint_events_stop()` | 开始/停止事件采集；开始前选择目标 |
| `android_breakpoint_events_read(stream, after="0", count=8)` | `breakpoint.events.read`；count=1..8 |
| `android_breakpoint_backtrace(stream, event_seq)` | `breakpoint.backtrace`；查询仍在环中的指定事件 |
| `android_trace_configure(directory)` | `trace.configure`；绝对路径、当前 UID 私有 0700 目录 |
| `android_trace_start(start_pc, max_steps=1000, duration_ms=5000)` | `trace.start`；步数 1..1000000，时限 1..60000 ms |
| `android_trace_status()` / `android_trace_stop()` | 查看状态 / 停止并清理 |
| `android_trace_list()` | 枚举持久会话 |
| `android_trace_read(session_id, after="0", count=100)` | 分页读已保存事件；count=1..500 |

先查询能力，再选择目标。事件流程为 start → 安装断点 → read/backtrace → clear → stop。`breakpoint.events.status` 可用原始 RPC 调用，当前没有同名 MCP 包装。序号与 stream 使用十进制字符串，地址使用 `0x...` 字符串，避免 JavaScript 53 位整数精度损失。

Trace 流程为 configure → start → status → stop → list/read。XSettings 进程重启后需重新 configure；目录里的历史会话仍可读取，缺少结束记录的文件显示 interrupted。`cleanup_pending=true` 时再次调用 stop，成功解除前不要切换目标。文件满 32 个时由操作者归档，不自动删除。
