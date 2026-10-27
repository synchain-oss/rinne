# ADR-0009：环路层的证据判据

## Summary (EN)

- **Decision**: each side judges per block whether Output ran before Input (3 consecutive votes, kept at most 1 s); offline blocks that can make an export differ from playback are counted and latched in the UI. A host "not playing" report shorter than max(100 ms, 4 x largest block), counted in samples, is not a stop, so the next playing block is not a play start. A writer whose ring sample rate differs from the local one counts as no writer: return 0, dry unchanged.
- **Consequences**: rewinding timestamps no longer flip the verdict; a 16 ms stop report no longer cuts the echo tail; mismatched rates play nothing.
- **Compatibility**: changing any of these thresholds or rules is a compat event.

## 状态

已采纳（2026-10-27）。

## 背景

- Output 先于 Input，回送就多晚一块，要提示；两端拿同一个宿主时间戳，但它不单调（Cubase 切换提前处理时回退数秒），单次比较不能当证据。
- 离线导出可能与播放不同，要提示，干净的导出不能误报。
- Cubase 15 走带中间曾连续 3 块（768 样本，约 16 ms）报 isPlaying=false 再恢复，十二次里一次；随后的起播沿让回送淡出、本地延迟线复位，回声尾巴被切。那几块的宿主时间戳回退约 62 s，不能用。实测真停走带最短约 5 s。
- 两端采样率不同时，环里的帧读出来是变调的声音。

## 决定

### 错序判据

- 每个 n > 0 的块判一次。两端各读对端槽里的块序号与时间戳，判法相反、结论相同；不改会合段布局。
- 对端时间戳不小于本块的 = Output 在前。连续 3 次 Output 在前定为错序，连续 3 次 Input 在前才清除。
- 判不了的块（没有时间戳、对端没发布或没处理新块、时间戳回退或原地不动、比预期多跳 0.25 s 以上）清零连续计数，不改结论。
- 结论至多保持 1 s（按样本计），没有有效检查就回到 unknown。

### 导出不一致

- 离线块里下列任一件，该块计一次：宿主违约（块长超声明值或支持上限）、主读头补 0、对端在新块上报超限、一次重同步（断档恢复、换写方、格式变化、跳变、位移）。起播沿、首次对齐、换绑定、偏移、D 变化、采样率不一致不算。
- 对端的超限标志只在新块上看一次，第一次只记块序号。0 样本调用不读槽、不推进计数。
- 音频线程只累加计数，已读值在消息线程，计数 ≠ 已读值即锁存。不进参数与 state，不随 prepare 清零。

### 起播去抖

- 起播沿 = 确认停走带之后第一个在走的块。连续报停的样本数累计到阈值才确认，没到就恢复，整段当没发生。
- 阈值 = max(100 ms, 4 × 最大块长)，按样本计，不读墙钟；最大块长取声明值与本会话见过的最大块长里较大的。100 ms 远大于实测的 16 ms、远小于真停走带的 5 s；4 块让大块长（如 2048）下的 3 块报停不超阈值。
- 宿主不再调用时样本数不走，另用两个已有信号：两端挂起超过 250 ms 的清空，以及报停已开始时重新 prepare，都视为已停，恢复后第一个在走的块是起播沿。一直在走时同采样率重新 prepare 不动。

### 采样率不一致

环头里写方的采样率（Output 发布的取整值）与本端不同，等同没有写方：读头 10 ms 淡出，回送为 0，干声不变，不增安全转移。改回一致是新配对，从当前写头起读、500 ms 淡入，不一致期间写进环的帧不读。

## 后果

- 时间戳回退不再翻转结论，真换序最多晚 3 块发现；提示可确认，新事件让它回来。
- 报停短于阈值不再触发起播复位；挂起之后第一个在走的块即使宿主没报过停也复位。

## 被否决的备选

- 判对一次就清除：时间戳回退一次，提示就闪一下。
- 用墙钟计失效或去抖：离线快速渲染时与块长、节奏有关，违反实时线程规则。
