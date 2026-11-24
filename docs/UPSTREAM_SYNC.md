# 上游同步和私有分支维护

| 位置 | 用途 |
| --- | --- |
| `origin` | 官方 `deskflow/deskflow`，记录上游版本 |
| `personal` | 私有 `xjfyt/deskflow`，维护增强版 |
| `deskflow` | 私有仓库的默认分支，跟踪 `personal/deskflow` |
| `master` | 本地上游基线 |

## 核对上游

```powershell
git fetch --no-tags origin master
git fetch personal deskflow
git rev-list --left-right --count personal/deskflow...origin/master
git log --oneline personal/deskflow..origin/master
```

在独立分支合并待同步的上游提交，记录协议、平台适配和打包冲突。构建经典和增强配置，按验收说明记录双机结果后再合并到 `deskflow`。推送使用 `git push personal deskflow`。

重点关注 `deskflow-core.cpp` 的两种输入模式互斥、GUI 的会话入口、`translations` 的自动扫描，以及 `net` 的 OpenSSL 依赖。增强文件帧和经典连接各走独立通道，任何 DFEX 帧编号、阶段顺序或能力位的改动都需要核对已发布客户端。
