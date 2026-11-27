# Deskflow Enhanced 打包与发布

公开仓库的默认分支为 `deskflow`。增强版标签和上游版本标签使用独立前缀。

增强版的公开发行版本取自标签，例如 `enhanced-v0.1.0` 对应增强版 0.1.0。应用和安装包内部保留上游 Deskflow 版本序列，以便沿用现有安装升级规则。

| 触发方式 | 产物去向 |
| --- | --- |
| `enhanced-ci-20261124.1` 之类的测试标签 | GitHub Actions 的工作流产物 |
| 在 **Actions → Enhanced packages** 手动运行 | GitHub Actions 的工作流产物 |
| `enhanced-v0.1.0-rc.1` 之类的预发行标签 | 六个打包任务全部成功后发布到 GitHub Releases，标为预发行版 |
| `enhanced-v0.1.0` 之类的正式标签 | 六个打包任务全部成功后发布到 GitHub Releases |

## 产物

| 系统 | 架构 | 文件 |
| --- | --- | --- |
| Windows | x64、ARM64 | MSI、ZIP |
| macOS | Intel、Apple Silicon | DMG |
| Debian 13 | amd64、arm64 | DEB |

打包会检查 Windows ZIP 中的主程序、core 和 `deskflow-file-agent.exe`，校验 DMG 镜像以及 DEB 的架构和安装位置。各架构运行证书、文件传输、输入协议和合成中继用例。Windows 额外运行自动文件剪贴板的协议、缓存和收发用例。发布任务复核八个包和 SHA-256 后再创建 Release。Windows 包使用现有 CPack/WiX 安装规则。验证后台服务模式时使用 MSI。便携 ZIP 有自己的设置文件，可用于普通用户模式。macOS CI 包采用临时本地签名，Developer ID 发行签名和 Apple 公证需要发行者的凭据。实机测试和签名状态应单独记录。

正式标签的发布任务会重新计算全部八个包的 SHA-256，并与各架构的清单逐项比对。若标签已有 Release，重跑会核对是否已发布、发行类型是否匹配，以及八个包和 `SHA256SUMS` 是否齐全。残留草稿或缺少资产会使任务失败，原 Release 保留供检查。

## 打测试标签

```powershell
git switch deskflow
git pull --ff-only personal deskflow
git tag enhanced-ci-20261124.1
git push personal refs/tags/enhanced-ci-20261124.1
```

在该标签的 **Enhanced packages** 运行详情查看六个架构的输出。修复失败后从修复提交创建下一测试标签。采用新标签可以保留失败运行和对应源码。

各轮打包、回环测试和实机边界见 [验证记录](VALIDATION_REPORT.md)。
