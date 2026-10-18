// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web-preview.json
//
// 等状态回声到位。mock 的回声和真桥一样是异步的：写函数的回执先到，rinne.state 晚一拍才到，
// 所以「写完立刻读状态」的断言读到的是旧值。不要为此把回声改成同步，也不要写死 sleep：
// 那是把判据钉在当前的延迟常数上。这里轮询到条件成立为止，并给一个明确的上界。

/** 轮询直到 read() 的结果满足 ok()，或超时抛错（错误里带最后一次读到的值，便于定位）。 */
export async function awaitState(read, ok, label, timeoutMs = 3000, stepMs = 25) {
    const t0 = Date.now();
    let last;
    for (;;) {
        last = await read();
        if (ok(last)) return last;
        if (Date.now() - t0 > timeoutMs) {
            throw new Error(`等 ${label} 超时（${timeoutMs} ms）；最后读到：${JSON.stringify(last)}`);
        }
        await new Promise((r) => setTimeout(r, stepMs));
    }
}

/** 便捷式：等收集表里某个事件的最新载荷满足条件。seen 是 Map，键 "<事件>:last" 存最新载荷。 */
export function awaitSeen(seen, event, ok, label, timeoutMs) {
    return awaitState(
        () => seen.get(`${event}:last`),
        (v) => v != null && ok(v),
        label || event,
        timeoutMs,
    );
}
