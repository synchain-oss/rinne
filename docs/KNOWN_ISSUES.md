# Known issues / 已知问题

**EN** These are the limits we know about, each with a way around it. The ids (KI-n) are stable: other documents refer to them. If you meet something that is not listed, see [the user guide](USER_GUIDE.md#troubleshooting) first, then report it.

**中文** 下面是我们已知的限制，每条都附了绕开的办法。编号（KI-n）保持稳定，别的文档会引用它们。遇到没有列出的情况，先看[用户指南](USER_GUIDE.zh-CN.md#故障排查)，再来反馈。

## KI-1: the loop takes D plus the latency of the plugins in it

**EN** Every plugin between Rinne Input and Rinne Output adds its own latency to the loop, so one trip takes D plus all of it. The interface shows "Total = D + in-loop plug-in latency". To hit a musical time, lower D by the latency of the plugins in the loop.

**中文** Rinne Input 与 Rinne Output 之间的每个插件都会把自己的延迟加到环路上，所以绕一圈的时间是 D 加上这些延迟。界面上写作「总延迟 = D + 环内插件延迟」。想对准某个音乐时值，就把 D 减去环内插件的延迟。

## KI-2: the two ends on different tracks or in different processes

**EN** Rinne can pair two ends that sit on different tracks, or that a host loads into different processes. The loop delay may then vary, and the interface says so (OTHER TRACK?, OTHER PROCESS). Keep both ends on one track, and check the plugin hosting mode of hosts that can isolate plugins.

**中文** Rinne 能把放在不同轨道、或被宿主加载进不同进程的两端配成一对，但环路延迟可能因此变化，界面会提示（「不同轨道？」「跨进程」）。尽量把两端放在同一条轨上，对能隔离插件的宿主，检查它的插件托管模式。

## KI-3: hosts that suspend plugins during silence can break the loop

**EN** Some hosts stop calling a plugin when its input is silent. The loop then stops circulating until sound comes back. Set Tail mode to Infinite in About (Auto already means infinite for VST3 and 30 s for Audio Unit), or turn the host’s "suspend on silence" style option off.

**中文** 有些宿主在输入静音时会停止调用插件，环路也就停止循环，直到声音回来。在「关于」里把 Tail 模式设为「无限」（「自动」对 VST3 已经是无限、对 Audio Unit 是 30 秒），或者关掉宿主里「静音时挂起」这类选项。

## KI-4: two projects, or two instances, on the same group

**EN** Groups A–H are shared by everything running in your login session. If a second project, or a second instance in the same project, uses a group that is taken, its Rinne Input shows CONFLICT and has no return. Use Move to free group, or pick another letter.

**中文** A–H 这几个组由你当前登录会话里运行的所有东西共用。第二个工程、或同一工程里的第二个实例用了已被占用的组，它的 Rinne Input 会显示「冲突」，没有回送。用「移到空闲组」，或者换一个字母。

## KI-5: an export that starts right after load or pairing begins with a fade-in

**EN** The return fades in over 500 ms after a new pairing, a new writer, a preset or project load, and Restore return, which is a safety rule. An export that starts within about 0.5 s after the project loads or the pair connects therefore has a faded-in return at its start, unlike later realtime playback. Wait a second after loading or re-pairing before you export. Turning Loop back on, or taking Rinne Input out of bypass, is not one of these: the return comes back to its previous level within about 10 ms.

**中文** 回送在新配对、换了写入方、载入预设或工程、「恢复回送」之后，都会在 500 ms 内淡入，这是安全规则。所以工程刚加载、配对后约 0.5 秒内就开始的导出，开头的回送是淡入的，与之后的实时播放不同。加载或重新配对之后，等一秒再导出。重新打开「环路」、或取消 Rinne Input 的旁路不在其列：回送会在约 10 ms 内回到原来的电平。

## KI-6: steady self-oscillation at moderate loop gain is held at the ceiling

**EN** The runaway protection steps in only when the limiter reduces the gain by 6 dB or more. A loop that self-oscillates at a loop gain below about +6 dB is pinned at the limiter ceiling instead: it is not growing, but it is not stopping either. Lower Feedback, or press PANIC.

**中文** 只有限制器压掉 6 dB 或更多时，失控保护才会介入。环路增益低于约 +6 dB 的自激会被钉在限制器上限：它不再增长，但也不会停下来。请调低反馈量，或按「紧急切断」。

## KI-7: a hot, harmonically rich dry signal can trip the +12 dBFS protection

**EN** The huge-level check cuts the return at +12 dBFS sustained for 100 ms. A hot dry signal rich in harmonics (clipped sine, sawtooth, square) can show sample peaks 1–8 dB higher after phase shifts in the loop and cut the return when it should not. Lower the level into Rinne Input and use Restore return.

**中文** 超大值检查在 +12 dBFS 持续 100 ms 时切断回送。谐波丰富的热干声（削波正弦、锯齿、方波）经过环路里的相位偏移后，采样峰值可能高出 1–8 dB，在不该切断时切断回送。请降低进入 Rinne Input 的电平，并用「恢复回送」。

## KI-8: an export can differ from playback when the host breaks its own limits

**EN** If a host sends blocks larger than it declared, or D is below the block size, or the loop resynchronised mid-way, the export can differ from realtime playback. The notice bar then says "Last export may differ" with the cause. Raise D and export again.

**中文** 宿主送来的块比它声明的大、D 小于块长，或环路中途重新对齐过，导出就可能与实时播放不同。提示条会写「上次导出可能与播放不同」并给出原因。调大 D 再导出。

## KI-9: a sample-rate mismatch silences the return

**EN** If the two ends run at different sample rates, the return stops and the lamp reads SAMPLE RATE MISMATCH. Make both ends run at the same rate (one project, one sample rate).

**中文** 两端的采样率不同时，回送会停止，指示灯显示「采样率不一致」。让两端用同一个采样率（一个工程用一个采样率）。

## KI-10: the macOS builds are not signed with a Developer ID and not notarized

**EN** macOS asks for an extra step before a host can open the plugins: remove the download quarantine flag first, then open your DAW. [INSTALL](INSTALL.md) has the steps.

**中文** macOS 版没有 Developer ID 签名，也未经公证，所以宿主能打开插件之前要多一步：先清除下载隔离标记，再打开 DAW。步骤见 [INSTALL](INSTALL.md)。

## KI-11: macOS is Apple Silicon only, and some hosts are out

**EN** The macOS build runs on Apple Silicon only, and the host has to run natively (not under Rosetta). GarageBand is not supported. Logic Pro loads Audio Units only and has not been tested. See [host compatibility](DAW_COMPATIBILITY.md).

**中文** macOS 版只有 Apple Silicon，宿主必须原生运行（不能经 Rosetta）。不支持 GarageBand。Logic Pro 只加载 Audio Unit，还没有测过。见[宿主兼容性](DAW_COMPATIBILITY.md)。

## KI-12: without a usable web view the plugins show a fallback panel

**EN** The interface is drawn by the Microsoft Edge WebView2 Runtime on Windows. If it is missing or too old, or the interface fails to start, the plugins show a simple panel instead. Its Panic, Resume and Loop buttons work without the web interface, but only on Rinne Input; on Rinne Output they are disabled. The panel also offers Install WebView2 Runtime and Retry. Install the runtime from Microsoft.

**中文** Windows 上的界面由 Microsoft Edge WebView2 运行时绘制。它缺失或过旧、或界面启动失败时，插件会改为显示一个简易面板。面板上的 Panic、Resume 和 Loop 按钮不依赖网页界面也能用，但只在 Rinne Input 上可用，Rinne Output 的面板上它们是禁用的。面板上另有 Install WebView2 Runtime 和 Retry。请从微软安装该运行时。

## KI-13: older WebKit fixes the zoom at 100 %

**EN** With WebKit older than 26.4 the zoom cannot be changed and stays at 100 %. The interface says "WebKit older than 26.4: zoom fixed at 100%".

**中文** WebKit 低于 26.4 时无法改变缩放，固定为 100%。界面会写「WebKit 低于 26.4，缩放固定为 100%」。

## KI-14: LUNA lists VST3 plugins only from the system folder

**EN** LUNA does not list VST3 plugins installed in your user Library (`~/Library/Audio/Plug-Ins/VST3`, where `install-macos.sh` puts them by default); only those in `/Library/Audio/Plug-Ins/VST3` appear. Audio Units are found in either place. To use the VST3 builds in LUNA, run `uninstall-macos.sh` first (so the same plugin is not registered in two places), then `install-macos.sh --system`, which asks for an administrator password. Using only the Audio Unit builds needs no change. [INSTALL](INSTALL.md#macos) has the steps.

**中文** LUNA 不会列出装在用户资源库（`~/Library/Audio/Plug-Ins/VST3`，`install-macos.sh` 默认装到这里）里的 VST3 插件，只列 `/Library/Audio/Plug-Ins/VST3` 里的。Audio Unit 在两处都能被找到。要在 LUNA 里用 VST3 版本，先运行 `uninstall-macos.sh`（免得同一个插件在两处各登记一次），再运行 `install-macos.sh --system`，它会要管理员密码。只用 Audio Unit 版本不需要改。步骤见 [INSTALL](INSTALL.md#macos)。
