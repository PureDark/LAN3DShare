# LAN3DShare 0.7.7 验证记录

## 0.7.7 接收音频保护（2026-10-25）

用户反馈偶发爆音。未收到 Huangwei-Gaming 的接收日志或爆音录音，不能确认实机根因。代码检查发现旧增益在整个 PCM 块发现峰值后立即降低全块增益，且静音/100% 分支直接跳变；这些路径可能产生块边界不连续。新实现使用连续的 96 帧预看限幅、共享左右声道包络和 5 ms 实时音量渐变。保守检测安静、平稳背景中的一至两个异常采样，以两侧有效采样插值；检测阈值随所选放大量调整。不依据单纯响度静音持续瞬态，无法承诺识别所有原始录音或设备端噪声。

状态跨音频块保留，固定延迟 100 帧（48 kHz 下 2.083 ms），输出帧数、时间戳、采样率和视频许可不变。开始/恢复时 2 ms 淡入；重连清空延迟音频。100% 不再为完全绕过保护的 bit-exact 模式；干净、未触发限幅的信号在固定延迟和初始淡入后保持原采样。持续静音在实时音量设为 0 后最多 5 ms 内到达。输出缓冲瞬间为零不直接复位：必须超出已提交音频时长和一个完整端点缓冲周期，避免把 WASAPI 正常交接误判为断流。真正丢失的声音无法重建，已经交给设备的声音也不能追溯修复。端点缓冲的含义依据 [Microsoft GetCurrentPadding 文档](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-getcurrentpadding)；持续耗尽判定是本程序的保守恢复策略，并非设备报告的硬件 underrun 计数。

新增接收日志 `AUDIO_PROTECTION`（每 5 秒）及停止时 `AUDIO_SYNC` 字段：`repaired_samples` 为插值修复的声道采样数，`limited_frames` 为限幅包络生效帧数（不等于削波次数），`underruns` 为根据提交时长判断的持续缓冲耗尽次数，`restarts`/`protection_restarts` 为保护状态重启次数，`delay_frames=100`。不录制或保存用户音频。

Release 编译通过；AudioGain、AudioScheduler、AudioMeter、AudioEndpoint 四项通过，证据 `build-opt/test-results/audio-protection.xml`。覆盖跨包/两采样尖峰、放大后的低幅尖峰、40 Hz–23 kHz 正常测试音、持续强瞬态、1/37/4800 帧分包输出一致、峰值边界、立体声比例、实时 0/50/100/600/800% 变化、音频时间/长度与断流清空。真实 WASAPI 使用静音验证，画面冻结后不再提交、旧代音频被丢弃、重连恢复且保留 600% 音量；不是远端扬声器听感验收。

同时复查 0.7.6 正式发送日志 `20261024T172900Z-send-{C253EC84-2932-46F7-A21B-8D77C9BDFD63}.log` 的 554 个统计窗口：排除启动窗口后平均 59.776 FPS、source 最大 7.4869 ms，没有超过 20 ms 的 source 窗口；209 次后台扫描最大 277.291 ms，未重新阻塞采集。仍有单次网络发送 213.426 ms 尖峰，不能宣称所有网络卡顿已消失。摘要见 `build-opt/test-results/audio-protection-sender-observation.json`。按用户确认，保留既有后台进程识别和局域网发现逻辑。

## 0.7.6 自动来源后台扫描（2026-10-25）

正式发送日志中每隔约 2 秒出现 source 阶段约 170–220 ms 的尖峰；原代码在自动来源模式的发送循环内按相同周期执行 ListProcesses，逐个进程枚举模块。该扫描现已移至单独的长期后台线程。发送循环仅用 try_lock 投递请求和取结果，不等待枚举；仅保留最新待处理请求，过期扫描结果不发布。结果使用前重新核对前台窗口、所选屏幕与存活的进程句柄，并以创建时间区分 PID 重用。首次接入/停止采集仍沿用原有逻辑，此修改不声称消除来源切换时的全部等待。

Release 编译通过；AsyncProcessScan / SourceProcess 两项 CTest 通过，证据为 `build-opt/test-results/async-process-scan.xml`。覆盖阻塞扫描期间非阻塞轮询、待处理请求合并、旧结果丢弃、异常后恢复、真实进程枚举和进程身份校验。检查期间正式客户端仍在发送，未打断连接、未运行额外 GPU 压测、未替换运行中的程序。因此尚无新版真实局域网帧率或接收端卡顿验收数据；先前观察到的 TCP 发送停顿与跨卡复制波动仍需另行分析。

只需更新发送端并重新打开客户端；不修改媒体协议、接收端或游戏内采集 DLL。

日期：2026-10-21–22。Windows x64 Release，Visual Studio 2026 / MSVC 14.51.36231，LeiaSR SDK 1.36.2 公开头文件。

## 0.7.5 GPU 图像管线与双 GPU（2026-10-25）

本轮没有重启、注入或操作真实游戏。测试结束时游戏已退出；以下是实际 GPU 的桌面回环和合成 SBS 验证，不等同于 Huangwei-Gaming 的局域网、LeiaSR 显示器观感或游戏负载验收。

### 原因与实现

已记录的普通桌面路径先回读整帧 BGRA，再使用 CPU 双线性缩放，最后上传给 NVENC。4K 缩到 1440p 的一次基线中，缩放平均 44.22 ms、编码入口平均 11.09 ms、发送 13.35 FPS；因此“NVENC 已开启”不代表整条图像管线都在 GPU。另一轮 4K 基线采集约 19.84 ms、编码入口约 16.97 ms、约 23.43 FPS。两轮 GPU 负载不同，不应视为严格同负载 A/B。

现在桌面使用保留所有权的共享 D3D11 纹理，缩放/色彩转换交给 DirectX Video Processor，接 NVENC。SBS 继续使用交织前共享纹理。选择其他 GPU 时，源端 D3D12 Copy 队列复制至跨适配器共享堆，目标端 Copy 队列复制至本地纹理，D3D11 GPU fence 等待后进行转换/编码；CPU 不搬运整帧像素，也不在 Copy 与 VP 之间等待 fence。NVENC 仍逐帧同步取出码流；尚未实现多帧异步 NVENC 环形队列，不宣称完全重叠相邻帧的复制与编码。

接收端 NVDEC 的 NV12 通过 CUDA/D3D11 互操作进入 GPU buffer，DirectX compute 转为 RGBA 共享纹理，显示器所在 GPU 完成显示/LeiaSR 交织。纹理池持有读者引用和 GPU 完成查询，防止显示未完成时被下一帧覆盖。没有整帧下载再上传。界面预览仅每秒读回 GPU 缩小后的不超过 432×432 缩略图；CPU 仍负责控制、码流和音频。

设置新增“发送处理 GPU”，只列出匹配实体 CUDA LUID 的 NVIDIA 显卡，排除 Windows 枚举出的同名虚拟适配器。名称后显示 PCI 地址，选择以 PCI 地址保存。当前实体设备为 GPU 0 / PCI 0000:01:00.0 和 GPU 1 / PCI 0000:03:00.0，均为 RTX 5090。PCI 地址不是主板丝印插槽编号。

### 实测与边界

- 桌面 3840×2160、60 FPS、AV1、400 Mbps、本机回环，排除首个启动统计窗口：同卡发送 **60.00 FPS**、解码 **59.99 FPS**；跨卡发送 **59.18 FPS**、解码 **59.28 FPS**。发送的编码入口耗时分别约 **4.11 ms / 8.64 ms**，跨卡数值包含复制依赖等待。接收解码与 GPU 转换约 **3.61–3.66 ms**，显示端 GPU 交接的 CPU 提交约 **0.095 ms**。证据：`build-opt/test-results/desktop-perf/comparison.json` 及 `release-same-4k` / `release-cross-4k` 原始日志。
- 跨卡 Copy 队列硬件时间戳：源侧约 **1.16 ms**，目标侧约 **2.27 ms**（33 MB BGRA 帧）。D3D11 图形时间戳不能代表独立视频处理引擎的 VP 耗时，因此未把该测法的异常近零读数作为转换耗时，正式日志也已移除该指标。NVENC API 的等待时间包含资源依赖，不能直接当作纯硬件编码时间。
- 合成 SBS：GPU 0 的 7680×2160 双眼纹理跨卡进入 GPU 1，编码总画面 **3840×2160 / 60 FPS / AV1 / 200 Mbps**，稳态发送 **59.95 FPS**。两次连接分别解码 240 / 60 个不同序号帧；双眼比例、内容标记、码流与 NVDEC 核对通过。证据：`build-opt/test-results/cross-gpu-end-to-end/result.json`、`sender.stdout.log`。
- AV1/HEVC 同卡与跨卡 GPU 像素测试通过：BGRA/RGBA、缩放、左右眼、元数据、持有多个解码纹理时不覆盖、销毁解码器后纹理仍可读。系统音频端到端、断线恢复、窗口/全屏自动交织及 UI smoke 的选定 7 项 CTest 通过；精确帧间隔调度后又复核音频及恢复 2 项。UI 检查：GPU 选择 6、接收音量 6、电平表 10、来源状态 16。
- `receive_wait_ms` 包含等待发送方下一帧，不是 LAN 单向延迟；`draw_present_ms` 含 Present/同步等待，不是纯绘制 GPU 时长。当前远程工具只列出 PureDark-Gaming，未自动更新或测量 Huangwei-Gaming。没有本轮真实游戏 FPS 对照，不能保证跨卡完全不影响游戏。两端需要更新到本版才能同时获得 GPU 接收路径。
- `LAN3DCapture.dll` 保持原二进制 SHA256 `9611CEBD5C003A2F26C8CBE830E8DF1E89CDA0C9E536CAF0C17ADD047E8D61EA`，无需因为本版发送/接收管线更新替换游戏内采集 DLL。

## 0.7.4 两端实时音量表与异常采集时钟

用户反馈接收无声。2026-10-24 最新正式发送日志显示 captured_frames=4682400、discarded_frames=4682400、AUDIO_SENT packets=0：采集运行但没有发出任何音频。修改前真实端点诊断复现 raw QPC=434992064897833、本机 QPC=5495335260514，相差 429496729637319 个 100 ns 单位（约 497 天），且端点未设置 TIMESTAMP_ERROR。这导致音频一直处于视频的未来、在有界队列中丢弃。证据 `build-opt/test-results/audio-meter/before.json`；尚不能判断声卡、驱动或 Windows 的具体内部原因。

微软 [IAudioCaptureClient::GetBuffer 文档](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer) 规定该字段已换算为 100 ns QPC。此次保留正常时间戳；偏差超过 2 秒时将设备采样位置锚定到本机 QPC，后续按采样数推进，避免每包到达抖动改变音频时间。设备回退、discontinuity 或异常漂移重新锚定并清除旧时间轴。退路初始时间为读取时刻减一个采集包时长，是估计值，不代表物理声画同步验收。

电平分别取自系统采集 PCM、成功发送的 PCM、接收播放队列收到的 PCM，以及增益后成功提交 WASAPI 的 PCM。按 50 ms 分桶保存最多 200 ms 的峰值/RMS，未保存或录制音频。接收端重连/暂停清空相应电平；发布进程的状态超过 600 ms 无效，网页没有更新超过 900 ms 清空显示。独立观察线程每 100 ms 发布小型共享内存，IPC 不在实时音频线程执行、不更改网络协议。

本轮七项 CTest 通过：AudioMeter、AudioGain、AudioScheduler、AudioEndpoint、AudioSystemEndToEnd、StreamRecovery、ViewerUiSmoke。AudioMeter 覆盖 S16 负满幅、RMS、静音/无采样区分、过期、异常时钟、采样位置回退、正常时钟恢复，以及共享状态发布/销毁。真实端点测试新增“必须能在视频时间轴上取出非空音频”的断言；本轮 clock_corrected=1、clock_corrections=1，时间差恢复到约 -16 ms，声音冻结与旧代号拒绝检查通过。生产端回环成功发送并接收 20 包、34466 个采样帧，隐藏接收端没有播放出声。

离线 Edge 音量表十项检查及已有实时滑条六组检查通过，截图覆盖两端 780×540 和画面窗口 480×294，检查静音/暂停/无设备/无数据及全屏交织时隐藏叠层。证据为 `build-opt/test-results/audio-meter/tests.xml`、`ui.json`、`system-result.json`、`sender.png`、`receiver.png`。这些结果不包含 Huangwei-Gaming 实际出声或物理声画偏差验收。采集 DLL 未改动，无需因本轮更新重启游戏。

## 0.7.3 接收端实时播放音量

按用户最终要求，移除发送端共享音量，发送 PCM 不再放大。接收端主界面与独立画面窗口提供 0–800% 的实时播放音量；默认 100%，0% 静音，全屏 Ctrl＋↑／↓ 每次调整 25 个百分点。GUI 保存 ReceiverVolumePercent，旧 AudioGainPercent 删除且不迁移到接收设置。更新音量只修改播放线程锁内的增益值，不重置队列或重连代号；设备提交前处理 PCM，保留峰值保护与时间戳。媒体协议仍为 v5。

本轮重新构建 GUI、发送/接收诊断入口及相关音频测试；采集 DLL SHA256 仍为 `9611CEBD5C003A2F26C8CBE830E8DF1E89CDA0C9E536CAF0C17ADD047E8D61EA`。

- `AudioGain`、`AudioScheduler`、`AudioEndpoint`、`AudioSystemEndToEnd`、`StreamRecovery`、`ViewerUiSmoke` 六项全部通过：真实 WASAPI 静音播放过程中依次切换 500/0/100/800%，每次下一批提交应用新值、代号和 reset 计数不变；视频冻结期间修改音量不会推进声音；重连后保持 600%。原始 100% PCM、增益收敛、峰值及时间戳检查通过。
- 真实 WebView2 控件 input 经原生桥生效为 500%；非法 -1/801/小数被拒绝；全屏快捷键生效为 525%；原有全屏、还原及透明 D3D 呈现检查通过。此项是本地合成图案，没有连接游戏或 LeiaSR。
- 离线 Edge 六组检查通过：主界面即时音量消息、边界/静音、发送界面禁用、状态更新、480×294 最小窗口布局、滑条不触发拖窗、全屏 SBS 隐藏叠层时的键盘操作。另有 16 项既有 UI 来源状态检查通过。

证据：`build-opt/test-results/receiver-volume/tests.xml`、`ui.json`、`receiving.png`、`viewer-minimum.png`。真实端点测试仅播放静音；本机回环验证收发，尚未测量远端扬声器听感与物理声画偏差。历史汇总测试按名称保留最近结果，并非本轮全量重跑。

## 0.7.2 程序启动源修复

真实故障日志 `20261021T153714Z-send-{4CEE21BF-E4E7-4BB2-A8EF-BE5A41806C07}.log` 位于 `%LOCALAPPDATA%\LAN3DShare\logs`。GUI 选择“打开程序…”后，最初启动 PID 5332；Steam 随后退出该实例，并启动实际游戏 PID 38740。旧发送端一直绑定 5332，日志持续为 `SOURCE_STATE waiting-input pid=5332`，即使前台已变为 38740 也未跟随。该会话实际输出 `SOURCE_KIND Desktop` 和 `ENCODER NVENC AV1 Desktop 3840x2160 target=60fps/600Mbps`，不能视为已经传输 LeiaSR SBS；界面的等待提示反映了发送端状态。

0.7.2 按所选 EXE 的完整规范路径查找已运行实例，唯一匹配时复用，没有实例时才启动。初始或后续实例退出后，会查找同路径的唯一替代实例；不要求替代实例是初始进程的直接子进程。持有进程句柄并校验 PID、创建时间和规范路径，避免把已退出或 PID 已复用的进程当成原来源。同名但不同路径的进程不会匹配；多个同路径实例保持歧义，不自动选择或注入，用户可选择具体运行进程。

初次真实实验的发送日志 `build-opt/test-results/program-follow-072/launch-sender.log` 已记录初始 PID 52752 退出，以及 `SOURCE_PROCESS followed pid=54896 same_executable=1`，证明该轮按路径找到了实际游戏实例。但接入过早，游戏随后黑屏，该轮发送 0 帧；停止记录为 `Capture stop NOT CONFIRMED`，不能作为采集、显示或停止恢复通过的证据。随后修复 `LoadLibrary` 同步扫描与采集锁之间的锁顺序冲突：纯 CPU 回归中，旧 0.7.1 DLL 明确复现嵌套加载死锁，新 DLL 通过。该对照证明了这条锁冲突路径，仍不是原游戏黑屏的线程栈根因证明。

当前 Program 模式不再预注入启动阶段的进程：先启动或按路径跟随，等 `SimulatedRealityDirectX.dll` 已加载，且可见、无 owner 的主窗口在 100 ms 内响应 `WM_NULL`。按 2 秒间隔检查，连续两次就绪后才热附加，未就绪时输出 `SOURCE_STATE waiting-runtime`。这是接入前的就绪检查，实际 SBS 输出仍以采集日志和视频帧为准。

来源切换时关闭旧 CPU/GPU 总线、清空旧帧和序号缓存，重建编码器并从新关键帧恢复。尚无唯一可接入实例时输出 `SOURCE_STATE waiting-process`；接入失败输出 `SOURCE_ATTACH_FAILED` 及错误原因。UI 分别提示“源程序正在切换进程，等待接入实际游戏…”和“正在等待源程序完成初始化并启用 LeiaSR…”，状态解析兼容没有附加字段的整行及空白分隔；Program 已连接时按实际画面显示普通/3D 状态，不再一直显示“连接后启动”。

| 本次检查 | 结果与边界 | 本地证据 |
| --- | --- | --- |
| CaptureLoader | 新 DLL 在 0.153316 秒内通过 `LoadLibraryW/A/ExW/ExA` 嵌套加载回归，不创建 GPU 对象；旧 DLL 对照退出码 1，输出 `Nested load deadlocked with capture module scanning`。 | `build-opt/test-results/program-follow-072/loader-final.xml`、`old-loader-negative-final.log` |
| Injector 与 24 项 Capture hook | 按每项最新结果合并后全部通过。`capture-hooks.xml` 保留首轮 7 项失败；`capture-ready.xml` 重跑 8 项普通 direct/forward 路径全部通过，原 16 项 late-attach 和 Injector 已通过且未重跑。不能将首轮报告单独称为全通过。 | `build-opt/test-results/program-follow-072/capture-hooks.xml`、`capture-ready.xml` |
| SourceProcess | CPU fixture 通过：复用唯一运行实例、初始进程退出后的同路径替代实例、持有句柄与创建时间校验、路径大小写/`.`/扩展写法、不同路径同名隔离、多实例歧义和退出实例拒绝。使用自建辅助进程，没有验证真实 Steam、采集或 GPU 管线。 | `build-capture-gpu/test-results/source-process.xml` |
| CPU/GPU FrameBus 关闭重开 | 两项通过：关闭旧代后重置状态，跨进程帧和 GPU 租约不沿用旧代缓存。 | `build-capture-gpu/test-results/bus-close.xml` |
| UI 源码状态检查 | 16 项通过：等待进程→等待 runtime→等待输入→GPU SBS、`waiting-runtime` 中文提示、空白与行尾、非法后缀拒绝、等待提示清除、Program/Process 预览标签。`tests/UiSourceStateTests.js` 读取实际源码，在 Node 的 ECMAScript 正则和隔离 VM 中执行页面函数；不是原生 C++ 正则或 WebView2 运行验证。 | `build-opt/test-results/ui-source-state/result.json` |

CaptureLoader 旧版对照 DLL 的 SHA256 为 `3D8790E7160DEAFCC3EE16EECEE4368F7A306E6196B60775A1D817EF983B08AE`；新版通过 DLL 为 `9611CEBD5C003A2F26C8CBE830E8DF1E89CDA0C9E536CAF0C17ADD047E8D61EA`。

修复后的真实启动由 PID 33156 切换到 PID 1264，日志依次记录 `waiting-runtime`、`SOURCE_PROCESS followed`、已有 modern DX12 weaver 热附加、`capturing 7680x2160 GPU SBS` 和 `SOURCE_KIND LeiaSbs`。使用生产控制协议连接 `HUANGWEI-GAMING`（192.168.3.197），远端 GUI 启动接收进程；两轮控制会话各持续 25 秒并收到 25 次心跳回复。第二轮仍使用 `--launch`，日志确认复用同一 PID 1264，全程没有再次重启游戏。

| 实际 LAN 会话 | 目标视频码率 | 主要 PROFILE_SUMMARY 区间 | 该区间发送帧数 | 发送 FPS | 系统音频发送采样帧 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Steam 进程切换后首次分享 | 600 Mbps | 24.8050 s | 1320 | 53.2151 | 1,182,808 |
| 复用同一游戏实例再次分享 | 200 Mbps | 23.9265 s | 1419 | 59.3067 | 1,141,651 |

两轮均为 GPU 源 7680×2160 → 总 SBS 3840×2160、NVENC AV1 自动 split2、P5/AQ。表中码率是配置目标，不是实测网络负载；FPS 来自发送端完整连接计时，未排除预热，不是接收端显示速率。第二轮先前还有一个 0.198421 秒、1 帧的短连接，后续主要区间为表中 1419 帧，进程结束总计 1420 帧。两轮日志均确认 `Capture stopped in PID 1264; the source application remains running`，系统回环音频已发送，`timestamp_errors=0`。音频采集/发送计数不能证明远端实际出声及同步。

上述证据位于 `build-opt/test-results/program-follow-072/fixed-launch-sender.log`、`reuse-200-sender.log`、`lan-control.log` 和 `lan-control-200.log`。`fixed-live-screen.png` 仅记录测试后瞬时 231 FPS，没有同条件开始前/期间截图，不能用于本轮游戏掉帧对照。

**0.7.2 用户反馈：**原定 120 秒的观察会话提前断开，不能记为完整通过。随后按用户要求打开正式 GUI，持续分享并等待反馈；用户明确确认画面与 3D 正常工作，同时反馈远端有声音但音量很小。这是该双机、游戏与显示器组合的功能确认，不是接收端 FPS、物理声画偏差或长期稳定 4K60 的量化验收。以下保留的 0.7.1 及更早性能、硬件和回归结果均为历史证据。

## 0.7.2 共享音量尝试（历史，已由 0.7.3 接收端方案替代）

用户本机 Windows 音量为 30%，接收机即使调到 100% 仍偏小。只读检查确认本机端点 scalar 为 0.300000012、端点报告约 −17.04 dB，相关应用会话均为 100%。同时采集默认端点回环和 process-loopback 的 5 秒对照，整体 RMS 仅相差 0.00219 dB；这项结果按总能量统计，没有逐采样对齐，不能说明换用 process-loopback 就能恢复音量。证据：`build-opt/test-results/audio-volume/before.log`。

另一轮以 997 Hz、PCM 峰值 128 的低幅参考信号验证，进程回环实测峰值为 29，稳定区段 RMS 约为输入的 0.212 倍（约 −13.5 dB）。全段 RMS 包含 200 ms 采集尾部，与稳定区段统计口径不同。测试未修改系统或会话音量；结果确认该采集路径上的参考音频明显衰减，但尚不能把全部衰减归因于 30% 主音量，或判定具体声卡、驱动、APO 根因。证据：`build-opt/test-results/audio-volume/reference.log`。

当时保留现有系统混音采集，增加发送端独立共享音量：0–800%，默认 100% 原始幅度、0% 静音。GUI 设置写入 `AudioGainPercent`，下一次分享经 `--audio-gain-percent` 传给发送端。增益仅处理即将发送的 PCM，包含峰值保护，不增加缓冲帧，不改动采样时间戳、视频许可、断线代号或接收端调度。媒体协议仍为 v5，同协议旧接收端无需为此升级。已更新本机 `F:\LAN3DShare\LAN3DShare.exe` 并重开，配置 500%；Windows 音量保持 30%。更新前用户的分享与游戏进程已经退出，本次没有启动或终止游戏。最终远端听感待用户确认。

真实系统声音同时走 100% / 600% 两路生产采集，预热 1.5 秒后测量 5 秒：两路均取得 240,178 个采样帧、888 个音频块，起止时间戳相同，时间戳错误与 discontinuity 均为 0。RMS 从 0.0179166 提升至 0.0971415，即约 5.42 倍 / +14.68 dB；较大的瞬时峰值触发保护，最大输出为 32000/32768，故整体增益小于 6 倍。没有渲染输出、修改系统音量或新增缓冲。证据 `build-opt/test-results/audio-volume/live-gain.log`，这是 PCM 电平验证，不是远端扬声器响度或音质验收。

| 本次音频检查 | 结果与边界 | 本地证据 |
| --- | --- | --- |
| 共享音量 GUI | 离线 Edge 的 10 项交互检查通过：100% 默认、草稿跨状态刷新保留、350% 保存与恢复、保存期间禁用及失败恢复、0/800% 边界、780×540 和 432×320 弹窗滚动、无页面异常。消息桥由测试模拟，没有操作正式客户端或用户配置。 | `build-opt/test-results/audio-gain-ui/result.json` |
| AudioGain | CPU 测试通过：所有 S16 值在 100% 下逐位不变、静音/衰减、6 倍低幅信号收敛、强瞬态限幅、立体声比例、静音期间恢复、块长度和时间戳保持。 | `build-opt/test-results/audio-volume/gain.xml` |
| 配置增益后端到端 | 600% 参数经生产 Sender 生效，真实系统混音采集并发送 34,438 个采样帧，隐藏接收端全部接收；隐藏播放为 0，时间戳错误为 0。 | `build-opt/test-results/audio-volume/system-gain.xml`（该轮系统结果已由 0.7.3 重跑覆盖） |
| 音频与恢复集成 | `AudioScheduler`、`AudioEndpoint`、`AudioSystemEndToEnd`、`StreamRecovery` 四项均通过，覆盖现有时间轴、真实声卡初始化与采集、系统音频收发和断线恢复。端点/集成测试使用静音播放或隐藏接收窗口，不代表远端扬声器听感及物理声画同步。 | `build-opt/test-results/audio-volume/integration.xml` |

以上为旧发送端增益的历史证据，不能当作 0.7.3 接收端音量或远端听感验收。

## 0.7.1 真实游戏性能验证（历史）

按用户授权重新启动《如龙7外传》，新进程 PID 43076、启动时间 2026-10-21 22:42:45 +08:00。用户将输出切回 LeiaSR 后，在同一个进程中反复开始/停止 GPU 采集，没有再次重启游戏，也没有修改 Mod。驻留采集 DLL SHA256 为 `3D8790E7160DEAFCC3EE16EECEE4368F7A306E6196B60775A1D817EF983B08AE`。实际源为 7680×2160，NVENC 输出总 SBS 3840×2160，每眼 1920×2160，原始单眼比例通过协议 v5 保留。

| 设置 | 稳态区间 | 新帧 FPS | 实际视频 Mbps | 帧间隔 P95 | 平均源时间戳至压缩包到达 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 采集 60 / 单引擎，第一轮 | 10.01 s | 58.57 | 192.34 | 28.77 ms | 34.03 ms |
| 采集 72 / 单引擎，短测 | 10.01 s | 60.06 | 200.10 | 25.43 ms | 31.15 ms |
| 采集 72 / 单引擎，长测 | 60.02 s | 59.38 | 197.54 | 29.50 ms | 35.68 ms |
| 采集 72 / 双引擎，长测 | 60.02 s | 59.90 | 198.89 | 27.93 ms | 31.85 ms |

以上均为真实游戏 → GPU 共享纹理 → GPU 缩放/NV12 → NVENC AV1 → 本机 TCP 压缩包接收器，P5、AQ、200 Mbps，预热 2 秒后统计；接收器不创建 NVDEC、不渲染、关闭音频。双引擎长测游戏 FPS 截图为 236 → 229/231 → 238，下降约 2–4%，不是零开销承诺；全局 FPS 叠层采样也不等于逐帧 GPU 时间测量。60 秒长测没有严格达到至少 60 FPS，且 P95 帧间隔高于 16.67 ms，仍存在抖动。不能把短测 60.06 当作长期每帧达标。

细分分析：原 GPU 路径 FrameBus 元数据读取约 0.002 ms、纹理映射约 0.004 ms、CPU Scale 为 0；`EncodePicture` 内等待约 8.4 ms，单引擎码流完成约 6 ms，双引擎约 3 ms。驱动提交等待可能包含前置 GPU 转换及编码计算，现有计时不能单独归因于 AQ。两种模式都保留 P5/AQ/码率；分片编码的真实游戏观感尚未做人眼对照验收。0.7.1 将 GPU 采集余量和符合条件的双引擎编码自动启用，其他设备使用原模式。

同机完整 NVDEC/渲染回环第一轮平均约 32.77 FPS；发送编码均值升至 29.22 ms，说明本机接收工作与满载游戏、编码同时竞争 GPU。该结果是真实限制，但不能替代空闲远程电脑的表现。独立 GPU 源截图的左右眼人物区域相邻变化率约 63–66%，GDI 显示人物区域也变化，排除了旧帧冻结。原始截图仅保留本地，不进入发布包。早期 GDI 全桌面范围受到 DPI 逻辑尺寸影响，分析只使用已确认可见的人物区域；之后截图工具使用实际像素 DPI 上下文。

完整结果含程序哈希、进程身份、退出码、停止确认、逐秒统计和测量范围，见 `validation/live-gpu-performance.json`、`live-gpu-pixels.json`、`live-gpu-sender.log`、`live-gpu-wire.log`。wire 日志 `missing_source_sequences` 来自高帧率游戏 weave token，包含主动限频跳过的画面，不能当成网络丢包数。此前 LAN 带宽证据仍有效作为历史测速，但这轮性能测试的 TCP 对端均为 127.0.0.1，未覆盖 Huangwei-Gaming 的显示或声音；后续双机功能反馈见上方 0.7.2 记录。

0.7.1 最终自动选择专项另见 `validation/nvenc-auto-split.json` 和对应日志：真实 RTX 5090 查询到 3 个编码引擎；默认 AV1/HEVC 4K 均选 split2，1080p 保持 AUTO，显式单引擎对照保持 AUTO。两轮均通过实际 GPUFrameBus 租约、元数据、RGBA/BGRA 左右眼颜色、关键帧 NVDEC 解码和 GPU 缩略图检查。自动 split 初始化失败分支会先销毁失败 session，重新创建后用原 P5/AQ/CBR 参数加 AUTO 重试；该失败分支只做静态审查，未人为制造驱动故障。长测使用诊断开关选择了相同 split2 设置，最终自动选择由该专项确认。用户表示当前效果已足够后，停止继续增加测试。随包 60 项 CTest 记录保留既有按名称合并结果，并未在此阶段全量重跑；本次最新专项以单独 JSON/日志为准。

## 先前定位与回归记录

本次性能定位（旧版 0.6.1、真实游戏 PID 38040）：20 秒回环会话平均发送 13.50 FPS，实际编码 3840×1080。CPU Scale 平均 51.24 ms，FrameBus 读取平均 10.10 ms，NVENC 平均 7.37 ms，TCP send 平均 0.406 ms。游戏右上角 FPS 截图为开始前 250、采集中 26/31、停止后 250。证据 `build/test-results/perf-baseline`。网络发送不是这次瓶颈。

0.7.0 改为 DX11/DX12 共享 GPU 纹理、上游限频、资源复用、GPU 缩放/NV12 转换和 NVENC。资源与 IPC 测试覆盖未 Reset 列表重执行、GPU 未完成、消费者未释放、槽耗尽、丢旧帧和断线后不抢占活跃资源；错误时无法确认 GPU 读结束的槽保持隔离。协议 v5 携带原始单眼比例，接收窗口不会把 1920×2160 半宽眼图显示成窄图。全屏 weaver 仍接受实际纹理尺寸。

RTX 5090 合成共享输入编码专项：7680×2160 → 3840×2160、200 Mbps、P5，AV1 平均 8.45 ms / P95 13.10 ms（118.28 FPS）；HEVC 10.04 ms / P95 15.97 ms（99.57 FPS）。测试时游戏运行，但未同步记录 GPU 占用；合成块图不等同于真实游戏复杂度，也不能与不同图案的 CPU 基准直接计算加速比。证据 `build-media-gpu/test-results/nvenc-gpu.xml`。后续真实游戏性能记录见上方 0.7.1 和 0.7.2 段落。

生产 Sender/Receiver 的合成 GPU 回环测试：源 7680×2160、编码总 3840×2160、AV1，初次收到 241 个新帧、重连后 61 个新帧，独立线协议检查器验证 v5、首关键帧和单眼比例。新截止时间逻辑版本稳态发送约 57.02 FPS；先前一次并行测试约 59.13 FPS，不能把不同负载结果当严格速度对照。接收机仍有 NVDEC 后 CPU 读回和上传，同机回环让这些工作与游戏及发送编码争抢同一 GPU。这项结果尚不满足严格至少 60 FPS。

仅接收压缩数据、不在本机解码的独立测试：预热 2 秒后，10.0085 秒收到 597 帧（59.6492 FPS），已发布的源帧序号缺失为 0；生产器全程因非阻塞发布碰撞跳过 4 帧。编码平均 8.10 ms，源时间戳到压缩包收到平均 15.86 ms。CPU Scale 为 0，元数据读取约 0.001 ms。实际合成图案码率仅约 4.07 Mbps，200 Mbps 是配置目标，不能据此证明复杂游戏画面、934 Mbps LAN 链路或真实接收显示验收。两项证据分别为 `validation/gpu-end-to-end.json`、`validation/gpu-sender-wire-sink.json`。

本次相关集成回归 24 项的首次执行中，StreamRecovery 测试代理仍接受旧 v4，现同步为 v5；AutoHardwareEndToEnd 的 CPU 4K 图案在游戏运行时约 19–20 FPS，原 8 秒限制无法收齐 150 帧。只延长该功能测试的时间预算并保留两轮 150 新帧断言，GPU 吞吐由独立专项测量。修正后两项均通过；其余 22 项首轮通过。

0.6.1 已修复热附加时在宿主中创建探针交换链造成的游戏场景冻结，并以公开 PrivateData token 验证 DX12 包装命令列表与实际提交对象的身份。探针只在发送端进程运行，注入后的游戏中不再创建探针图形对象。复跑采集、注入、GUI 和桌面收发检查，保留未修改音频/媒体/发现模块及设计界面的既有通过记录。音频时序规则保持不变；媒体协议 v5 增加原始显示比例，两端必须一起升级。`validation/ctest.xml` 按测试名称保留最新结果，`validation/build-info.json` 记录来源报告哈希、源文件哈希及汇总方式。测试总数和结果以这两个随包记录为准。

真实《如龙7外传》验证：重启清除 0.6.0 触发的 Mod 队列错误后，对同一 PID 38040 先运行 8 秒仅观察钩子的阶段，再开启两次独立分享，每轮均用 NVENC AV1 / NVDEC 解码 30 个不同源帧，全程没有再次重启游戏。两轮实际源 SBS 均为 7680×2160，编码按比例缩为 3840×1080。观察阶段和两轮采集各保存 3 次 GDI 最终桌面截图，两轮另保存各 3 个 weave 前 SBS。排除菜单、FPS 和调试文字的人物区域，间隔约 4 秒的 SBS 有 56.1% / 58.0% 像素变化，最终显示也在变化；新游戏会话日志中队列切换/NR stopped/设备丢失匹配数为 0。两轮结束均显式停止采集，游戏仍运行。记录见 `validation/live-hotattach.json` 和 `validation/live-pixels.json`，本地原始图像不打进用户发布包。

此前 0.6.1 实机收发日志约 11–14 个新解码帧/秒，截图中开启采集时游戏帧率约 38，停止后日志恢复约 235–237 帧/秒。这是热附加与场景持续更新的功能验证，**尚未达到 4K60，采集开销仍需优化**。最终显示截图通过 GDI 独立采样，不冒充 Present 原始纹理或与 SBS 同一帧。此次 Desktop Duplication 截图为黑帧而 GDI 正常，不能用黑帧判断游戏冻结；该桌面采集兼容性问题未在本次修复中处理。

新增共享音量前，离线 Edge 对照原稿九种状态：780×540 主界面、720×440 接收窗口，deviceScaleFactor=1；像素比较颜色阈值 0.1，排除 12px 原生窗口外框与用户明确要求更改的码率滑条/数值区域，九项均为 0 差异。结果见 `validation/web-ui.json`。这项比较使用固定设计 fixture，不把演示设备/IP/画面当成实际发现或传输证据，也不证明本次新增音量控件与原稿像素相同。真实 WebView2 宿主另验证消息、超长消息拒绝、外部导航拒绝、关闭时异步回调及 PNG 截图；程序图标的九个尺寸均检查透明、半透明和不透明像素。

| 检查 | 覆盖与结果 |
| --- | --- |
| PeerService | 三个本机服务实例：发现、状态更新、过期、接受/拒绝、忙碌、同时发起、token 重用、取消待决请求、心跳超时、畸形/不完整消息、有界停止 |
| WorkerProcess | 同一个 GUI 子系统 EXE 的后台工作模式；中文、引号和反斜杠参数；独立就绪/停止事件；标准输出/错误日志；无控制台；非阻塞停止和不强杀源程序 |
| DesktopCapture | 真实当前 Windows 会话只读采集 7680×2160 BGRA8，验证 Desktop 标志、序号、时间戳及主动刷新后新帧；不保存桌面截图 |
| SessionTransport | 匹配、错误、缺失 token；同一连接 Desktop → LeiaSbs → Desktop，帧总线、H.264 编解码和 TCP v5 保留内容类型及 PCM |
| ReceiverAutoDisplay | 普通帧在窗口/全屏均不加载 Leia；SBS 窗口左眼像素验证；runtime 缺失恢复 2D；模拟 modern/legacy runtime 的全屏交织、还原、切 Desktop 撤销、回 SBS 重建及退出释放 |
| GuiSmoke | 实际主界面创建、布局和截图检查；修复高 DPI 初始窗口裁切；截图设备名称/IP 为演示数据，不是真实 LAN 发现结果 |
| ViewerUiSmoke | 实际设计接收窗口、全屏/Esc/关闭、fallback 截图；去掉设计场景后用三个屏幕像素验证真实 D3D 视频透过透明叠层，不保存桌面截图 |
| PreviewChannel | 真实 4K BGRA/SBS 缩略图、左右眼像素、跨进程读取、1Hz 限频、锁竞争、非法元数据拒绝与资源释放 |
| AdaptiveBitrate | 自动码率 20–1000 Mbps 限幅、低复杂度保持、偶发延迟保持、持续背压降档、缓慢探升、恢复冷却、发送超时减半与跨重连目标保留 |
| StreamRecovery | 同一生产发送/接收进程，经本机透明代理完成 token/能力握手；完整转发 8 帧后，在第 9 帧视频已经完整、PCM 尚未完整时暂停 700ms，关闭旧连接；恢复后继续解码到 40 帧。两代均验证完整 PCM、新连接 discontinuity、接收音频重置及音视频原子交付；隐藏接收窗口不播放声音。当前运行的具体序号、计数及停止耗时见 `validation/stream-recovery.json` |
| AudioScheduler | 模拟 10 秒 60 FPS，接收 480,000 个采样帧，播放仅保留正常起始/末尾余量；只有首次重置，无持续重锚。覆盖画面冻结和重复绘制、跳帧、回退时间戳、断线代号、迟到回调、采集 discontinuity、队列上限、分数采样边界及未来音频等待 |
| AudioEndpoint | 实际默认 Windows 声卡回环及静音播放：48 kHz 双声道 PCM 初始化、QPC 时间戳、声音提交、停帧后提交不再增长、旧连接回调拒绝、新连接恢复；应用提交的声卡队列不超过 960 个采样帧（20ms）。不保存录音，不播放测试音调，不代表扬声器实测出声同步 |
| AudioSystemEndToEnd | 独立静音源激活真实系统播放端点，生产发送程序实际回环采集 46,080 个采样帧，经视频时间戳截取并发送 41,740 个，接收程序收到同样的 41,740 个；同时解码 20 帧桌面视频。隐藏接收窗口实际播放提交为 0。统计见 `validation/audio-system.json`，只保存计数，不保存声音或桌面内容 |
| UnifiedClient | 两个相同 LAN3DShare.exe 自动发现，经界面同一按钮路径发起，自动打开接收后台、会话握手、SBS 解码及主动断开，正常退出 |
| UnifiedDesktop | 同一 GUI 流程传输本机普通桌面；确认发送标为 Desktop、接收为 2D、未初始化 Leia，正常退出 |
| ReceiverDisconnect | 正常视频包边界 EOF 输出统计并退出；指定接收帧数尚未满足时仍返回失败 |
| Injector | 自建子进程在 EXE 入口暂停，注入、正斜杠及 .. 路径、配置、启动/停止/重启、恢复及正常退出 |
| CaptureCore | DX11/DX12 WARP + debug layer：像素、RGBA/BGRA、mip/slice、mask/final、ordinal、weaver 重建、未知状态跳过；GPU 未完成、Reset、重复 Execute、退休资源与槽复用；真实转发 wrapper/native 不同地址的提交与精确像素、PrivateData 不转发时跳过、跨 Core token 唯一性，以及未观察提交后 Reset/Stop/销毁仍保留读回与 24 槽上限 |
| CaptureDirect_dx11 / dx12 / legacy11 / legacy12 | 模拟 runtime 的新工厂及旧导出，实际 WARP 资源，延迟加载、失败工厂、query/destroy 身份、旧接口嵌套调用与像素 |
| CaptureForward_dx11 / dx12 / legacy11 / legacy12 | PE 转发工厂和旧导出位于另一实现模块时仍取得正确对象和 SBS |
| CaptureDirect / Forward 的 late11 / late12 / late11-input / late12-input | 先创建旧接口 weaver、资源并调用 weave，再加载捕获 DLL；通过公开 getter 或后续 setter 恢复，校验实际左右眼像素、原始调用次数、销毁和 GPU debug。DX11 空 getter 等待后续输入，显式 deferred/null context 不被默认 context 覆盖；DX12 使用后续观察的真实列表及明确 barrier |
| CaptureDirect / Forward 的 modern-late11 / modern-late12 / modern-late11-pimpl / modern-late12-pimpl | 8 项均先创建、设置输入并实际渲染，再加载捕获 DLL；首次热附加、像素变化、停止后再次启动均没有重新调用 factory/input/context/list setter。覆盖直接及 pimpl/偏移接口、空输入/执行上下文、DX11 deferred 和 DX12 bundle 拒绝、后续真实 setter 恢复、对象销毁/替换，以及停止前已记录但未提交的 DX12 帧不被重放。全部使用实际 WARP 设备及 debug layer |
| ModernDiscovery | 实际 MSVC 直接/虚继承/偏移布局的 DX11/DX12 接口识别，实时对象校验；歧义和损坏元数据拒绝 |
| SetterAnalysis | 编译生成的直接/pimpl/setter、Win64 参数来源、vtordisp 和格式字段；字段歧义、空值不一致、部分覆写、未知 helper 获得字段地址、不可读地址与不支持代码拒绝 |
| ModernRuntimeLayout | 仅映射实际官方 SimulatedRealityDirectX.dll 与 TitanLeiaRuntime.dll，不执行 DllMain/factory。官方新版 DX11/DX12、Titan 新版 DX12 的 RTTI、SBS、context/list 和 DX12 格式读取计划均通过；不等同实际显示交织验收 |
| FrameBus | 跨进程共享帧、行距、序号、状态、竞争时不阻塞发布、异常尺寸拒绝 |
| MediaTransport | 真实 Windows 软件 H.264 + TCP 回环；非 16 对齐和奇数高度、颜色/左右眼、尺寸变化、关键帧、时间戳、超时、取消、超大包；PCM 内容/字节序往返、非法音频大小/标志/时间戳/溢出拒绝、音频半包 EOF/超时整包清空 |
| HardwareCodec | RTX 5090 实际 H.264 / HEVC / AV1 编解码；每输入立即输出、重复/回退的时间戳与序号、Desktop/SBS 切换、六色块与黑白范围、关键帧重连、640×360 → 800×600 → 650×359、非法包/配置拒绝；20→80→20 Mbps 原地改码率后持续解码，纯码率元数据变动不重建解码器 |
| 协议 v5 回归 | 0/1/3/7 硬解能力掩码、三种 codec 的 payload/header/content 元数据往返、重复协商、旧版本与未知能力拒绝；包含在 MediaTransport 内 |
| ReceiverPreview | 隐藏窗口 GPU 回读核对左右顺序及 sRGB，输入/窗口 resize，不加载 Leia |
| SenderReceiverEndToEnd | 发送 → TCP → 接收 → GPU 预览，连续两次连接均解码至少 30 个不同源帧 |
| HardwareEndToEnd | 实际 worker 自动协商 NVENC AV1 → TCP → NVDEC → GPU 预览；3840×2160、60 FPS / 400 Mbps 配置，连续两次连接均解码至少 30 个不同源帧；验证功能，不作为稳定帧率验收 |
| AutoHardwareEndToEnd / MaxBitrateEndToEnd | 实际 4K NVENC/NVDEC 收发，分别验证 0 自动从 200 Mbps 开始和 1000 Mbps 手动目标；两次连接分别至少 150/30 个不同源帧，不作为吞吐或稳定 4K60 验收 |
| StaticReconnect | 只发布一帧的静止源，连续两次连接均能解码、上传和呈现；保留原始序号和时间戳 |
| ReceiverRuntimeModern / Legacy | 模拟 runtime 验证新旧 DX11 ABI、输入宽度语义、尺寸切换、context 生命周期与 sRGB |

上述产品集成测试的网络对端为 127.0.0.1。发现测试使用独立测试端口并关闭广播。常规桌面检查只在内存中采集和通过本机回环传输；另按用户授权对真实游戏注入修复版并保存本地诊断截图。测试脚本只清理自己创建的辅助进程，不终止游戏。没有修改 ToDesk、游戏 Mod、TitanLeiaBridge 源码或已安装 runtime。8 项 modern late-attach fixture 的独立创建观察器校准后要求 device/factory/queue/allocator/list/swapchain 创建数均为 0；DXGI 首次 Present1 的内部懒初始化在计数前完成。

另已完成 **PureDark-Gaming (192.168.3.42) ↔ Huangwei-Gaming (192.168.3.197)** 的真实 LAN 测速：每方向约 9 秒、单 TCP、纯内存、按接收端实际字节数统计。正向平均 **933.723 Mbps**、最低完整一秒 **889.202 Mbps**；反向平均 **943.812 Mbps**、最低完整一秒 **925.066 Mbps**。不是双向同时饱和测速，也不包含视频编解码。记录位于 `validation/lan-bandwidth.txt`。

4K 硬件基准使用同一组预生成的 12 帧移动细节 SBS，HEVC/AV1 各测 100/200/400 Mbps，每组 12 帧预热、120 帧计时。编码含 BGRA 上传、GPU 转换与码流取回；解码含 NVDEC、GPU 转换及 BGRA 读回。两阶段分别测量，未计入画面生成、采集、网络和渲染。另一次不计时解码计算每眼 RGB PSNR、MAE 与强边缘梯度误差；包含色彩转换和 4:2:0 损失。完整结果在 `validation/hardware-codec.json`；这些合成图指标不证明所有游戏画面达到观感无损。

本机 RTX 5090、驱动 616.64，4K60 合成 SBS 结果如下。表中 FPS 为阶段吞吐，不能相加、也不是双机端到端帧率。RGB MAE 是每个 8-bit 色值的平均绝对误差，左右眼平均；越低越好。

| 编码 | 目标 Mbps | 实际视频 Mbps | 编码 FPS | 解码 FPS | RGB MAE |
| --- | ---: | ---: | ---: | ---: | ---: |
| HEVC | 100 | 95.0 | 90.7 | 171.2 | 2.291 |
| HEVC | 200 | 137.9 | 90.5 | 166.9 | 1.750 |
| HEVC | 400 | 146.2 | 90.4 | 164.7 | 1.548 |
| AV1 | 100 | 100.2 | 105.4 | 133.4 | 2.132 |
| AV1 | 200 | 195.8 | 104.7 | 119.0 | 1.611 |
| AV1 | 400 | 293.7 | 103.8 | 111.4 | 1.506 |

没有启用码流填充，实际码率随内容变化，不要求持续占满目标带宽。上述基准保留自 0.3.0；AV1 在此样本从 100 升到 200 Mbps 的 RGB MAE 降低约 24.5%，从 200 升到 400 再降低约 6.5%。默认仍取 200，新界面使用 0–1000 Mbps 滑条。4:2:0 的色度抽样损失不会因提高码率而消失。

**尚未验收：**更多局域网广播和防火墙环境、除上述单个游戏/Mod/官方 runtime 组合外的实际兼容性、更多显示器与 runtime 组合、不同显卡、目标分辨率和帧率下的性能及延迟，以及 Huangwei-Gaming 上的实际扬声器/屏幕声画偏差和本次增益后的音量听感。默认音频设备切换和失效包含重试逻辑，但本轮未实际拔插声卡。同步测试证明应用的时间戳许可、队列及恢复行为；`Present` 成功与 WASAPI 提交不等于物理屏幕扫描和扬声器出声，驱动、显示器和声音设备延迟还需双机验收。模拟转发 DLL 和 ABI 测试不能证明所有 runtime 版本兼容。界面会根据实际 SBS 是否可用决定发送类型；热附加到已有 weaver 的限制见 README。

LeiaSR 共享 GPU 采集至 NVENC 已移除完整图像 CPU 读回和上传；普通桌面/软件回退仍使用 CPU，接收端也仍保留读回和上传，尚非完整端到端零拷贝。60 FPS 是默认高画质目标，窗口标题已显示不同解码帧的接收速率，日志另外保留显示刷新速率。HDR、鼠标指针合成、旋转桌面、DX12 enhanced barrier 专用路径、多接收端不在本版实现范围内。

发布包仅包含 LAN3DShare.exe、LAN3DCapture.dll、说明、许可证和验证记录。对应两套 PDB 单独打包。package.ps1 校验 PE/PDB GUID 与 Age、复制前后的 SHA-256，并生成 manifest 和压缩包校验清单；测试 EXE、模拟 runtime 和独立诊断 CLI 不进入用户包。
