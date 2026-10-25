# LAN3DReceiver

接收画面窗口按 F12 切换 3D / 2D 左眼显示，长按只切换一次，无需断开连接。2D 模式保留窗口或全屏状态，普通桌面不裁切；选择 3D 后仍须全屏且来源为 Leia SBS 才交织。模式选择在本次接收期间保留。

0.8.3 两端需一起更新。F12 的选择通过媒体协议 v6 反向通知发送端，2D 时在 GPU 上先裁出原始左眼，再按画质上限缩放、转换和编码；目标码率不变。左眼数据带独立标记，接收端不会重复裁切，也不会交织尚未切回 SBS 的单眼帧。切回 3D 从新关键帧恢复双眼，重连会重新发送当前选择。窗口预览仍可接收完整 SBS，只有用户选择 2D 才要求发送端裁切。

接收线程负责 TCP 与 H.264 / HEVC / AV1 解码，优先使用 NVDEC 硬解及 GPU 色彩转换，只保留最新完整画面。统一桌面客户端以 `--auto-display` 启动：默认是可拖动、缩放的小窗口，普通桌面直接显示，Leia SBS 显示左眼的 2D 图。点击全屏按钮、双击画面或按 F11 后，仅 Leia SBS 启用本机交织；普通桌面即使全屏也不会加载 Leia。退出全屏、或来源变为普通桌面，会立即撤销 weaver。Esc 在全屏时还原窗口；设计界面的窗口模式下不关闭接收窗口。

默认接收并播放随视频传来的 48 kHz 双声道 PCM。独立 WASAPI 播放线程仅允许音频推进到已成功显示的视频时间戳；上传纹理、解码成功、重复呈现旧画面都不能单独推进声音。窗口最小化、DXGI 遮挡或绘制失败会暂停声音。播放队列有界，视频跳到新时间戳时丢弃落后的声音，设备队列也在需要时清空。每次媒体重连使用新代号，旧帧、旧音频及迟到的呈现回调不能恢复旧连接的声音。`--no-audio` 可关闭播放；`--hidden` 诊断模式不授予音频播放进度。

接收页面和画面窗口提供实时播放音量（0–800%，100% 原始、0% 静音），全屏时可按 Ctrl＋↑／↓ 调节。音量只在本机 WASAPI 提交前处理，沿用同一播放队列和时间轴；峰值保护不增加延迟。GUI 在接收机保存 `ReceiverVolumePercent`，并用 `--volume-percent` 传入初始值；运行时通过父窗口与接收窗口的消息更新，无需重连。

3D 模式下，窗口线程按显示器刷新率持续交织，即使源端暂时没有新帧，也会重新读取接收机的本地眼位。网络不传源端眼位。无法加载本机 runtime 时显示错误、恢复 2D 窗口，并保持接收连接。诊断 `--preview` 默认显示完整的普通 2D SBS，F12 可切为左眼；始终不加载 LeiaSR。

```powershell
.\LAN3DReceiver.exe --list-monitors
.\LAN3DReceiver.exe --connect 192.168.1.20 --auto-display --peer-name '工作电脑'
.\LAN3DReceiver.exe --connect 192.168.1.20 --monitor 1 --fullscreen
.\LAN3DReceiver.exe --connect 192.168.1.20 --monitor 1 --runtime-dir 'F:\LeiaSRBridge\runtime'
.\LAN3DReceiver.exe --connect 127.0.0.1 --preview --frames 120
.\LAN3DReceiver.exe --connect 127.0.0.1 --preview --hidden --receive-frames 90
.\LAN3DReceiver.exe --preview --self-test --frames 90
```

显示屏编号从 0 开始。默认尝试按 Windows 显示器名称找到 Leia/Dimenco/M27E6V/27 3D，否则使用主屏，并打印完整屏幕列表。`--monitor` 可以明确指定。DPI 感知和实际客户区大小的交换链避免交织结果被窗口缩放。SBS 输入采用左眼在左、右眼在右、top-down BGRA8 SDR sRGB；SRV 和 RTV 均使用硬件 sRGB 转换。2D 画面保持宽高比。内容类型由协议 v4 的每帧标志携带，不根据画面长宽猜测。

默认 runtime 目录为 `C:\Program Files\LeiaSR\Platform\bin`。`--runtime-dir` 必须指向同一套 runtime 的完整 DLL 目录，不只是 DirectX 假 DLL，统一桌面客户端只使用本机保存的设置。DLL 搜索限制为所选目录和 Windows 系统目录，已加载 SimulatedReality/TitanLeiaRuntime 模块来源也会检查。客户端不静态链接 Leia 导入库；编译时需要原始 SDK 头文件以取得真实 C++ 布局。自动显示模式仅在全屏 SBS 时加载 runtime，失败会明确报错后还原 2D，不会把回退画面称作 3D。

优先使用 `CreateDX11Weaver` / `IDX11Weaver1`。若新接口缺失或返回失败，则回退到 `PredictingDX11Weaver` 导出，使用 SDK 的 `sizeof` / `alignof` 分配原位对象，支持当前 LeiaSRBridge 的 legacy DX11 实现。新接口的输入宽度按官方 DX11 示例传每眼宽；legacy 构造参数传 SBS 总宽。接收端只用 DX11；发送端捕获 DX11 或 DX12 与接收端渲染 API 无关。

`--frames N` 在已有输入后呈现 N 次退出（不是 N 个唯一网络帧）。`--receive-frames N` 在解码得到 N 个不同的源帧后退出；依据 TCP 顺序下的序号/时间戳变化去除连续重复帧，退出日志输出 `decoded_unique`、`last_sequence`、`uploaded`、`presented`。`--hidden` 允许与 `--preview` 或 `--auto-display` 搭配，用隐藏窗口做回环验证；隐藏的自动显示始终保持 2D，不会打开 Leia。最小化自动显示窗口时也会撤销交织，恢复全屏后才重新启用。`--timeout SECONDS` 默认 10 秒，无完整帧超时退出；断线或解码错误会打印错误并退出。`--self-test` 必须同时指定 `--preview`，使用隐藏窗口验证 SBS 左右顺序、sRGB 往返、输入纹理改变尺寸、窗口改变尺寸和正常退出；不加载 Leia runtime、不连接显示器服务。

设计接收窗口使用嵌入资源的 WebView2 透明叠层；真实视频仍由 D3D11 呈现。进入全屏 SBS 交织前隐藏整个叠层，保留物理像素尺寸。UI 预览通道每秒发布一次最长边 432 像素的真实缩略图，只写共享内存、不写磁盘。程序/窗口图标使用资源 101 的透明 ICO。开发构建由根目录 CMake 管理，新增 `lan3d_ui` 与 `lan3d_preview` 依赖。

统一进程入口调用 `lan3d::RunReceiver(argc, argv)`；旧 CLI 的 `wmain` 在 `ReceiverMain.cpp`。内部 `--stop-event` 供父进程请求正常退出，`--ready-event` 在窗口和 D3D 呈现准备好后通知父进程（不代表网络或 3D 已就绪），`--session-token` 接受 32 个十六进制字符并在 TCP 连接后执行会话握手。这些内部参数由本机客户端生成，不接受远端任意命令或路径。

未启用恢复的诊断 CLI 在完整视频包之间收到 TCP EOF 时正常结束并输出最终统计；显式 `--receive-frames` 目标尚未完成时仍以失败退出。包头或负载接收一半时断线属于截断错误，不能作为正常结束。`--frames` 只约束呈现次数，干净 EOF 后可继续呈现已收到的最后一帧直至达到次数。

GUI 的 `--auto-display` 且带 `--session-token` 会自动启用媒体恢复；诊断可显式使用 `--recover-stream`。半包超时或连接关闭后，旧 socket、半包和解码器一起丢弃，在同一会话端口重新认证、协商，从关键帧重新解码；保留最后完整画面直到新帧可用。每次 I/O 最多 250ms，从最后完整解码帧算起最多等待 6 秒，完全使用接收机单调时钟，不比较两机时间戳。主动停止会关闭当前 socket 并终止恢复。`VIEWER_STATE` 日志的 `recovering` 字段以及 `STREAM_RECOVERY` / `STREAM_RESUMED` 标明实际恢复过程。
