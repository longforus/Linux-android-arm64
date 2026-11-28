# 2026-11-28 选择性移植、构建、刷机与验证

上游基准 `dc99e8dc7a228520f0dbd5cd288355dbbc17f433`，本地起点 `b90379f`。以当前上游状态为准，不恢复丢弃的历史。

本轮落实：

- HTTP：独立请求解析器，整请求 15 秒截止时间、严格头部/版本校验、拒绝 Transfer-Encoding、区分头部和 body 大小；连接 fd 自动清理，发送超时和有界等待队列。保留本地 RPC/MCP、62 工具、health 能力与 JNI 日志。
- hook：BTI JC + 五条指令入口、五条原始序言回放，调整跳板控制流；支持 B/BL/literal 重定位，拒绝 literal 与覆盖入口相交和范围溢出；保留本地条件分支近端 branch island。未改架构、PAC/LTO 构建配置。
- 连接：RCU 下读取 real_parent，排除已连接 LS 的直接 fork 子进程；比较父 task 指针，不新增对旧 client task 的字段解引用。既有进程扫描引用生命周期未在本轮整体重构。

同步：内核树整体镜像（排除 byte_stream）；xsettings 仅 HTTP 两个文件，保留 fork 适配。未变更用户态/内核协议布局。

已通过：HTTP 主机解析与 15 秒超时、encoder、生产跳板布局、生产重定位/decoder 联测、初始化故障注入、MCP 协议/catalog、扫描契约；xsettings assembleRelease；新 APK 在刷机前现有内核上 26 项 API 回归及新增 HTTP 非法请求、40 次并发 health、15 秒慢连接回收。

构建过程纠正：最初直接 podman exec 未按项目规定进入 distrobox，导致错误生成源码树 out；已停止、清理该目录，纠正构建输出文件权限。后续使用 `distrobox enter aosp-build -- bash -ic`，source envsetup、croot、lunch lineage_oriole-user、mka bootimage -j12；日志明确 OUT_DIR=/mnt/sdwd/android_build/out/pixel6_20，CCACHE 使用原配置。早期失败/中断日志保留，不能视为成功构建证据。

## 最终结果

- 按 AGENT.md 使用 distrobox 进入构建环境；boot 构建成功（08:20），标准 OTA `m bacon -j12` 成功（10:17）。
- OTA：`/mnt/sdwd/android_build/out/pixel6_20/target/product/oriole/lineage-20.0-20261128-RELEASE-oriole.zip`，1,172,057,174 字节，SHA256 `1ccf34e52fc4fd7a2634b59cce08b27182f69d99334ea02670402b67c2c9c398`。
- 新 APK 已复制至 ROM 源码 `device/google/raviole/xsettings/com.android.xsettings.apk`。Gradle 产物与源码 APK SHA256 一致；镜像 staging APK 内 `libLS_KTool.so` SHA256 也一致，见 apk-packaging.json。设备另执行覆盖安装并校验完整 APK 哈希，见 installed-apk.json。
- Recovery sideload-auto-reboot 成功，返回 0，Total xfer 2.00x。自动启动到 `20.0-20261128-RELEASE-oriole`，B 槽，SELinux Enforcing、root、KPM 3337 正常。未擦除用户数据。
- boot-dmesg.txt 确认各 hook 入口为 `d50324df`（BTI JC），真实覆盖五条指令；未发现 panic/Oops。
- 刷机后 26 项 API 断言通过：RPC/MCP、62 工具、内存读写及独立 fixture 观察、并发读、读取完整性、整数/浮点/跨页扫描、反汇编、HWBP/PTEBP/STEPBP、事件/FP 回溯、有界 trace 与收尾。见 device-results.json、device-test-postflash.log。
- HTTP 真机检查通过：非法 HTTP 版本、Transfer-Encoding、重复 Content-Length 返回 400；40 次并发 health 成功；慢连接在 14.995 秒关闭。见 http-device.json、http-test-postflash.log。
- HARD_STOP/START_HTTP 后约 3.01 秒重新握手，PID 改变、target 重置为 0，旋转设置不变。见 restart-results.json。
- wxshadow 回归 20 通过、0 失败：补丁读隐藏、fork 保持、mprotect 撤销、退出、运行时开关；无坏页/rss/panic。见 wxshadow-test.log。
- 最后普通重启验证通过，版本/B 槽/Enforcing/root/KPM/HTTP 正常，target=0、KPM 独立模块列表为空、旋转设置保持原值。KPM 开机初次查询尚未就绪，脚本有界等待后成功，见 normal-reboot.log 与 normal-reboot-results.json。

未进行长期压力测试或所有上游新功能测试；Surface、触摸/资源重构、DPTDBG 专项等仍按审查报告延期。本轮未做全量上游合并。提交与同步进度见 UPSTREAM_SYNC.md。


## 刷机后仅更新源码版本号

按用户追加要求，`lsdriver/io_version/version.h` 已由 `2.4.0` 更新为本轮上游快照的 `3.5.11`，同步到内核树。**没有重新编译或刷机**；上述已部署固件和测试记录仍对应运行时 `2.4.0`。下次统一构建后版本号才会在设备生效；版本对齐不表示全量合并上游。

原始 `*-dmesg.txt` 与早期失败/中断构建日志保存在本机 ROM 目录 `/home/void/lineageos/pixel6_20/reports/lsdriver-upstream-20261128/`，不将大量原始内核日志混入主仓库提交。其余构建、测试脚本、断言结果与产物校验记录随本报告提交。
