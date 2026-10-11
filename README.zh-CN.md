[English](README.md) | **简体中文**

# Synchain Rinne（回环）

> 一对开源插件，让通道把信号回送给自己，环路延迟由你设定。

**Rinne**（读作 *RIN-neh*，取自日文「輪廻」，与中文「轮回」同字；中文名「回环」）由两个插件组成。**Rinne Input** 放在插入链第一格，所有调节都在它上面；**Rinne Output** 放在你想纳入环路的最后一个插件之后，只负责配对。两者之间的插件会被回送到 Rinne Input，环路延迟按设计不受缓冲大小和导出方式影响。

> **状态：开发中。** 目前还没有发布版本，开发在 `feature/v1` 分支上进行。

> **警告：** 反馈可能迅速累积到很大的音量。Rinne 默认反馈为 0，并带有限制器和紧急切断，但试用时请先把监听音量调低。

计划支持的格式：Windows 与 macOS 上的 VST3，macOS 上的 Audio Unit。

以 [GPL-3.0-or-later](LICENSE) 授权发布。由 [Synchain](https://synchain.ca) 制作。
