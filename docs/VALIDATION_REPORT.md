# Deskflow Enhanced 验证记录

记录日期为 2026-11-24。官方基线为 `80221f0f7b7ebd807fb1ffdf00906bbcfe561011`。

## 标签打包

测试标签 `enhanced-ci-20261124.3` 对应提交 `aa0435ea8`。六个架构均完成打包、包检查和增强协议、文件传输、证书测试，共上传六组产物、八个安装文件。该标签只生成 Actions 产物。 [完整运行](https://github.com/xjfyt/deskflow/actions/runs/45306387765)

| 系统 | 架构 | 结果 |
| --- | --- | --- |
| Windows | x64、ARM64 | MSI 和 ZIP 已生成，ZIP 包内程序检查通过 |
| macOS | Intel、Apple Silicon | DMG 已生成，镜像校验通过，macOS 输入后端完成 Apple SDK 编译 |
| Debian 13 | amd64、arm64 | DEB 已生成，架构和安装位置检查通过，X11 输入后端完成构建 |

测试标签 `enhanced-ci-20261124.5` 对应提交 `0ffa7a3bd`。六个架构全部通过打包、三个 CTest 套件和合成三机中继测试，并上传六组产物。macOS 从只读挂载的 DMG 启动中继，Windows 从解压后的 ZIP 启动中继。测试标签的正式发布任务已跳过。 [终版复验运行](https://github.com/xjfyt/deskflow/actions/runs/45308174709)

## Windows 本地验证

代码提交为 `79d6b03a0`。Windows x64 用 MSVC、Qt 6.9.3 和 OpenSSL 3.6.5 编译经典与增强配置。 `EnhancedProtocolTests`、`EnhancedFileTransferTests` 和 `SecureUtilsTests` 三个套件通过，覆盖源文件变更、错误指纹、拖放接收授权和单文件续传。

从本地便携包运行 `test_input_hub.py`，两个临时 TLS 端点分别取得租约和转发，断开任一侧后关闭剩余连接。此用例在回环地址运行，未触发桌面输入捕获。

图形界面单文件续传交付后，Windows x64 增强版重新编译，`EnhancedProtocolTests`、`EnhancedFileTransferTests` 和 `SecureUtilsTests` 再次通过。文件传输用例验证了非零前缀恢复、篡改拒绝、检查点清除，以及再次清除无检查点时的结果。图形界面双机、重启及磁盘空间不足场景待实机验收。

发布步骤通过 `actionlint`，并在 Bash 使用模拟的 GitHub CLI 返回验证新建、已完整发布、残留草稿、附件缺失、发行类型错误和校验清单重复。正式标签上传和发布待验收。

## 待补实机记录

使用 [实机验收矩阵](ENHANCED_AUDIT.md#重点回归) 填写系统版本、构建 SHA、屏幕布局、方向与操作结果。重点为 Windows、Xorg 与 macOS 的物理越界和断网释放，Finder/Explorer 的拖放实际落点，三台物理设备上的转发延迟，以及跨设备续传后最终 SHA-256。

平台构建、协议测试、签名和真实桌面行为分别记录。macOS CI 镜像使用临时签名，Developer ID 签名和 Apple 公证待发行验收。
