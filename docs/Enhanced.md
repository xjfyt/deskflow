# Deskflow Enhanced 验收说明

增强入口由 `BUILD_ENHANCED` 编译开关控制。打开普通 Deskflow，按原方式启动，仍使用经典模式。增强文件接收和 Windows 双向输入各自需要在两端明确启动。Windows 双向输入运行于交互用户会话，开始前需停止两端的经典 core。

## 文件与剪贴板

1. 在两台机器打开 **文件 → 增强文件传输**。在各自界面核对对方的完整 SHA-256 指纹，再填入对方指纹。
2. 接收端填写本机的局域网 IP，点击 **接收…** 并选择现有保存目录。发送端填写接收端 IP。两端端口一致，控制与数据端口分别默认为 TCP 24801 和 24802。
3. 在发送端添加文件、目录或已复制的文件，也可把文件拖入传输窗口。点击 **发送列表中的文件**。要从文件管理器触发，先点击 **发送下次文件复制**，然后在 90 秒内按 Ctrl+C 或 Cmd+C。应用会按复制处理剪切项。
4. 接收端校验全部内容后会在指定目录中公布一个 `Deskflow-<UUID>` 文件夹。点击 **复制收到的文件**，然后在文件管理器粘贴。也可以从接收窗口拖出文件到这台电脑的文件管理器。接收前勾选自动发布时，仅在本地剪贴板保持原修订的情况下自动发布。
5. 换方向时重新启动一次接收。取消、断线或校验失败会撤销本次未完成的暂存。已完成目录保留在用户选定的位置，由用户管理。

先重复验收中文名、空文件、空目录、多选和 300 MiB 文件。再检查剪贴板在接收期间被改动时是否保留用户内容，检查源文件与接收文件的 SHA-256 和目录结构，并确认同名接收得到独立批次。

接收端只接受本次明确启用的传输。若发送端提示无法确认提交，先到接收端查看收件目录，再决定是否重传。重传生成独立批次。

在 Windows 或支持 PowerShell 7 的机器上，可以用清单工具对比一组测试文件。在源机把清单写到测试目录之外，复制清单到目标机后执行第二条命令。

```powershell
pwsh -File docs/validation/Get-DeskflowFileManifest.ps1 -Root 'C:/测试用/源目录' > C:/测试用/预期.json
pwsh -File docs/validation/Get-DeskflowFileManifest.ps1 -Root 'C:/收件/Deskflow-批次/源目录' -ExpectedManifest 'C:/测试用/预期.json'
```

## 命令行文件传输

每台电脑生成自己的身份文件。记录命令输出的指纹，仅交换指纹。

```powershell
.\deskflow-core.exe --enhanced-files identity --certificate "$env:LOCALAPPDATA/DeskflowDev/peer.pem"
```

以下示例将 A 的文件发送给 B。将占位指纹换成对端完整指纹，B 的 IP 换成实际地址。接收端 B 先执行第一条，再在 A 执行第二条。

```powershell
.\deskflow-core.exe --enhanced-files receive --certificate "$env:LOCALAPPDATA/DeskflowDev/peer.pem" --peer '替换为 A 指纹' --address 192.168.1.20 "C:/已有接收目录"
.\deskflow-core.exe --enhanced-files send --certificate "$env:LOCALAPPDATA/DeskflowDev/peer.pem" --peer '替换为 B 指纹' --address 192.168.1.20 "C:/样例文件.txt" "C:/样例目录"
```

两条命令均可通过 `--control-port` 和 `--data-port` 指定替代端口。macOS 和 Linux 使用相同参数，替换本机命令名、证书和文件路径。GUI 和 CLI 可以互通，选择互通时核对实际使用的增强身份指纹。

## Windows 双向键鼠原型

两台 Windows 主机使用同一版构建，在普通用户的交互桌面停止经典 core。将 A 放在布局左侧，B 放在右侧。

1. 打开两端的 **文件 → 增强输入（Windows 预览）…**。核对并填入对端 SHA-256 指纹。
2. A 选择 **监听**，填本机局域网 IP，边缘选 **右侧**。B 选择 **连接**，同样填 A 的局域网 IP，边缘选 **左侧**。两端使用同一端口，默认 TCP 24803。
3. A 先启动，B 再启动。窗口会显示当前控制者。勾选 **记住此已验证对端** 后，首次配对成功会保存一组输入配置。下次仍由用户手动启动，取消勾选即可忘记。
4. 在两边用本机实体设备验证越界、滚轮、按钮和按键。本机操作可以接回控制。关闭窗口、点击 **停止** 或按 `Ctrl+Alt+Shift+F12` 会结束增强会话。恢复经典模式时，在增强进程退出之后重新启动经典 core。

命令行仍可使用。分别填写本机证书和对端指纹，A 先监听，B 再连接。

```powershell
.\deskflow-core.exe --enhanced-control listen --certificate "$env:LOCALAPPDATA/DeskflowDev/peer.pem" --peer '替换为 B 指纹' --address 192.168.1.10 --edge right
.\deskflow-core.exe --enhanced-control connect --certificate "$env:LOCALAPPDATA/DeskflowDev/peer.pem" --peer '替换为 A 指纹' --address 192.168.1.10 --edge left
```

返回相邻边缘可还回控制。目标侧的本机键盘、鼠标按键和滚轮可立即抢回。鼠标移动累计约 8 像素时也可抢回。应额外检查按住修饰键时断网、锁屏、重连前恢复本地控制，以及高 DPI 或非矩形显示器布局。

## 当前能力和验收范围

| 功能 | 代码状态 | 验收 |
| --- | --- | --- |
| 经典模式 | 保留原有协议、界面入口和平台路径 | 上游 Windows x64 构建已完成，经典双机互通待验收 |
| 文件与目录 | GUI 和 CLI 的双端指纹认证、独立数据通道、分块哈希和取消 | Windows、macOS、Linux 文件管理器粘贴与 1 GiB 并发样例待验收 |
| 双向输入 | Windows 两台机器、左右边缘、图形状态与停止、单对端配置、双端发起 | 真实设备、热键、物理输入过滤、各 DPI 布局与延迟待验收 |
| 文件拖放 | 源端窗口选择，接收后可拖到接收端的文件管理器 | 各桌面拖放交互待验收。Explorer、Finder 和 Linux 的原生跨屏交接待开发 |
| 后续 | macOS/Linux 双向输入、三机路由、断点续传、设备列表和统一配对管理 | 待开发 |

Wayland 剪贴板能力取决于当前会话和合成器。文件窗口可以通过文件选择和接收目录操作。Windows 输入原型面向普通交互桌面。当前布局使用虚拟桌面的最左或最右边缘，未提供跨多个物理屏幕的任意拓扑。

## 可复现验证

- Windows 编译环境和经典构建命令见 [P0 基线](../P0_BASELINE.md)。构建增强版时使用独立的 `build/enhanced` 并打开 `BUILD_ENHANCED`。
- 单测覆盖帧版本和序号、清单边界与路径校验、目标就绪、租约竞争和清理。回环用例覆盖双端指纹、分块传输、未认证连接、取消和文件被替换。把 Qt 和 OpenSSL 的 DLL 目录加到 Windows 的 `PATH`，然后执行 `ctest --test-dir build/enhanced/src/unittests -R Enhanced --output-on-failure`。
- [按原计划复核及验收矩阵](ENHANCED_AUDIT.md) 列出行为级验收步骤、预期和构建边界。
