[English](README.md) | **简体中文**

[![Release](https://img.shields.io/github/v/release/synchain-oss/rinne?include_prereleases&style=flat-square)](https://github.com/synchain-oss/rinne/releases)
[![License](https://img.shields.io/github/license/synchain-oss/rinne?style=flat-square)](LICENSE)

<!-- BEGIN gen:platform-badge:zh -->
Windows x64 VST3 · macOS arm64 VST3/AU
<!-- END gen:platform-badge:zh -->

# Synchain Rinne（回环）

> 一对开源插件，让通道把信号回送给自己，环路延迟由你设定。

Rinne 读作 "RIN-neh"（两个音节），取自日文「輪廻」，与中文「轮回」同字；中文名「回环」。

> **状态：预发布。** 单人维护，尽力而为的支持。构建发布在[发布页](https://github.com/synchain-oss/rinne/releases)。

<!-- BEGIN gen:safety-warning:zh -->
> [!WARNING]
> 反馈可能在一瞬间变得很响。Rinne 的用途就是把一条通道接回自己。请从反馈量 0 开始，试验时把监听音箱和耳机调小，并保持安全保护开启。Rinne 会对回送信号做限幅和异常值清理，也有紧急切断（Panic），但放在环路里的插件仍可能抬高送到音箱的电平；任何软件都无法保证环路永远不啸叫。本软件不提供任何担保（GPL-3.0 第 15–16 条）。
<!-- END gen:safety-warning:zh -->

## 它做什么

<!-- entity:begin -->
回环（Synchain Rinne）是一对开源（GPL-3.0-or-later）音频插件——Rinne Input 与 Rinne Output——把一条混音通道插入链上两者之间的那一段变成反馈环路。Rinne Input 放在插入链第一格，承载全部调节；Rinne Output 放在你想纳入环路的最后一个插件之后、推子分隔线之上，只负责配对。两者之间的插件输出，会在你设定的环路延迟 D 之后送回 Rinne Input（D 不小于宿主的最大块长），环内插件自身的延迟另计。环路延迟不随缓冲大小和导出方式变化，这已在 Windows 上的 Cubase Pro 15 实测，Audio Unit 版本也在 macOS 上的 LUNA 实测（测试记录见 docs/validation）。反馈量从 0 开始，限制器与失控保护默认开启，紧急切断（Panic）随时切断回送。Windows 10/11（x64）上为 VST3 格式；macOS 11 及以上（Apple Silicon）上为 VST3 与 Audio Unit 格式（Audio Unit 为 beta）。目前为预发布：Windows 上的 Cubase Pro 15 在预览构建上通过了验收矩阵；macOS 上的 LUNA 有第一份部分测试记录，仍有一个发现未解决；其他宿主尚未测试。Rinne 本身不发起任何网络连接，无需账号。
<!-- entity:end -->

环路里的延迟、失真、均衡或混响，每一圈都会再被听到一次。环路延迟是固定的样本数，两个插件也都不向宿主上报插件延迟。回送路径上有一条安全链，对信号限幅、清理。

Rinne 免费开源，以 [GPL-3.0-or-later](LICENSE) 授权，由 [Synchain](https://synchain.ca) 制作。Rinne Input 是环路的返回端，Rinne Output 是发送端；如果你用过 Synchain 的 SCVB，注意它的 Input 与 Output 方向恰好相反。

## 插件放哪里

<!-- BEGIN gen:placement:zh -->
```
One mixer channel - inserts (Cubase shown)
+----------------------------------------------+
| 1   Rinne Input   <--- fed back after D --+  |   return end: mixes the loop back in
| 2   any plugin                            |  |   inside the loop
| 3   another plugin                        |  |   inside the loop
| ..  ...                                   |  |
| n   Rinne Output  ------------------------+  |   send end: last pre-fader slot
|----------- pre/post-fader divider -----------|
|     post-fader inserts                       |   outside the loop
+----------------------------------------------+
    fader / pan / sends                            outside the loop
```
<!-- END gen:placement:zh -->

与 SCVB 相反：Rinne 的 Input 放第一格，Output 放最后一格。

把 Rinne Output 放在你想纳入环路的最后一个插件之后、推子分隔线之上（最简单：推子前区最后一格）。两者之间的每个插件都在环路里，包括宿主自带的插入效果器，它们的延迟会叠加到 *D* 上；推子、声像和发送不在环路里。

## 系统要求

- Windows 10 或 11（x64），并安装 Microsoft Edge WebView2 运行时。
- macOS 11 或更高、Apple Silicon，宿主必须原生运行（不能经 Rosetta）。界面使用系统 WebKit（15.4 或更高）。
- 支持 VST® 3 的宿主；macOS 上另可用 Audio Unit 宿主。不支持 GarageBand。

## 已支持的宿主

<!-- BEGIN gen:daw-tiers:zh -->
| 宿主 | 版本 | 平台 | 格式 | 等级 | 状态 | 备注 |
| --- | --- | --- | --- | --- | --- | --- |
| Cubase Pro | 15 | Windows | VST3 | Tier 2 | 部分验证 | 两个预览构建上的真机记录。七项验收全部通过（2026-11-03），环内的 Cubase 自带插入效果器把它的延迟叠加到环路延迟上（2026-11-07）。升到 Tier 1 要在发布构建上重做同一套矩阵。 |
| LUNA | 3.x | macOS | VST3 / AU | Tier 3 | 部分验证 | 第一份真机记录，预览构建，只测了 Audio Unit 版本。环路周期、64 / 512 / 2048 三档缓冲、实时对离线导出、保存重开都通过。有一个发现未解决：开播后第一个回声比应有电平低 13 到 21 dB。没有测：VST3 版本（LUNA 只列系统级的 VST3）、Bounce、卸载重装、Freeze。发现修复并复测之前停在 Tier 3。Audio Unit 版本是 beta。 |
| Cubase Pro | 14 | Windows | VST3 | — | 待实测 | 尚未在真机上测试。 |
| Cubase Artist | 15 | Windows | VST3 | — | 待实测 | 尚未在真机上测试。 |
| Cubase Elements | 15 | Windows | VST3 | — | 待实测 | 尚未在真机上测试。 |
| Nuendo | any | Windows | VST3 | — | 待实测 | 尚未在真机上测试。 |
| Cubase Pro | 15 | macOS | VST3 / AU | — | 待实测 | 尚未在真机上测试。 |
| REAPER | 7 | Windows | VST3 | — | 待实测 | 尚未在真机上测试。 |
| REAPER | 7 | macOS | VST3 / AU | — | 待实测 | 尚未在真机上测试。 |
| Ableton Live | 12 | Windows | VST3 | — | 待实测 | 尚未在真机上测试。 |
| Ableton Live | 12 | macOS | VST3 / AU | — | 待实测 | 尚未在真机上测试。 |
| Studio One | any | Windows | VST3 | — | 待实测 | 尚未在真机上测试。 |
| Studio One | any | macOS | VST3 / AU | — | 待实测 | 尚未在真机上测试。 |
| Bitwig | any | Windows | VST3 | — | 待实测 | 尚未在真机上测试。它的插件托管模式可能把两个插件分进不同进程。 |
| Bitwig | any | macOS | VST3 | — | 待实测 | 尚未在真机上测试。它的插件托管模式可能把两个插件分进不同进程。 |
| FL Studio | any | Windows | VST3 | — | 待实测 | 尚未在真机上测试。 |
| FL Studio | any | macOS | VST3 / AU | — | 待实测 | 尚未在真机上测试。 |
| Logic Pro | any | macOS | AU | — | 待实测 | 只加载 AU。尚未在真机上测试。 |
| GarageBand | any | macOS | AU | 不支持 | 不支持 | 不支持：沙盒与进程外的 AU 托管未测。 |
| Pro Tools | any | 任意 | AAX | 不支持 | 不支持 | 不支持：本构建只提供 VST3 与 Audio Unit。 |
| CLAP | any | 任意 | CLAP | 不支持 | 不支持 | 不支持：本构建只提供 VST3 与 Audio Unit。 |
| VST2 hosts | any | 任意 | VST2 | 不支持 | 不支持 | 不支持：本构建只提供 VST3 与 Audio Unit。 |
| Linux hosts | any | Linux | VST3 | 不支持 | 不支持 | 不支持：本构建只提供 Windows 与 macOS 插件。 |
| Intel Macs | any | macOS | VST3 / AU | 不支持 | 不支持 | 不支持：macOS 版只有 Apple Silicon 版本。 |
| Rosetta hosts | any | macOS | VST3 / AU | 不支持 | 不支持 | 不支持：宿主必须原生运行。 |
<!-- END gen:daw-tiers:zh -->

只有 [docs/validation/](docs/validation/TEMPLATE.md) 里有书面测试记录，等级才会上调。Windows 上的 Cubase Pro 15 有两个预览构建的记录，处于 Tier 2；macOS 上的 LUNA 有第一份 Audio Unit 版本的记录，仍有一个发现未解决，修复并复测之前停在 Tier 3。其余宿主都还在等第一次测试，macOS 的 Audio Unit 版本是 beta。等级定义和各宿主的说明见 [DAW_COMPATIBILITY](docs/DAW_COMPATIBILITY.md)（英文）。

## 安装

1. 从[发布页](https://github.com/synchain-oss/rinne/releases)下载对应系统的 zip，不要下「Source code」。权威的 SHA-256 写在发布说明里，安装前先对照。
2. Windows：解除 zip 的锁定（下面的命令），解压，把两个 `.vst3` 文件夹复制到 `%COMMONPROGRAMFILES%\VST3\`。
3. macOS：安装包没有 Apple Developer ID 签名，也未经公证。把 zip 解压到 `~/Downloads/Rinne`，对这个文件夹解锁（下面的命令），在里面运行 `install-macos.sh`，然后才打开宿主。
4. 在宿主里重新扫描插件。两个插件都要装，版本必须相同。

```powershell
Unblock-File .\Rinne-v<X.Y.Z>-win64.zip
```

```sh
xattr -dr com.apple.quarantine ~/Downloads/Rinne
```

如果 macOS 提示无法验证插件，不要点「移到废纸篓」。LUNA 只列系统级文件夹里的 VST3，要在 LUNA 里用 VST3 版本，请先运行 `uninstall-macos.sh`，再运行 `install-macos.sh --system`；只用 Audio Unit 版本不需要。逐步说明（含升级和卸载）见 [INSTALL](docs/INSTALL.md)。

## 快速上手

1. 把 Rinne Input 插在通道插入链第一格，Rinne Output 插在你想纳入环路的最后一个插件之后。
2. 确认两端的指示灯都显示「已连接」。组号（A–H）一致才会连接；新插入的插件会自己挑一个组。
3. 先把监听音量调低。反馈量从 0 开始。
4. 慢慢加大反馈量，边调边听。设定环路延迟 *D* 和混合。
5. 出了问题先按「紧急切断」，或者在宿主里旁路 Rinne Input。

开始之前请先读这几条：

<!-- BEGIN gen:hard-rules:zh -->
1. **放置位置**：Rinne Input 插在通道插入链的第一格；Rinne Output 插在你想放进环路的最后一个插件之后、推子分隔线之上（最简单：推子前区最后一格；宿主没有推子前区时，插在插入链最后一格）。两者之间的插件，包括宿主自带的插入效果器，都在环路里，它们自身的延迟会叠加到环路延迟 D 上；Output 之后的插件、推子、声像和发送都不在环路里。
2. **从 0 开始**：新实例的反馈量是 0，加载工程或预设时回送从 0 淡入。先调低监听音量，再从 0 慢慢往上调：回送越多越容易自激。混合在 50% 及以上、环路里没有衰减时，反馈超过 100% 一定会自激；环路里的插件带增益（EQ 提升、压缩补偿、饱和）时，更低也可能自激。限制器和失控保护默认开启，但挡不住长时间的高电平。
3. **出事先切断**：听到啸叫或异常，先按「紧急切断」：它只切断回送，干声照常通过；恢复请点提示条里的「恢复回送」。窗口关着时，最快的办法是在宿主里旁路 Rinne Input。推子不在环路里，只拉推子只是听不见，环路仍在增长；停止播放和静音也停不住自激。
4. **一组一对**：一个组（A–H）只放一个 Rinne Input 和一个 Rinne Output，两端组号一致才会连接。复制轨道会连组号一起复制：副本会显示「冲突」，在副本的 Rinne Input 上点「移到空闲组」，再在副本的 Rinne Output 上点「重新配对」。
<!-- END gen:hard-rules:zh -->

## 安全

- 反馈量默认 0，限制器和失控保护默认开启，回送在新配对、换了写入方、载入预设或工程、「恢复回送」之后，都会在 500 ms 内淡入。
- 送回去的一切都先清理（非有限值变成 0）、滤掉直流、限幅、软削波；任何失败只会让回送静音，不会让它变响。
- 紧急切断只切断回送，干声照常播放。
- Rinne Output 对宿主的音频原样直通，旁路时往环路写入静音。
- Rinne 无法知道你放进环路的插件做了什么。它们可能抬高送到音箱的电平，长时间的高电平不会被拦住（见 [KNOWN_ISSUES](docs/KNOWN_ISSUES.md)）。

## 隐私

Rinne 本身不发起任何网络连接，不需要账号，也不收集任何数据。它的界面在 Windows 上由 Microsoft Edge WebView2 运行时绘制，在 macOS 上由系统 WebKit 绘制；它们是由微软和苹果更新、配置的操作系统组件（例如 Microsoft Defender SmartScreen 的设置会作用于 WebView2），Rinne 不改变它们的行为。Rinne 会写入的文件列在[用户指南](docs/USER_GUIDE.zh-CN.md#隐私与磁盘上的文件)里。

## 从源码构建

```powershell
git clone https://github.com/synchain-oss/rinne.git
cd rinne
pwsh scripts/build.ps1 -JucePath <JUCE checkout>
```

JUCE 按提交钉版，由你传入，不会自动下载。Windows 与 macOS 的完整步骤见 [BUILDING](docs/BUILDING.md)。

## 已知限制

- 环路时间是 *D* 加上环路里每个插件的延迟（KI-1）。
- 会在静音时挂起插件的宿主可能中断环路；把 Tail 模式设为「无限」，或关掉宿主的这个选项（KI-3）。
- 回送还在淡入时（载入或配对后约前 0.5 秒）开始的导出，开头是淡入的（KI-5）。
- 中等环路增益下的稳态自激会被压在上限，不会被停掉；请用紧急切断（KI-6）。
- macOS 版只有 Apple Silicon，未签名、未公证；不支持 Intel Mac、Rosetta 下的宿主和 GarageBand，Logic Pro 未测试（KI-10、KI-11）。
- LUNA 只列系统级文件夹里的 VST3；要在 LUNA 里用 VST3 版本，请先卸载装给当前用户的那份，再用 `--system` 安装（KI-14）。

## 文档

- [用户指南](docs/USER_GUIDE.zh-CN.md)：放置、控件、安全、导出、故障排查。
- [安装](docs/INSTALL.md)与[构建](docs/BUILDING.md)。
- [宿主兼容性](docs/DAW_COMPATIBILITY.md)与[已知问题](docs/KNOWN_ISSUES.md)。
- [架构](docs/ARCHITECTURE.md)、[契约](docs/contracts/PARAMETERS.md)与[决策记录](docs/adr/README.md)。
- [更新日志](CHANGELOG.md)。

## 参与贡献

见 [CONTRIBUTING](CONTRIBUTING.md)。每个提交都要签署（`git commit -s`）。安全问题请走 [SECURITY](SECURITY.md)，不要公开提 issue。

## 许可

源码以 [GPL-3.0-or-later](LICENSE) 授权。二进制整体按 GPLv3 分发，其中 JUCE 部分适用 AGPLv3（GPLv3 第 13 条）。可以用在商业作品里：GPL 管的是软件的复制与分发，不管你用它做出来的音频。不提供任何担保。第三方组件及其许可见 [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES.md)。

## 商标

VST 是 Steinberg Media Technologies GmbH 的注册商标。Cubase、LUNA、Logic Pro、GarageBand、macOS、Audio Units、Windows、WebView2 及其他商标归各自所有者所有。

GPL 不授予商标权利（见 GPLv3 第 7(e) 条）。Synchain 与 Rinne 的名称和标志不在 GPL 授权范围内；修改版不得以暗示官方的方式使用它们。

## 相关项目

- [SCVB](https://github.com/synchain-oss/scvb)：Synchain 的人声平衡插件对。
- [synchain.ca](https://synchain.ca)：项目网站。
