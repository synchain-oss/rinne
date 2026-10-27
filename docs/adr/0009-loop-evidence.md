# ADR-0009：错序与导出不一致的判据

## Summary (EN)

- **Decision**: each side judges once per block, from the shared host timestamp, whether Output ran before Input: 3 consecutive votes decide; blocks that cannot be judged (including regressing timestamps) never vote and keep the verdict for at most 1 s. Offline blocks that can make an export differ from playback are counted, and the UI latches on a count it has not acknowledged.
- **Consequences**: a host that rewinds its timestamps no longer flips the verdict; the export hint can be acknowledged and returns on new events.
- **Compatibility**: the vote count, the 1 s expiry and what counts as an export event are a compat event when changed.

## 状态

已采纳（2026-10-27）。

## 背景

同一个宿主周期里两端拿到同一个时间戳：Output 先于 Input 被处理，回送就多晚一块，要提示用户。真宿主的时间戳并不单调：Cubase 切换提前处理时，时间戳会回退数秒再跳回，两端取到同一串值，单次比较不能当证据。另一件事是离线导出：宿主可以用任意大的块、任意快的节奏调用，导出可能与播放不同，要提示，但干净的导出不能误报。

## 决定

### 错序判据

- 每个 n > 0 的块判一次。Input 块首读 Output 槽里的块序号与时间戳，Output 镜像读 Input 槽、判法相反，对同一现场结论相同；不改会合段布局。
- 对端的时间戳不小于本块的 = Output 在前。连续 3 次 Output 在前定为错序，连续 3 次 Input 在前才清除。
- 判不了的块：宿主没给时间戳、对端没发布过、对端自上次检查起没处理新块、本端或对端的时间戳回退或原地不动、本端比按样本推进的预期多跳 0.25 s 以上。这样的块清零连续计数，不改结论；回退与跳变另计数供诊断。
- 结论至多保持 1 s（按样本计，不读墙钟），没有有效检查就回到 unknown。

### 导出不一致

- 离线块里下列任一件，该块计一次（至多一次）：宿主违约（块长超声明值或支持上限）、主读头补 0、对端在新块上报超限、一次重同步（断档恢复、换写方、格式变化、跳变、位移）。
- 起播沿、首次对齐、换绑定、偏移与 D 变化不算。块长不小于最大块长、两端按序配合的干净导出一件都不会发生，计数不动。
- 对端的超限标志只在对端的新块上看一次；第一次读到只记块序号，不计（槽里可能是上一个会话的旧标志）。0 样本调用不读槽、不推进计数，只算账不出样本的块照常计。
- 音频线程只累加计数；已读值在消息线程，计数 ≠ 已读值即锁存，确认后再有新事件重新锁存。不进参数与 state，计数不随 prepare 清零。

## 后果

- 时间戳回退不再翻转结论；代价是真的换序最多晚 3 块才被发现或清除。
- 提示可以确认而不是永久关闭，新事件会让它回来；确认状态不随工程保存。
- 判据放在 core 的小类里，两端共用；这几条数值改动都要写进 CHANGELOG 的 Compatibility。

## 被否决的备选

- 判对一次就清除、判不了立刻 unknown：时间戳回退一次，提示就闪一下。
- 用墙钟计失效时间：离线快速渲染时与块长、节奏有关，违反实时线程规则。
- 在会合段加标志位让两端互报错序：会合段布局已冻结。
- 永久关闭提示：之后真出了问题也看不到。
