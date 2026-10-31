// SPDX-License-Identifier: GPL-3.0-or-later
//
// 参数手势与回声协议（页面侧）：旋钮、推子、开关、分段控件都经它与桥说话。
//
// 手势：beginParamGesture -> setParam（不超过 50 Hz，末值必达）-> endParamGesture。同一个参数的桥调用串成一条链，
// 顺序不会被异步回执打乱。
// 回声：rinne.params 是宿主回声。
//   - 手势进行中，该参数的回声一律丢（显示跟手，不被回声拉回去）；
//   - 手势结束后，帧序号不超过这次手势里 setParam 回执的最大 seq 的回声继续丢（那是我们自己写入的旧回声）；
//   - 其余回声是宿主自动化：浮点参数的显示值用 rAF 指数逼近目标，时间常数 40 ms，逼近到位后停掉 rAF；
//     开关与枚举直接落位。回声值与当前目标相同时什么都不做，恒值段不会触发任何显示更新（不闪）。
//   - 页面刚打开收到的第一帧直接落位，不从 0 动画过去。
// 没有 requestAnimationFrame 的环境（首帧回归测试里被故意删掉）直接落位。
// 锁：宿主的第一批快照没到之前（locked() 为真），或这个参数还没有宿主给的值（显示值是 null），参数不接受手势与写入。
//   不挡的话，用户在这段空档里的一次拖动会把显示值设成凭空的数，第一帧真回声到了反而被当成宿主自动化去逼近它。

/** 自动化逼近的时间常数（毫秒）。 */
export const AUTOMATION_TAU_MS = 40;
/** setParam 的最小间隔（毫秒），即不超过 50 Hz。 */
export const SEND_INTERVAL_MS = 20;
/** 滚轮与方向键手势在停手多久后结束（毫秒）。 */
export const IDLE_END_MS = 300;
/** 宿主自动化的「自动」态在最后一次变化后保留多久（毫秒）。 */
export const AUTO_HOLD_MS = 600;

/**
 * @param {object} o
 * @param {object} o.bridge 桥（beginParamGesture、setParam、endParamGesture、setLimiter）
 * @param {{setTimeout:Function, clearTimeout:Function}} [o.timers]
 * @param {(cb:Function)=>number} [o.raf] 缺省取全局 requestAnimationFrame；不是函数时直接落位
 * @param {(id:number)=>void} [o.caf]
 * @param {()=>number} [o.now]
 * @param {()=>boolean} [o.locked] 为真时所有参数都不接受手势与写入（缺省不锁）
 */
export function createParamEcho(o) {
    const { bridge } = o;
    const timers = o.timers || globalThis;
    const raf =
        o.raf !== undefined
            ? o.raf
            : typeof globalThis.requestAnimationFrame === "function"
              ? (cb) => globalThis.requestAnimationFrame(cb)
              : null;
    const caf =
        o.caf ||
        (typeof globalThis.cancelAnimationFrame === "function"
            ? (id) => globalThis.cancelAnimationFrame(id)
            : () => {});
    const now = o.now || (() => (typeof performance !== "undefined" ? performance.now() : Date.now()));
    const globalLocked = o.locked || (() => false);

    /**
     * 登记一个参数。
     * @param {string} id 参数 ID
     * @param {object} [opt]
     * @param {"float"|"bool"|"enum"} [opt.kind]
     * @param {number} [opt.epsilon] 逼近到差值小于它就落位（浮点参数的显示单位）
     * @param {(v:any)=>void} [opt.onDisplay] 显示值变了
     * @param {(on:boolean)=>void} [opt.onAuto] 宿主自动化中
     * @param {(on:boolean)=>void} [opt.onGesture] 手势开始或结束
     * @param {(v:any)=>Promise<any>} [opt.send] 覆盖写入函数（限制器走 setLimiter）；给了就不走手势
     */
    function param(id, opt = {}) {
        const kind = opt.kind || "float";
        const epsilon = opt.epsilon !== undefined ? opt.epsilon : 1e-3;
        const onDisplay = opt.onDisplay || (() => {});
        const onAuto = opt.onAuto || (() => {});
        const onGesture = opt.onGesture || (() => {});

        let shown = null; // 当前显示值
        let target = null; // 最新宿主目标值
        let gesture = null; // 进行中的手势
        let dropUntil = -Infinity; // 回声帧序号不超过它就丢；一开始什么都不丢（含无桥时序号为 -1 的默认快照）
        let rafId = 0;
        let lastFrame = 0;
        let auto = false;
        let autoTimer = 0;
        let idleTimer = 0;
        let chain = Promise.resolve();

        const display = () => onDisplay(shown);
        const setAuto = (on) => {
            if (auto === on) return;
            auto = on;
            onAuto(on);
        };
        const stopApproach = () => {
            if (rafId) caf(rafId);
            rafId = 0;
        };
        const snap = (v) => {
            stopApproach();
            if (shown === v) return;
            shown = v;
            target = v;
            display();
        };
        const approach = () => {
            if (rafId || !raf) return;
            lastFrame = now();
            const step = () => {
                rafId = 0;
                const t = now();
                const dt = Math.min(100, Math.max(0, t - lastFrame));
                lastFrame = t;
                const next = shown + (target - shown) * (1 - Math.exp(-dt / AUTOMATION_TAU_MS));
                if (Math.abs(target - next) < epsilon) {
                    shown = target;
                    display();
                    return;
                }
                shown = next;
                display();
                rafId = raf(step);
            };
            rafId = raf(step);
        };

        const send = (g, v) => {
            chain = chain
                .then(() => (opt.send ? opt.send(v) : bridge.setParam(id, v)))
                .then((res) => {
                    if (res && res.ok) {
                        if (Number.isFinite(res.seq)) g.maxSeq = Math.max(g.maxSeq, res.seq);
                        if (res.value !== undefined) g.lastValue = res.value;
                    }
                })
                .catch(() => {});
        };
        const flush = (g) => {
            if (g.timer) timers.clearTimeout(g.timer);
            g.timer = 0;
            if (g.pending === undefined) return;
            const v = g.pending;
            g.pending = undefined;
            g.lastSend = now();
            send(g, v);
        };

        const api = {
            id,
            /** 当前显示值。 */
            value: () => shown,
            /** 手势进行中。 */
            active: () => gesture !== null,
            /** 宿主自动化中。 */
            automating: () => auto,
            /** 不接受手势与写入：整体被锁，或这个参数还没有宿主的值。 */
            locked: () => globalLocked() || shown === null,
            /** 页面刚打开或外部直接给值（不经桥）：落位。 */
            seed(v) {
                if (gesture) return;
                snap(v);
            },
            /** rinne.params 的一帧里本参数的值；seq 是整帧的序号。 */
            receive(value, seq) {
                if (gesture) return; // 含「已发出结束命令、回执还没回来」的手势
                if (Number.isFinite(seq) && seq <= dropUntil) return;
                if (kind === "float" ? typeof value !== "number" || !Number.isFinite(value) : value === undefined)
                    return;
                if (shown === null) {
                    snap(value);
                    return;
                }
                if (value === target) return; // 恒值回声（含逼近进行中的）：什么也不做
                target = value;
                setAuto(true);
                if (autoTimer) timers.clearTimeout(autoTimer);
                autoTimer = timers.setTimeout(() => {
                    autoTimer = 0;
                    setAuto(false);
                }, AUTO_HOLD_MS);
                if (kind === "float" && raf) approach();
                else snap(value);
            },
            /** 手势开始。已经在手势里就什么也不做。 */
            begin() {
                if (api.locked()) return undefined;
                if (gesture && !gesture.ended) return gesture;
                stopApproach();
                if (idleTimer) timers.clearTimeout(idleTimer);
                idleTimer = 0;
                const g = {
                    ended: false,
                    startValue: shown,
                    maxSeq: -1,
                    lastValue: undefined,
                    pending: undefined,
                    lastSend: -Infinity,
                    timer: 0,
                };
                gesture = g;
                target = shown;
                onGesture(true);
                if (!opt.send) chain = chain.then(() => bridge.beginParamGesture(id)).catch(() => {});
                return g;
            },
            /** 手势里给一个新值：显示立即跟手，写入按 50 Hz 合并。 */
            set(v) {
                if (api.locked()) return;
                // 已经排了结束命令（ended）、回执还没回来的手势不能再写：这时来的拖动、滚轮、方向键会让 setParam 落在
                // endParamGesture 之后、任何手势之外。当它不存在，开一个新手势
                const g = gesture && !gesture.ended ? gesture : api.begin();
                if (shown !== v) {
                    shown = v;
                    target = v;
                    display();
                }
                g.pending = v;
                const wait = SEND_INTERVAL_MS - (now() - g.lastSend);
                if (wait <= 0) flush(g);
                else if (!g.timer) g.timer = timers.setTimeout(() => flush(g), wait);
            },
            /** 手势结束：末值先发出去，再结束手势；回执里的最大 seq 之内的回声继续丢。 */
            end() {
                const g = gesture;
                if (!g || g.ended) return chain;
                g.ended = true; // 之后再来的 begin 开新手势，不往这个已经排了结束命令的手势里写
                if (idleTimer) timers.clearTimeout(idleTimer);
                idleTimer = 0;
                flush(g);
                if (!opt.send) chain = chain.then(() => bridge.endParamGesture(id)).catch(() => {});
                return chain.then(() => {
                    dropUntil = Math.max(dropUntil, g.maxSeq);
                    if (gesture === g) {
                        gesture = null;
                        // 原生侧夹取过的实际值：和显示不同就以它为准
                        if (g.lastValue !== undefined && g.lastValue !== shown) snap(g.lastValue);
                        onGesture(false);
                    }
                });
            },
            /** 回滚：显示回到起点，写回起点值，再结束手势（pointercancel 用）。 */
            cancel() {
                const g = gesture;
                if (!g || g.ended) return chain;
                if (g.startValue !== null && g.startValue !== undefined) {
                    shown = g.startValue;
                    target = g.startValue;
                    display();
                    g.pending = g.startValue;
                    flush(g);
                }
                return api.end();
            },
            /** 滚轮与方向键：第一个事件开手势，停手 IDLE_END_MS 后结束。 */
            nudge(v) {
                if (api.locked()) return;
                api.set(v);
                if (idleTimer) timers.clearTimeout(idleTimer);
                idleTimer = timers.setTimeout(() => {
                    idleTimer = 0;
                    api.end();
                }, IDLE_END_MS);
            },
            /** 一次性写入（开关、分段、双击复位、数值输入）：开手势、写、结束。 */
            commit(v) {
                api.begin();
                api.set(v);
                return api.end();
            },
            destroy() {
                stopApproach();
                for (const t of [autoTimer, idleTimer]) if (t) timers.clearTimeout(t);
            },
        };
        return api;
    }

    return { param };
}
