# 上游更新全量构建、刷机与设备验证（2026-11-14）

## 结果

完整 `lineage_oriole-user` / `m bacon -j12` 成功，耗时 **16:07**。Recovery `sideload-auto-reboot` 安装返回 0（`Total xfer: 2.00x`），正常启动到 **20.0-20261114-RELEASE-oriole / A 槽**。随后普通重启复验通过。

未擦除用户数据，使用标准完整 OTA；没有额外刷写 bootloader 或标准 OTA 之外的分区。vivo vermagic 和旧 CFI 厂商兼容未吸收。

## 构建与产物

- 环境：现有 `aosp-build` 容器、`lunch lineage_oriole-user`，沿用编译缓存构建完整系统和 OTA。
- OTA：`/mnt/sdwd/android_build/out/pixel6_20/target/product/oriole/lineage-20.0-20261114-RELEASE-oriole.zip`。
- 大小：`1164886277` 字节。
- SHA256：`760325ff37952111698886e11c2384d5e3e1410dbed3a9fadb1adf4670d76943`。
- xsettings APK 已复制至 ROM 源码并刷新打包中间产物；APK 与系统镜像 staging 的 `libLS_KTool.so` 哈希一致。
- 手机已有 `/data/app` 更新版覆盖系统 APK，因此刷机后执行 `adb install -r` 更新同一批 APK，并验证设备 APK 的 SHA256 与本地一致。
- 宿主机缺少 bc，且之前局部编译产生过宿主 libc 头文件依赖。完整构建使用已有容器的 bc，并重建了隔离移走的 BTF 主机工具缓存；没有修改产品内核配置。

## 设备回归

受控进程 `6881`，仅操作自建 fixture 的内存。完整调用证据见 [device-results.json](device-results.json)。

| 项目 | 实测 |
| --- | --- |
| MCP | 2025-11-25 协商、57 个工具、新扫描 schema、媒体类型和 Origin 校验通过 |
| RPC/MCP 状态共享 | 两个入口读取同一目标 PID |
| 内存读写 | 完整读取、写入读回、独立 fixture 观察到修改、恢复原值 |
| 并发请求 | 8 个并发客户端完成 64 次有效读取 |
| 读取完整性 | 未映射地址返回有效性元数据，严格模式拒绝不完整读取 |
| 扫描 | 未对齐整数、保留对齐的再次扫描、rounded 浮点、exact 过滤、跨页浮点全部通过 |
| 反汇编 | 查看器刷新通过 |
| HWBP / PTEBP / STEPBP | 分别 20 / 20 / 21 次有效命中，均成功移除 |
| 事件与回溯 | 事件流读取、命中现场 FP 回溯通过 |
| 有界 Trace | 64 条捕获与保存、丢失 0、step_limit 收尾、stop_status=0、cleanup_pending=false，文件读取通过 |
| Vulkan 旋转 | 竖屏/横屏使用同一 Lark 图层、2400×2400 buffer；截图人工检查未见拉伸 |
| 服务重启 | HARD_STOP 后新进程约 3 秒重新握手；target PID 重置为 0 |
| 普通重启 | 系统、root、KPM 自动加载、HTTP、空 target、原旋转设置均通过 |

API 断言共 **26 项**，全部通过；另外完成图形、服务重启与普通重启检查。服务冷启动及 KPM 开机加载采用有界就绪等待，`boot_completed=1` 并不代表所有后启动服务已就绪。

## 最终状态与证据

- SELinux：Enforcing；root：uid=0。
- KPM：3337 (Pixel 6 KPM)，最终模块列表为空；配套 kernelpatch.ko 哈希与构建 staging 一致。
- fixture 已终止；测试内存恢复，断点、事件采集和 trace 清理均成功；最后普通重启后的 target 为 0。
- 自动旋转和用户旋转恢复为 `1` / `0`。HTTP 前台服务保留可用。
- 初次、回归后及普通重启内核日志中均未发现 `Kernel panic - not syncing` 或 `Oops:`。

截图：[竖屏](portrait.png)、[横屏](landscape.png)。状态：[旋转](rotation-results.json)、[服务重启](restart-results.json)、[普通重启](normal-reboot-results.json)、[产物](package.json)。受控 fixture 和测试脚本已一并留存，完整构建、内核日志及刷机证据另保存在 ROM 的 `reports/lsdriver-upstream-20261114/`。

本次未执行长期压力、全部虚拟输入功能或任意第三方 KPM 兼容测试。
