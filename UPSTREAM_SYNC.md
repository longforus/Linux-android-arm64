# 上游跟进检查点

| 项目 | 当前记录 |
| --- | --- |
| 上游仓库 | https://github.com/lsnbm/Linux-android-arm64 |
| 上游分支 | `main` |
| 已审查到的提交（含该提交） | **`a7f7da248a9b0a4b4615fd5219d47ee89e3d91f6`** |
| 提交标题 | `fix(kernel): 补充隐藏进程的 CPU 时钟查询拦截` |
| 跟进日期 | 2026-11-14 |
| 吸收方式 | 选择性移植；检查点表示已审查到的位置，不表示整体合并了上游 |
| 本仓库实现提交 | `eaf8e3e0eb9766a0a3231c4eb905dcf55fafb8ab` |
| xsettings 同步提交 | `d2625410c5d01142007410775f75457228bed618` |
| Pixel 6 内核树同步提交 | `f7782c1cc5ad9bfd6484b810b0cd9af7937d2f73` |

## 下次从哪里开始

**只审查上述提交之后的新增上游提交**。拉取后按这个范围列出新增内容：

```bash
git fetch https://github.com/lsnbm/Linux-android-arm64.git main
git log --reverse --oneline a7f7da248a9b0a4b4615fd5219d47ee89e3d91f6..FETCH_HEAD
git diff --stat a7f7da248a9b0a4b4615fd5219d47ee89e3d91f6 FETCH_HEAD
```

新增提交的审查基准使用上次的上游检查点，避免直接比较两条分叉分支时反复处理已审查差异。移植时仍须对照本仓库当前实现，保留本地功能、协议及 Pixel 6 / xsettings 适配。

上游若重写历史，先确认检查点是否仍为其祖先，再确定新的比较范围，不能直接把当前 main 当作已跟进。

## 本轮明确未吸收

- 用户明确排除：**vivo vermagic、旧 CFI 厂商兼容**；后续跟进默认继续排除，除非用户改变要求。
- 其余保留项：PAC/BTI/LTO 构建配置变更、CPU/DRM 渲染、进程隐藏扩展、外网 ping、未落地的模拟访存保护、缓存同步重写。这些在本轮已审查，不是漏合并；后续若有实质修复，可重新评估。

本轮具体移植说明：[UPSTREAM_INTEGRATION.md](UPSTREAM_INTEGRATION.md)。全量 ROM、刷机与设备回归：[验证报告](reports/upstream-20261114/REPORT.md)。

每次完成一轮跟进后，更新本文件的已审查提交、日期、实现提交及未吸收项；尚未审查完的新提交不能提前推进检查点。
