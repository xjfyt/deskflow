# Deskflow Enhanced 打包与发布

公开仓库的默认分支为 `deskflow`。增强版标签和上游版本标签使用独立前缀。

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

打包会检查 ZIP 的主程序、DMG 的镜像校验和，以及 DEB 的架构和安装位置。各架构还运行证书、文件传输、输入协议和合成中继用例。发布任务复核八个包和 SHA-256 后再创建 Release。Windows 包使用现有 CPack/WiX 安装规则。macOS CI 包采用临时本地签名，Developer ID 发行签名和 Apple 公证需要发行者的凭据。实机测试和签名状态应单独记录。

## 打测试标签

```powershell
git switch deskflow
git pull --ff-only personal deskflow
git tag enhanced-ci-20261124.1
git push personal refs/tags/enhanced-ci-20261124.1
```

在该标签的 **Enhanced packages** 运行详情查看六个架构的输出。修复失败后从修复提交创建下一测试标签。采用新标签可以保留失败运行和对应源码。
