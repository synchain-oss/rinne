# ADR-0010：宿主短暂报停与两端采样率不一致

## Summary (EN)

- **Decision**: a host "not playing" report shorter than max(100 ms, 4 x largest block), counted in samples, is not a stop; the first playing block after a confirmed stop is the play start. A writer whose ring sample rate differs from the local one counts as no writer: return 0, dry unchanged, 500 ms fade-in once the rates match again.
- **Consequences**: a 16 ms stop report no longer cuts the echo tail; the first playing block after a suspend always resets.
- **Compatibility**: changing the threshold, the confirming signals or the rate rule is a compat event.

## 状态

已采纳（2026-10-27）。

## 背景

- Cubase 15 走带中间曾连续 3 块（每块 256 帧，共 768 样本，约 16 ms）报 isPlaying=false 再恢复，十二次切换里一次。旧口径「上一块没走、这一块走了」把恢复当起播沿：回送 5 ms 淡出、本地延迟线复位，回声尾巴被切。那几块的宿主时间戳回退约 62 s，不能拿来判时长。
- 同一份探针日志里，真停走带最短约 245,000 样本（约 5 s）。
- 两端采样率不同时，环里的帧按另一个速率读出来是变调的声音。环头里写方发布的采样率原先引擎不读。

## 决定

### 起播去抖

- 起播沿 = 确认停走带之后第一个在走的块。连续报停的样本数累计到阈值才确认，没到就恢复，整段当没发生。
- 阈值 = max(100 ms, 4 × 最大块长)，按样本计，不读墙钟；最大块长取声明值与本会话见过的最大块长里较大的。下限 100 ms 约为实测 16 ms 的 6 倍、真停走带最短 5 s 的 1/50，落在两者之间偏小的一侧；4 块让大块长（如 2048 帧）下的 3 块报停不超阈值。只有一份宿主日志，有别的宿主的数据再调。
- 宿主不再调用，或只送一块报停就挂起时，样本数不走，累计分不出短暂报停与真停走带。另用两个已有信号：两端挂起超过 250 ms 的清空，以及报停已开始时重新 prepare，都按已停处理。一直在走时同采样率重新 prepare 不动。
- 取舍：挂起之后第一个在走的块一律算起播沿，哪怕宿主挂起期间没报过停。250 ms 以上的挂起本就清空了本地延迟线；吞掉真起播的复位，旧回声会复燃，比多复位一次糟。

### 采样率不一致

环头里写方的采样率（Output 发布的取整值）与本端不同，等同没有写方：读头 10 ms 淡出，回送为 0，干声不变；不增安全转移，InputEngine 与安全状态机不动。改回一致是新配对：从当前写头起读，首个有效读样本起 500 ms 淡入，不一致期间写进环的帧不读。导出里同样回送为 0，与播放一致，不计入导出不一致。

## 后果

- 短于阈值的报停不再触发起播复位；代价是短于阈值的真停走带（例如脚本在 100 ms 内连点停止与播放）也不复位。
- 挂起之后第一个在走的块总会复位。阈值只按一份 Cubase 日志标定。
- 采样率不一致期间回送是静音而不是变调；状态快照给出 rateMismatch 与对端采样率。

## 被否决的备选

- 用宿主时间戳判停多久：它回退约 62 s 且不单调。
- 用墙钟计短停：离线快速渲染时与块长、节奏有关，违反实时线程规则。
- 固定 100 ms：2048 帧、48 kHz 下 3 块已约 128 ms。
- 不一致时重采样读出：实时线程要加变速插值，D 的样本口径也跟着变。
- 不一致时走安全锁存：可自动恢复的情形不该要人工确认。
