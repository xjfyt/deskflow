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

### 单文件断点续传

在相同的发送和接收命令中都加 `--resume`，发送端只选一个普通文件。中断或 Ctrl+C 后，使用原来的文件、对端指纹和本地保存目录重试。接收端保留的已完成块需要通过双端独立校验，发送端第一行进度会显示保留偏移。文件完成并验证全文件 SHA-256 后才公布正常收件目录。

每个接收目录仅保留一个检查点，已有检查点只能在 24 小时内恢复。文件变化、检查点损坏或指纹不同会拒绝恢复。确认不再需要该进度后，用以下命令移除检查点。传输正在使用该检查点时会拒绝清理。

```powershell
.\deskflow-core.exe --enhanced-files discard-resume 'C:/已有接收目录'
```

### 暂存式文件拖放交接

目标端勾选 **接受文件拖放并在本机放置** 并开始接收。源端点击 **发送下次文件拖放**，在 90 秒内把选区拖入本机的 Deskflow 窗口。收件校验完成后，目标端窗口提示把列表中的文件拖到本机文件管理器。全流程使用复制动作，保留两端剪贴板。两端都需支持并启用交接。

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

## Linux X11 双向键鼠预览

两台 Linux 电脑均需在普通用户的本机 Xorg 会话运行，使用相同版本、相对指针和兼容的键盘布局。需要 XI2.2、XTEST，以及 logind 提供的当前本机解锁会话。经典 core 退出后使用与 Windows 示例相同的 `--enhanced-control listen|connect` 参数，替换为 Linux 的程序和身份文件路径。在两个终端观察会话状态，按 Ctrl+C 结束。双机注入使用 X11 原生键码，Windows 和 macOS 使用各自的协议族。

Wayland 会话、远程 X display 和无法核实的本地会话会拒绝启动此入口。Wayland 双向输入仍需单独的 Portal/libei 适配。

## macOS 双向键鼠预览

两台 Mac 使用相同版本和单显示器的普通本机活动会话。停掉经典 core 后，从终端启动 `Deskflow.app/Contents/MacOS/deskflow-core --enhanced-control listen|connect`。提供和 Windows 示例相同的身份文件、对端指纹、IP、端口和左右边缘。按 macOS 提示，为实际运行的终端或应用授予辅助功能与输入监控权限。

启动后通过本机输入抢回，使用 Ctrl+C 或 Control+Option+Shift+F12 结束会话。系统接管事件监听、进入 Secure Input、切换桌面或屏幕布局变化时，会结束当前会话。媒体键、Globe 和 Caps Lock 暂不转发。权限变更、锁屏及断线时的事件释放须在实机上复验。

## 三机输入中继预览

让第三台电脑 H 作为协调节点，A 与 B 作为输入端点。H 可以运行任意已构建的系统版本，A 和 B 需要同一输入协议族与一致的版本。三台电脑分别生成身份。H 验证 A 和 B 的指纹，A 和 B 分别核对 H 的指纹并用 `--via-hub` 明确允许 H 转发。端点先停止经典 core。

以下示例使用 Windows 两端和 H 的 IP `192.168.1.10`。先运行 H，再启动 A 和 B。

```powershell
# H
.\deskflow-core.exe --enhanced-hub --certificate 'C:/DeskflowDev/hub.pem' --address 192.168.1.10 --family windows --peer-a '替换为 A 指纹' --peer-b '替换为 B 指纹'
# A
.\deskflow-core.exe --enhanced-control connect --via-hub --certificate 'C:/DeskflowDev/a.pem' --peer '替换为 H 指纹' --address 192.168.1.10 --port 24803 --edge right
# B
.\deskflow-core.exe --enhanced-control connect --via-hub --certificate 'C:/DeskflowDev/b.pem' --peer '替换为 H 指纹' --address 192.168.1.10 --port 24804 --edge left
```

等待 H 确认两个端点后，A 和 B 可以互相越过配置的边缘。任一端断开都会结束转发，端点按看门狗和本机状态释放注入的按键。H 只承担中继与仲裁。三个桌面均可参与的动态布局是后续阶段。

## 当前能力和验收范围

| 功能 | 代码状态 | 验收 |
| --- | --- | --- |
| 经典模式 | 保留原有协议、界面入口和平台路径 | 上游 Windows x64 构建已完成，经典双机互通待验收 |
| 文件与目录 | GUI 和 CLI 的双端指纹认证、独立数据通道、分块哈希和取消 | Windows、macOS、Linux 文件管理器粘贴与 1 GiB 并发样例待验收 |
| 双向输入 | Windows 双机图形入口、Linux X11 与 macOS 同系统双机命令行入口，各有左右边缘和双端发起 | 真实设备、热键、物理输入过滤、各 DPI 布局与延迟待验收 |
| 文件拖放 | 双端授权的暂存式交接，收件后由本机原生拖放落到目标文件夹 | 各桌面交互、落点与取消待验收。第三方应用实时跨屏接管待开发 |
| 断点续传 | 命令行双端启用后续传一个普通文件，验证前缀和全文件哈希 | 跨设备中断、过期清理和设备重启复验待执行 |
| 三机中继 | 协调节点固定绑定两个获准端点，选择同一协议族双向中继 | 三台物理设备和延迟待验收，协调节点本机作为输入端点及动态拓扑待开发 |
| 后续 | 跨系统按键映射、Wayland 输入、目录和多文件续传、统一配对管理 | 待开发 |

Wayland 剪贴板能力取决于当前会话和合成器。文件窗口可以通过文件选择和接收目录操作。Windows 输入原型面向普通交互桌面。当前布局使用虚拟桌面的最左或最右边缘，未提供跨多个物理屏幕的任意拓扑。

## 可复现验证

- Windows 编译环境和经典构建命令见 [P0 基线](../P0_BASELINE.md)。构建增强版时使用独立的 `build/enhanced` 并打开 `BUILD_ENHANCED`。
- 单测覆盖帧版本和序号、清单边界与路径校验、目标就绪、租约竞争和清理。回环用例覆盖双端指纹、分块传输、未认证连接、取消和文件被替换。把 Qt 和 OpenSSL 的 DLL 目录加到 Windows 的 `PATH`，然后执行 `ctest --test-dir build/enhanced/src/unittests -R Enhanced --output-on-failure`。
- 使用 `python docs/validation/test_input_hub.py build/enhanced/bin/deskflow-core.exe` 可在 Windows 验证中继协议。脚本只使用临时证书和本机模拟端点。
- [按原计划复核及验收矩阵](ENHANCED_AUDIT.md) 列出行为级验收步骤、预期和构建边界。
- [后续开发计划](ENHANCED_ROADMAP.md) 记录三机路由、跨系统输入、原生拖放和续传的验收门槛。
