# 2026-11-28 当前上游跟进建议

## 范围与基准

- 本地开发分支：`myPixel6@b90379f`。
- 当前上游：`lsnbm/Linux-android-arm64/main@dc99e8dc7a228520f0dbd5cd288355dbbc17f433`（GitHub API 核实）。
- 本地 `main` 和 `myFork/main` 仍为 `aa3a577`，并非当前上游；本次未移动它们。
- 旧检查点 `a7f7da248a9b0a4b4615fd5219d47ee89e3d91f6` 与当前上游分叉。当前历史中，同标题、同日期的 CPU 时钟提交为 `330e94db`。按用户要求，以当前上游代码状态为准，不恢复被上游丢弃的历史。
- 本次是定向源码审查与移植建议，未修改运行代码、构建、刷机或同步其他工程。未声称全量审完所有历史及所有文件，因此不把旧检查点直接推进到当前 HEAD。

## 建议先做

### 1. HTTP 接收截止时间、严格解析及连接资源管理

参考 `a46b79ba`、`87f71394` 的当前结果：`android/jni/include/service/http_request.h` 与 `http_server.h`。

本地接收函数使用阻塞 `recv`，没有整请求截止时间；连接接受后直接排入线程池。慢客户端或不完整请求可长期占住工作线程。当前上游以 `steady_clock` 设置整请求 15 秒期限，使用 poll 与非阻塞 recv，并限制排队、设置发送超时和用 RAII 关闭 fd。解析还拒绝 Transfer-Encoding、非法字段名、无冒号头和非法 HTTP 版本；头大小只计算头部，不把同批收到的 body 算入头部上限。

适合单独移植接收器与连接管理，保留本地 ApplicationService、debug 服务、MCP catalog、health 能力字段及共享请求锁。验证应包含慢请求、分片 body、重复 Content-Length、Transfer-Encoding、超长头及正常 MCP/RPC。

### 2. Hook 入口 BTI/PAC 修复

参考 `73bc33dd` 当前保留的设计：入口使用 `BTI JC + 长跳转`，覆盖长度由四条变为五条；原始 PAC 序言仍在继续执行原函数时回放。

本地仍为四条入口直接长跳，没有合法 BTI landing pad。值得移植，但不能整文件覆盖：本地额外实现了条件分支近端 branch island，当前上游改成直接重编码，可能因短分支范围不足而拒绝本地能安装的 hook。需一起调整 replay、返回地址、metadata、branch slot 布局及静态断言，保留本地范围检查。不能连带采用上游 ARMv8.6/构建参数。

参考 `2238012e` 可再补 literal LDR/PRFM 重定位、指向入口覆盖区的分支重映射与 literal 覆盖区相交拒绝；B/BL 支持需逐项验证寄存器与 LR 语义。

### 3. 排除已连接 LS 的子进程，防误重连

参考 `2c3eee13`。本地连接扫描只检查 comm、当前 task 与启动时间；fork 子进程可能继承 LS 名称并被当作新连接。该修复有实际价值。

不要仅粘贴父进程判断：本地 for_each_process 和旧 task 的引用生命周期仍需审查。应在正确保护下读取 real_parent，避免新增未受保护 task 解引用。

## 值得分阶段跟进

- **Surface 私有 ABI 与释放兼容**：`ccf0907a`、`77653f1c`、`2238012e`。当前上游按不同符号/签名提供兼容，补析构符号缺失回退。适合和 renderer 生命周期一起审查；Pixel 6 需验证创建/销毁、旋转与录屏镜像，保留共享库多 TU inline 状态。
- **资源与异步任务所有权**：`a46b79ba`、`ef9ae3b3`、`87f71394`。RAII fd/file、任务收尾、渲染器生命周期与触摸线程停止值得借鉴。改动涉及 scanner/pointer/service/UI，不能用上游 runtime 覆盖本地 DebugService 和 JNI 单例设计。
- **触摸线程及锁优化**：`d546e3ca` 当前实现预分配 dispatch 缓冲，将归一化和 ImGui 调用移出锁。它依赖上游新的 TouchInput 类，不能孤立 cherry-pick 到本地旧接口；应与触摸生命周期改造一起处理。
- **DPTDBG 生命周期与 mprotect 同步**：当前 `arm64_dptdbg.h` 有分类在途引用、分阶段卸载、映射更新期间暂停私有视图与权限同步，方向有价值。但当前 context 仍持有 get_mm_by_pid 的 mm_users 直到卸载，不能解决本地已知退出/Binder 风险；mprotect 路径还改用不可中断 mmap 写锁。须独立设计和测试退出、force-stop、mprotect、munmap、fork、并发卸载及与 wxshadow 共存，暂不直接移植。

## 已有等价行为、暂不需要或排除

- `d2429c20` 的 Vulkan SUBOPTIMAL 修复：本地 acquire 已接受 SUBOPTIMAL，present 仅 OUT_OF_DATE 重建，已有等价行为。
- `31543d60` 的 UDF 编码修复：本地上轮已纠正立即数字段，不重复吸收。
- `151b773e` 禁用初始化 ping：本地早已移除，无须跟进。
- `dc99e8dc` 的 proc_pid_readdir 回放空指针检查：对上游新隐藏实现很关键，但本地仍是 filldir64 hook，没有此回放指针，当前不受该特定缺陷影响。以后移植进程隐藏时必须带最终修复。
- 进程隐藏扩展：本地 filldir64 过滤会误伤其他目录中的同名数字目录；当前上游改为 proc 范围过滤可作为修复参考。pidfd/CPU timer/fcntl/Binder/cgroup 等隐藏扩展仍按之前约定暂不吸收。
- `1faff7f3` PAGE_PINNER 与 `0d48b25a` 未导出符号兼容：主要服务外部模块/特定设备；Pixel 6 内置构建无必要为此覆盖配置或改变锁等待语义。
- vivo vermagic、旧 CFI 厂商兼容继续排除；不带入预编译 ko、installer 二进制载荷、panic 原始 dumps、临时日志与 pycache。CPU/DRM 渲染当前仍未接入主路径，不跟进。

建议顺序：HTTP → hook BTI 与重定位 → 连接扫描 → Surface/触摸/资源生命周期 → DPTDBG 专项。
