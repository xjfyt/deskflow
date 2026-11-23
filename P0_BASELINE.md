# Deskflow 增强版基线

| 项目 | 记录 |
| --- | --- |
| 核对日期 | 2026-11-23 |
| 上游 | `deskflow/deskflow` 的 `master` |
| 上游提交 | `80221f0f7b7ebd807fb1ffdf00906bbcfe561011` |
| 分支 | `feature/omni-control-foundation` |
| 本机 | Windows x64，系统版本 10.0.26300 |
| 编译环境 | MSVC 19.44.35230，Windows SDK 10.0.26100，CMake 4.3.1，Ninja 1.13.2 |
| 依赖 | Qt 6.9.3 MSVC x64，OpenSSL 3.6.5 vcpkg x64-windows |
| 构建结果 | 干净的上游 worktree 完成 Release 构建，生成 GUI、core、daemon 和单测可执行文件 |
| 产物 | `%LOCALAPPDATA%/DeskflowDev/upstream-classic/build/classic/bin/` |
| 实机和自动化执行 | 本次仅构建。Windows 双机键鼠、普通剪贴板、TLS 互通，以及 macOS、Linux 编译与实机交互待验收 |

已执行 `git fetch --no-tags origin master`。上游与工作区改动前的 HEAD 相同，`git rev-list --left-right --count HEAD...FETCH_HEAD` 返回 `0 0`。

## 构建复现

在 VS 2022 x64 Developer PowerShell 中运行。Qt 和 vcpkg 缓存位于当前用户的 `DeskflowDev` 目录，便于复用到经典与增强两个构建目录。

```powershell
$deps = Join-Path $env:LOCALAPPDATA 'DeskflowDev'
$qt = Join-Path $deps 'Qt/6.9.3/msvc2022_64'
$openssl = Join-Path $deps 'vcpkg/installed/x64-windows'
$env:Path = "$qt/bin;$openssl/bin;$openssl/tools/openssl;$env:Path"
cmake -S . -B build/classic -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_INSTALLER=OFF -DSKIP_BUILD_TESTS=ON "-DCMAKE_PREFIX_PATH=$qt" "-DOPENSSL_ROOT_DIR=$openssl"
cmake --build build/classic --parallel 8
```

执行记录来自上游提交的独立 worktree。增强代码合入后，可用 `-DBUILD_ENHANCED=OFF` 构建经典配置。原有 Debian、Fedora、macOS 和 Windows 构建基础见 [上游构建说明](docs/dev/build.md) 与 [CI](.github/workflows/continuous-integration.yml)。

## 代码路径

- `deskflow-core` 在 Qt 应用中为经典 `ServerApp` 或 `ClientApp` 启动核心线程，复用各自事件队列。GUI 经 `CoreProcess`、`CoreIpcClient` 和 Windows daemon 管理核心生命周期。
- `Server` 管理屏幕拓扑、焦点和剪贴板。原生 Primary 事件经 `PrimaryClient` 到达服务端，远端客户端经 `ClientProxy` 接收。Secondary 的 `Client`、`ServerProxy` 与平台注入构成另一侧。
- `Computer` 在 `PlatformComputer` 外区分 Primary 与 Secondary 的生命周期。Windows 的 `MSWindowsComputer` 和 `MSWindowsHook` 管理本机桌面与 Win32 hook。macOS 对应 `OSXComputer`，X11 对应 `XWindowsComputer`，Wayland 对应 `EiComputer`、`PortalInputCapture` 和 `PortalRemoteDesktop`。
- `IClipboard::Format` 目前涵盖文本、HTML、位图。经典 `ClientProxy1_5` 中 `DFTR` 和 `DDRG` 的接收处理没有文件引擎。独立增强协议可以隔离文件路径和兼容协商。
- 经典接口、TLS 指纹格式、默认端口与消息参考见 [上游协议说明](docs/dev/protocol_reference.md)。

## 参考实现与许可

| 来源 | 设计参考 | 本项目采用 |
| --- | --- | --- |
| [Deskflow](https://github.com/deskflow/deskflow) | 平台生命周期、经典协议和证书生成 | 以官方分支为基线，复用仓库内 OpenSSL 证书函数 |
| [deskflow-filecopy](https://github.com/EugeneEvan/deskflow-filecopy) | Windows 用户会话、预缓存后发布文件 | 独立实现，在 Qt 交互用户进程中处理文件 |
| [deskflow-file-transfer](https://github.com/tutoupifengxiaa/deskflow-file-transfer) | Win/macOS 文件剪贴板和拖拽适配的分工 | 独立实现文件选择和传输，后续原生交接需要单独的系统验证 |

未引入参考 Fork 的源文件。增强 C++ 源码遵循仓库的 `GPL-2.0-only WITH LicenseRef-OpenSSL-Exception`，构建文件使用 MIT。

## 经典验收记录模板

| 操作 | 验收结果 |
| --- | --- |
| 原版 A → B 键鼠越过屏幕边缘，返回后本机按键与鼠标可用 | 待双机 |
| 经典客户端与本分支经典服务端通过 TLS 指纹互通，反方向重复 | 待双机 |
| 双向文本、HTML、图片和多客户端焦点切换 | 待双机 |
| Windows 服务安装、托盘启动、登录及电脑休眠恢复 | 待 Windows 实机 |
| macOS Accessibility、X11 和 Wayland Portal 的经典行为 | 待相应系统 |
