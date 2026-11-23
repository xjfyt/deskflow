# Deskflow Enhanced 验收说明

增强入口由 `BUILD_ENHANCED` 编译开关控制。打开普通 Deskflow，按原方式启动，仍使用经典模式。增强文件接收和 Windows 双向输入各自需要在两端明确启动。Windows 双向输入运行于交互用户会话，开始前需停止两端的经典 core。

## 文件与剪贴板

1. 在两台机器打开 **文件 → 增强文件传输**。在各自界面核对对方的完整 SHA-256 指纹，再填入对方指纹。
2. 接收端填写本机的局域网 IP，点击 **接收…** 并选择现有保存目录。发送端填写接收端 IP。两端端口一致，控制与数据端口分别默认为 TCP 24801 和 24802。
3. 在发送端添加文件、目录或已复制的文件，也可把文件拖入传输窗口。点击 **发送选中项**。要直接从文件管理器触发，先点击 **发送下次文件复制**，然后在 90 秒内按 Ctrl+C 或 Cmd+C。
4. 接收端校验全部内容后会在指定目录中公布一个 `Deskflow-<UUID>` 文件夹。点击 **复制收到的文件**，然后在文件管理器粘贴。接收前勾选自动发布时，仅在本地剪贴板保持原修订的情况下自动发布。
5. 换方向时重新启动一次接收。取消、断线或校验失败会撤销本次未完成的暂存。已完成目录保留在用户选定的位置，由用户管理。

先重复验收中文名、空文件、空目录、多选和 300 MiB 文件。再检查剪贴板在接收期间被改动时是否保留用户内容，检查源文件与接收文件的 SHA-256 和目录结构，并确认同名接收得到独立批次。

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

在两台 Windows 主机停止经典 core。将 A 放在布局左侧，B 放在右侧。从各自的普通用户终端启动下列命令，填写对应本地证书和相对侧指纹。A 先监听，B 再连接。

```powershell
.\deskflow-core.exe --enhanced-control listen --certificate "$env:LOCALAPPDATA/DeskflowDev/peer.pem" --peer '替换为 B 指纹' --address 192.168.1.10 --edge right
.\deskflow-core.exe --enhanced-control connect --certificate "$env:LOCALAPPDATA/DeskflowDev/peer.pem" --peer '替换为 A 指纹' --address 192.168.1.10 --edge left
```

输入通道默认 TCP 24803。等待配对成功后分别用 A 和 B 的实体鼠标越过各自配置的一侧，双向验证打字、滚轮和按钮。返回相邻边缘可还回控制。目标机的实体键鼠输入可抢回，用 `Ctrl+Alt+Shift+F12` 可结束本地增强输入会话。应额外检查按住修饰键时断网、锁屏、重连前恢复本地控制，以及高 DPI 或非矩形显示器布局。

## 当前能力和验收范围

| 功能 | 代码状态 | 验收 |
| --- | --- | --- |
| 经典模式 | 保留原有协议、界面入口和平台路径 | 上游 Windows x64 构建已完成，经典双机互通待验收 |
| 文件与目录 | GUI 和 CLI 的双端指纹认证、独立数据通道、分块哈希和取消 | Windows、macOS、Linux 文件管理器粘贴与 1 GiB 并发样例待验收 |
| 双向输入 | Windows 两台机器、左右边缘、双端主动发起、本地释放 | 真实设备、热键、物理输入过滤、各 DPI 布局与延迟待验收 |
| 文件拖放 | 本机传输窗口接收文件选择，传输后在接收端取回 | Finder、Explorer、XDND、Wayland 原生跨屏拖放尚未实现 |
| 后续 | macOS/Linux 双向输入、三机路由、断点续传、持久配对管理 | 尚未实现 |

Wayland 剪贴板能力取决于当前会话和合成器。文件窗口可以通过文件选择和接收目录操作。Windows 输入原型面向普通交互桌面。当前布局使用虚拟桌面的最左或最右边缘，未提供跨多个物理屏幕的任意拓扑。

## 可复现验证

- Windows 编译环境和经典构建命令见 [P0 基线](../P0_BASELINE.md)。构建增强版时使用独立的 `build/enhanced` 并打开 `BUILD_ENHANCED`。
- 单测覆盖帧版本和序号、清单边界与路径拒绝、租约竞争和过期 epoch。回环测试用临时文件验证双端指纹、分块传输和保存。执行 `ctest --test-dir build/enhanced/src/unittests -R Enhanced --output-on-failure`。
- 实机记录填写两端系统版本、构建 SHA、连接方向、布局与缩放、输入释放时间或文件 SHA-256、是否进入实际目标文件夹。
