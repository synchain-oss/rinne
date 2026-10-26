# SPDX-License-Identifier: GPL-3.0-or-later
"""探针日志的合成夹具。

按 LOG-FORMAT.md 手写出探针会写的 CSV，供 analyze-probe.py 自测，也能落盘成 fixtures/。
每个判据 A1-A9 各有通过、不通过、数据不全三格，另有一套全部通过的 full-pass。
文件内容全由本模块的确定性代码生成，不含任何真实路径、用户名或主机名。

    python scripts/probe/probe_synth.py --write      # 重写 fixtures/
    python scripts/probe/probe_synth.py --check      # 与已入库的 fixtures/ 逐字节对拍
"""

import json
import os
import shutil
import sys

MAGIC = "rinne-probe-log,1"
PERIOD = 5_333_333  # 256 帧 @ 48 kHz，单位 ns
T_START = 1_000_000_000
HOST0 = 7_000_000_000
STATES = ("pass", "fail", "incomplete")
ITEMS = tuple(f"A{i}" for i in range(1, 10))
HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE_DIR = os.path.join(HERE, "fixtures")

BLOCK_COLS = (
    "seq call_seq role n n_decl host_ns host_ns_valid playing nonrt bypassed mono_ns thread m w off rs d".split()
)


class Log:
    """一个探针实例的日志。"""

    def __init__(self, role, fmt="vst3", host_kind="cubase", os_name="win", arch="x64", pid=4100, ppid=900, tok="a1b2"):
        self.role = role
        self.pid = pid
        self.tok = tok
        self.lines = [MAGIC, f"H,{role},{fmt},{host_kind},{os_name},{arch},{pid},{ppid},{tok},0123abcd"]
        self.seq = 0
        self.written = 0  # 实际写出的 B 行数
        self.lost = 0  # 占了 seq 却没写出的块数（探针计入 dropped）
        self.closed = False

    @property
    def name(self):
        return f"probe-{self.role}-{self.pid}-{self.tok}.csv"

    def _add(self, line):
        """Z 行必须是最后一行：收尾之后再补的内容插在它前面。"""
        self.lines.insert(len(self.lines) - 1 if self.closed else len(self.lines), line)

    def case(self, t, case, variant="-", buf="-", guard="na", sr=48000):
        self._add(f"C,{t},{case},{variant},{buf},{guard},{sr}")

    def event(self, t, name, a="", b=""):
        self._add(f"E,{t},{name},{a},{b}")

    def summary(self, t, blocks=0, dropped=0, max_n=256, resync=0, trips=0):
        self._add(f"S,{t},{blocks},{dropped},{max_n},{resync},{trips}")

    def block(self, call_seq, mono_ns, drop=False, **kw):
        row = dict(
            seq=self.seq, call_seq=call_seq, role=self.role, n=256, n_decl=256, host_ns=mono_ns, host_ns_valid=1,
            playing=1, nonrt=0, bypassed=0, mono_ns=mono_ns, thread="1a2b3c4d", m=0, w=4800 + self.seq * 256,
            off=480, rs=0, d=9600,
        )  # fmt: skip
        row.update(kw)
        self.seq += 1
        if drop:
            self.lost += 1
        else:
            self.written += 1
            self.lines.append("B," + ",".join(str(row[c]) for c in BLOCK_COLS))

    def close(self, t, blocks=None, dropped=0):
        self.lines.append(f"Z,{t},{self.seq if blocks is None else blocks},{dropped}")
        self.closed = True

    def text(self):
        return "\n".join(self.lines) + "\n"


def pair(case, variant="-", buf="-", guard="na", *, count=40, hook=None, fmt=("vst3", "vst3"), pid=4100,
         tok=("a1b2", "c3d4"), host_kind="cubase", os_name="win", prepare_max=256, blocks=True, close=True,
         sr=48000, pre_blocks=0, period=PERIOD):  # fmt: skip
    """一对 In / Out 实例。hook(role, k, row) 可改行；row["_drop"] = True 表示丢掉这一条（制造序号缺口）。
    period 是相邻两块的间隔（ns）。默认 5.3 ms 一块，整个格不到 1 s；换成 LONG 这类慢节奏时，摘要改成每秒一条一直写到最后一块之后，
    这样几十秒的格里 1 Hz 摘要不缺，收尾也在最后一块之后。"""
    li = Log("I", fmt[0], host_kind, os_name, pid=pid, tok=tok[0])
    lo = Log("O", fmt[1], host_kind, os_name, pid=pid, tok=tok[1])
    for lg in (li, lo):
        lg.event(T_START - 500, "prepare", sr, prepare_max)
        lg.case(T_START - 100, case, variant, buf, guard, sr)
        lg.summary(T_START)
    for j in range(pre_blocks):  # 选 case 之前就开始写的块
        for lg, dt in ((li, 0), (lo, 1000)):
            lg.block(2 * j + (lg is lo), T_START - 1000 + j * 100 + dt, host_ns=HOST0 - 10_000 + j)
    if blocks:
        for k in range(count):
            t = T_START + k * period
            k2 = k + pre_blocks
            for lg, cs, dt in ((li, 2 * k2, 0), (lo, 2 * k2 + 1, 1000)):
                row = dict(host_ns=HOST0 + k * period, mono_ns=t + dt, rs=1 if k == 0 and lg is li else 0)
                if hook:
                    hook(lg.role, k, row)
                drop = row.pop("_drop", False)
                cs = row.pop("_cs", cs)
                lg.block(cs, row.pop("mono_ns"), drop=drop, **row)
    end = T_START + 2_000_000_000
    if period != PERIOD and blocks:
        end = max(end, T_START + count * period + 1_000_000_000)
    for lg in (li, lo):
        s = T_START + 1_000_000_000
        while s <= end:  # 默认节奏下就是 1 s、2 s 两条
            lg.summary(s, blocks=count if blocks else 0)
            s += 1_000_000_000
        if close:
            lg.close(end + 100_000_000)
    return li, lo


def multi_pair(cells, *, pid, tok=("e1f2", "e3f4"), fmt=("vst3", "vst3"), host_kind="cubase", os_name="win",
               count=40, sr=48000, prepare_max=1024):  # fmt: skip
    """一对实例连续记录多个格：seq 与 call_seq 是实例级的，跨格（跨记录窗口）不归零。cells = [(case, variant, buf, guard)]。"""
    li = Log("I", fmt[0], host_kind, os_name, pid=pid, tok=tok[0])
    lo = Log("O", fmt[1], host_kind, os_name, pid=pid, tok=tok[1])
    span = 1_000_000_000
    for lg in (li, lo):
        lg.event(T_START - 500, "prepare", sr, prepare_max)
        lg.summary(T_START)
    for c, (case, variant, buf, guard) in enumerate(cells):
        base = T_START + c * span
        for lg in (li, lo):
            lg.case(base - 100, case, variant, buf, guard, sr)
            lg.summary(base + span // 2)
        for k in range(count):
            g = c * count + k
            host = HOST0 + c * span + k * PERIOD
            li.block(2 * g, base + k * PERIOD, host_ns=host, rs=1 if g == 0 else 0)
            lo.block(2 * g + 1, base + k * PERIOD + 1000, host_ns=host)
    end = T_START + len(cells) * span
    for lg in (li, lo):
        lg.summary(end)
        lg.close(end + 100_000_000)
    return li, lo


def multi_cells(cells, *, pid, tok=("e1f2", "e3f4"), sr=48000, prepare_max=1024, summaries=True, pre=""):
    """像 multi_pair，但每格单独指定块数与记录方式，用来造「现场误记了一格」的日志。
    cells = [(case, variant, buf, guard, 块数, 方式)]，方式：
      ok          正常写出；
      unbound     窗口里拿不到绑定：块不写 B 行，却占 seq、计丢弃（探针对没取到绑定的块就是这么记的）；
      gap:K       In 端第 K 块丢掉（序号缺口，同时计丢弃）；
      headdrop    两端的第 0 块都没拿到绑定：占 seq、计丢弃，其余正常（紧跟在前一格后面时，缺口出现在两格交界处）；
      headdrop-in 同上，只有 In 端丢第 0 块；
      headdrop-drift 两端的第 0 块都没拿到绑定（缺口出现在两格交界处，归给这一格），且 In 端从第 20 块起 m=3；
      taildrop    正常写完之后，窗口末尾又有 3 块没写出（占 seq、计丢弃，但之后没有任何块可以让缺口露出来）；
      drift       In 端从第 20 块起 m=3（稳态对齐不通过）；
      bign        两端每块的 n 都是 2048，超过 prepare 声明的最大块长（A4 不通过）；
      hostns      Out 端每块的 hostNs 比 In 端大 7（A7 不通过）。
    每格的 S 行在这一格的块都写完之后才写，`blocks` 是此刻实际写出的累计块数，`dropped` 是累计丢弃数，与探针的计数一致；
    Z 行同理。这样 `blocks + dropped` 就是此刻已经占用的 seq 个数。
    summaries=False 不写任何 S 行（摘要线程被宿主占住、一条都没写出来），只剩收尾的 Z 行报累计丢弃数。
    pre：第一个 C 行之前就开始写的块（孤块），每个字符一块：o = 写出，x = 丢掉（占 seq、计丢弃）。"ooxxooxo" 是孤块之间有两处缺口，
    "xxoo" 是实例开头先丢了两块、之后才有孤块。第一条摘要在这些块之后写，blocks + dropped 仍是已占用的 seq 个数。"""
    li = Log("I", "vst3", "cubase", "win", pid=pid, tok=tok[0])
    lo = Log("O", "vst3", "cubase", "win", pid=pid, tok=tok[1])
    span = 1_000_000_000
    for lg in (li, lo):
        lg.event(T_START - 500, "prepare", sr, prepare_max)
    g = 0
    for ch in pre:  # 孤块的时间戳早于第一个 C 行（T_START - 100）
        for lg, cs, dt in ((li, 2 * g, 0), (lo, 2 * g + 1, 50)):
            lg.block(cs, T_START - 5_000 + g * 100 + dt, host_ns=HOST0 - 10_000 + g, drop=ch == "x")
        g += 1
    for lg in (li, lo):
        if summaries:
            lg.summary(T_START, blocks=lg.written, dropped=lg.lost)
    for c, (case, variant, buf, guard, count, mode) in enumerate(cells):
        base = T_START + c * span
        for lg in (li, lo):
            lg.case(base - 100, case, variant, buf, guard, sr)
        for k in range(count):
            host = HOST0 + c * span + k * PERIOD
            lost_in = mode == "unbound" or mode == f"gap:{k}" or (k == 0 and mode in ("headdrop", "headdrop-in", "headdrop-drift"))
            lost_out = mode == "unbound" or (k == 0 and mode in ("headdrop", "headdrop-drift"))
            extra = dict(m=3, off=483) if mode in ("drift", "headdrop-drift") and k >= 20 else {}
            both = dict(n=2048) if mode == "bign" else {}
            li.block(2 * g, base + k * PERIOD, host_ns=host, rs=1 if g == 0 else 0, drop=lost_in, **extra, **both)
            lo.block(2 * g + 1, base + k * PERIOD + 1000, host_ns=host + 7 if mode == "hostns" else host, drop=lost_out, **both)
            g += 1
        if mode == "taildrop":
            for lg in (li, lo):
                for _ in range(3):
                    lg.block(0, 0, drop=True)
        for lg in (li, lo):
            if summaries:
                lg.summary(base + span // 2, blocks=lg.written, dropped=lg.lost)
    end = T_START + len(cells) * span
    for lg in (li, lo):
        if summaries:
            lg.summary(end, blocks=lg.written, dropped=lg.lost)
        lg.close(end + 100_000_000, blocks=lg.written, dropped=lg.lost)
    return li, lo


def clone(role="I", pid=4990, tok="f0f0", span_ns=300_000_000, close=True):
    """Render in Place / Freeze 的临时克隆实例：拿到 setState 就开了日志，没有 C 行，活不到第一条 1 Hz 摘要。
    close=False 是它被强杀：没有 Z 行。"""
    lg = Log(role, pid=pid, tok=tok)
    lg.event(T_START, "prepare", 48000, 256)
    lg.event(T_START + 50_000_000, "set_state", "A")
    if close:
        lg.close(T_START + span_ns)
    else:
        lg.event(T_START + span_ns, "destruct")
    return lg


def files_of(*logs, extra=None):
    out = {lg.name: lg.text() for lg in logs}
    out.update(extra or {})
    return out


def compare_text(*rows):
    head = "case,variant,bit_exact,residual_dbfs,head_residual_dbfs,lag_samples"
    return "\n".join([head, *rows]) + "\n"


def _drop_at(k0, role="I"):
    def hook(r, k, row):
        if r == role and k == k0:
            row["_drop"] = True

    return hook


def scen_a1(state, pid=4100):
    if state == "pass":
        hook = None
    elif state == "fail":
        def hook(r, k, row):
            if r == "I" and k >= 20:
                row["m"] = 3
                row["off"] = 483
    else:
        hook = _drop_at(15)
    return files_of(*pair("CB-01", "steady", 256, "low", hook=hook, pid=pid))


def scen_a2(state, pid=4100):
    toggles = 3 if state == "incomplete" else 10
    ks = [5 + 10 * i for i in range(toggles)]

    def hook(r, k, row):
        if r == "I" and k in ks:  # 切换后的第一块记一次重同步；失败格里第 3 次切换记两次
            row["rs"] = 2 if (state == "fail" and k == ks[2]) else 1

    li, lo = pair("CB-02", "toggle", 256, "low", count=120, hook=hook, pid=pid)
    for i, k in enumerate(ks):
        li.event(T_START + k * PERIOD, "monitor_toggle", i % 2)
    return files_of(li, lo)


THREADS = ("1a2b3c4d", "5e6f7a8b")
THREAD3 = "9c0d1e2f"
LONG = 200_000_000  # 慢节奏夹具的块间隔：0.2 s 一块，2 s 窗口里有 10 块
SW_GAP = 15  # 相邻两次切换隔 15 块 = 3 s：比 2 s 窗口宽，也是清单让用户等的节奏


def switch_blocks(n, gap=SW_GAP):
    return [gap * (i + 1) for i in range(n)]


def a2_long(pid, thread_of, *, count, rs_of=None, explicit_at=(), buf=256, guard="low", tok=("a1b2", "c3d4"), host_of=None):
    """慢节奏的 CB-02 toggle 格（0.2 s 一块）。thread_of(k) 给 In 端第 k 块的线程散列，rs_of(k) 给它的重同步次数；
    explicit_at 里的块序号处另写 monitor_toggle 事件（宿主不通知监听键，真实日志里没有这类事件）；host_of(k) 给两端第 k 块的 hostNs。"""

    def hook(r, k, row):
        if host_of:
            row["host_ns"] = host_of(k)
        if r != "I":
            return
        row["thread"] = thread_of(k)
        if rs_of and rs_of(k):
            row["rs"] = rs_of(k)

    li, lo = pair("CB-02", "toggle", buf, guard, count=count, hook=hook, pid=pid, tok=tok, period=LONG)
    for i, k in enumerate(explicit_at):
        li.event(T_START + k * LONG, "monitor_toggle", i % 2)
    return li, lo


def alternating(sw, skew=0, kinds=2):
    """线程在 THREADS 两个值之间来回，每次切换换一次；skew 是线程比切换点晚几块变。kinds=3 轮流用三个线程（三种处理形态）。"""
    pool = (*THREADS, THREAD3)[:kinds]
    return lambda k: pool[sum(1 for x in sw if x + skew <= k) % kinds]


def scen_a2_thread(state, pid=4100, switches=10, explicit=False, skew=0):
    """宿主不通知监听键，日志里没有 monitor_toggle：切换点只能从 In 端 B 行 thread 的变化推断。
    state：pass / fail（第 3 次切换后重同步两次）：线程每 3 s 换一次，切换块记一次重同步；
    incomplete（thread 一直不变，只有块长 n 来回抖，抖动不算切换）；
    flap（thread 每 3 块换一次，一共几十次，全挤在 2 s 之内：不是用户在按）。
    explicit=True 另外写出显式事件，线程比事件晚 skew 块变：同一次切换不能数两遍。"""
    if state in ("pass", "fail"):
        sw = switch_blocks(switches)

        def rs_of(k):
            at = k - skew
            return (2 if state == "fail" and at == sw[2] else 1) if at in sw else 0

        li, lo = a2_long(pid, alternating(sw, skew), count=sw[-1] + 20, rs_of=rs_of, explicit_at=sw if explicit else ())
        return files_of(li, lo)

    def hook(r, k, row):
        if r != "I":
            return
        if state == "incomplete":
            row["n"] = 128 if k % 2 else 256
        else:  # flap
            row["thread"] = THREADS[(k // 3) % 2]

    li, lo = pair("CB-02", "toggle", 256, "low", count=120, hook=hook, pid=pid)
    return files_of(li, lo)


def scen_a2_pool(pid, threads, every=23, switches=13):
    """用户一次都没按：宿主的线程池每 every 块把这条轨交给下一个线程。三个线程轮转、4.6 s 一次、13 次。"""
    count = every * switches + 20
    li, lo = a2_long(pid, lambda k: threads[(k // every) % len(threads)], count=count)
    return files_of(li, lo)


def scen_a2_split(pid, spurious=True):
    """12 次切换，每 3 s 一次。第 3 次切换时重同步两次（切换块一次，0.8 s 后又一次）：每次切换后 2 s 内最多 1 次的判据应当不通过。
    spurious=True 在第 3 次切换后 0.6 s 又让宿主把线程换到第三个值（监听状态没变，只是线程池调度）：推断出来的下一次切换
    不能把第 3 次的窗口截成两段，让这两次重同步各占一段而都不超限。"""
    sw = switch_blocks(12)
    base = alternating(sw)

    def thread_of(k):
        return THREAD3 if spurious and sw[2] + 3 <= k < sw[3] else base(k)

    def rs_of(k):
        return 1 if k in sw or k == sw[2] + 4 else 0

    return files_of(*a2_long(pid, thread_of, count=sw[-1] + 20, rs_of=rs_of, buf=256, guard="normal"))


def scen_a2_presync(pid):
    """重同步落在旧线程的最后一块上，加上切换块自己的一次，两次紧挨着：窗口从切换块之前的一块算起才数得到。"""
    sw = switch_blocks(12)
    return files_of(*a2_long(pid, alternating(sw), count=sw[-1] + 20, rs_of=lambda k: 1 if k in sw or k == sw[2] - 1 else 0))


def scen_a2_mixed(pid):
    """12 次切换：前 6 次有显式事件，后 6 次只有线程变化；第 9 次（只有线程变化的那几次之一）切换后重同步两次。
    有显式事件的格不能只看显式的那几次切换：推断出来的切换点照样要判。"""
    sw = switch_blocks(12)
    return files_of(*a2_long(pid, alternating(sw), count=sw[-1] + 20, rs_of=lambda k: 2 if k == sw[8] else (1 if k in sw else 0), explicit_at=sw[:6]))


def scen_a2_tail(pid):
    """10 次切换，最后一次离记录结束只剩 4 块：这一次的窗口不够 8 块。"""
    sw = switch_blocks(10)
    return files_of(*a2_long(pid, alternating(sw), count=sw[-1] + 4, rs_of=lambda k: 1 if k in sw else 0))


POOL = ("11aa11aa", "22bb22bb", "33cc33cc", "44dd44dd", "55ee55ee", "66ff66ff")  # 宿主线程池里的线程（预取，几乎每块换一个）
RT_THREAD = "9a8b7c6d"  # 实时时用的专用线程
DUO = ("4a4a4a4a", "5b5b5b5b")  # 第三种形态：两个线程逐块轮换，哪个都占不满迟滞
STRAY_THREAD = "7e7e7e7e"  # 实时切回预取之前，有几块跑在这个线程上


def regime_blocks(phases, *, tail=5, cadence=LONG):
    """按处理形态排的 In 端块序列。phases = [("pool" | "rt" | "duo", 秒数)]：
    pool = 预取：块长 512，线程池里每块换一个线程；rt = 实时：块长 256，同一个专用线程；duo = 块长 256，在 DUO 两个线程之间逐块轮换。
    实时切回预取时，预取的第一块之前先有 tail 块跑在另一个线程上、块间隔 0.1 ms（现场见过：不到 1 ms 里换了一下线程再进池）。
    → [{t: 相对 T_START 的偏移 ns, thread, n, first: 是不是新形态的第一块}]"""
    out, t, k, prev = [], 0, 0, None
    for kind, secs in phases:
        start = len(out)
        if kind == "pool" and prev == "rt":
            for j in range(tail):
                out.append(dict(t=t - (tail - j) * 100_000, thread=STRAY_THREAD, n=256))
        for _ in range(round(secs * 1_000_000_000 / cadence)):
            thread = {"rt": RT_THREAD, "duo": DUO[k % 2], "pool": POOL[k % len(POOL)]}[kind]
            out.append(dict(t=t, thread=thread, n=512 if kind == "pool" else 256))
            t += cadence
            k += 1
        out[start]["first"] = prev is not None
        prev = kind
    return out


def scen_a2_regimes(pid, phases, *, tail=5, explicit=False, host_back=None):
    """CB-02 toggle 格：处理形态在线程池预取与专用线程实时之间来回（按键切换监听时 ASIO-Guard 把轨在两者之间挪）。
    每次切换的第一块记一次重同步。explicit=True 另外在每次切换处写显式事件。
    host_back = (相对 T_START 的偏移 ns, 回退量 ns)：从这一偏移起两端的 hostNs 整体回退这么多（ASIO-Guard 切换时的现场现象）。"""
    sched = regime_blocks(phases, tail=tail)

    def hook(r, k, row):
        b = sched[k]
        back = host_back[1] if host_back and b["t"] >= host_back[0] else 0
        row.update(mono_ns=T_START + b["t"] + (1000 if r == "O" else 0), host_ns=HOST0 + b["t"] - back, n=b["n"], n_decl=b["n"])
        if r == "I":
            row.update(thread=b["thread"], rs=1 if b.get("first") or k == 0 else 0)

    li, lo = pair("CB-02", "toggle", 256, "normal", count=len(sched), hook=hook, pid=pid, period=LONG, prepare_max=512)
    if explicit:
        for b in sched:
            if b.get("first"):
                li.event(T_START + b["t"], "monitor_toggle", 1)
    return files_of(li, lo)


REAL_LIKE = [(kind, 3.0) for kind in ("pool", "rt") * 6 + ("pool",)]  # 13 段 = 12 次切换
THREE_KINDS = [(kind, 3.0) for kind in ("rt", "duo", "rt", "pool") * 3 + ("rt",)]  # 专用线程、两线程轮换、线程池三种形态，13 段 = 12 次切换


def scen_a2_stray_between(pid):
    """两个专用线程轮着来（每 3 s 换一次，10 次切换），每次切换处新旧形态之间夹着 2 块跑在第三个线程上的杂散段（0.2 s，短于迟滞，被丢弃）。
    第 3 次切换的杂散段第一块上有一次重同步，新形态第一块（切换块）上又有一次。切换时刻取新形态第一块、窗口从它之前的一块（杂散段末块）算起，
    杂散段第一块上的那次落在窗口之外，这次切换后只有 1 次；若把杂散段第一块当切换时刻，窗口把两次都装进去，会判成 2 次。"""
    sw = switch_blocks(10)
    base = alternating(sw)

    def thread_of(k):
        return STRAY_THREAD if any(s - 2 <= k < s for s in sw) else base(k)

    return files_of(*a2_long(pid, thread_of, count=sw[-1] + 20, rs_of=lambda k: 1 if k in sw or k == sw[2] - 2 else 0))


def scen_a2_blip(pid, blocks):
    """一直在同一个专用线程上，只有中间 blocks 块跑去了另一个线程再回来（0.2 s 一块）：短于迟滞的不算形态变化。"""
    return files_of(*a2_long(pid, lambda k: THREADS[1] if 70 <= k < 70 + blocks else THREADS[0], count=150))


def scen_a2_explicit_presync(pid):
    """显式切换事件：每次切换块记一次重同步；第 3 次切换之前的那一块（旧状态的最后一块）又记一次。
    窗口从事件之前的最后一块算起时，这一次切换后有 2 次重同步；从事件时刻算起就漏掉前一块。线程不变，没有推断出来的切换。"""
    sw = switch_blocks(10)
    return files_of(*a2_long(pid, lambda k: THREADS[0], count=sw[-1] + 20, rs_of=lambda k: 1 if k in sw or k == sw[2] - 1 else 0, explicit_at=sw))


def scen_a2_unsorted(pid):
    """In 端的块在文件里按 seq 排，但 mono_ns 不一定同序（探针在不同线程取时间戳）：每次切换处的前后两块互换了时间戳。
    窗口按 mono_ns 排好序再取，换序不影响每次切换后重同步的计数。线程不变，切换点用显式事件。
    每次切换前还有一块早 2 块的重同步（文件里第 s-2 块）：它按时间排在窗口起点之前，不算这一次；
    不排序直接按文件顺序取窗口时，窗口起点会错落到它上面，这一格就会被判成每次切换 2 次重同步。"""
    sw = switch_blocks(10)

    def hook(r, k, row):
        if r != "I":
            return
        for s in sw:
            if k == s - 1:
                row["mono_ns"] = T_START + s * LONG
            elif k == s:
                row["mono_ns"] = T_START + (s - 1) * LONG
        if k in sw or k + 2 in sw:
            row["rs"] = 1

    li, lo = pair("CB-02", "toggle", 256, "low", count=sw[-1] + 20, hook=hook, pid=pid, period=LONG)
    for i, k in enumerate(sw):
        li.event(T_START + k * LONG, "monitor_toggle", i % 2)
    return files_of(li, lo)


def stall_off(stall, *, base=480, step=256, jump=100):
    """Out 端 off 的起播停顿：头 stall 块里宿主时间线不走，off 每块涨一个块长；停顿结束那块时间线只走了一部分
    （off 只涨 jump，比块长少），之后 off 恒定。返回 off_of(k)。"""

    def off_of(k):
        return base + step * k if k < stall else base + step * (stall - 1) + jump

    return off_of


def scen_a1_out(pid, off_of, *, role="O", count=40, pause=None):
    """CB-01 steady 格；role 这一端的 off 由 off_of(k) 给，其余照常。pause = (起, 止) 块序号区间里 playing = 0（停播）。"""

    def hook(r, k, row):
        if r == role:
            row["off"] = off_of(k)
        if pause and pause[0] <= k < pause[1]:
            row["playing"] = 0

    return files_of(*pair("CB-01", "steady", 256, "low", count=count, hook=hook, pid=pid))


def two_runs_off(*, stall=3, second_settled=None, resume=25):
    """两段播放：第一段按 stall_off(stall) 停顿后落在 settled；第二段从 resume 起，停顿 2 块后落在 second_settled（默认同 settled）。"""
    first = stall_off(stall)
    settled = first(stall)
    second = settled if second_settled is None else second_settled

    def off_of(k):
        if k < resume:
            return first(k)
        j = k - resume
        return second - 256 * (2 - j) if j < 2 else second

    return off_of


def _drop_at_range(k0, k1, role="I"):
    def hook(r, k, row):
        if r == role and k0 <= k < k1:
            row["_drop"] = True

    return hook


def scen_a7_out_opens_first(pid, extra=5, count=40):
    """Out 先开窗：Out 比 In 多出头 extra 块（In 的前 extra 块没拿到绑定）。两端的 call_seq 差仍是 Out = In + 1。"""
    return files_of(*pair("CB-01", "steady", 256, "low", count=count + extra, hook=_drop_at_range(0, extra), pid=pid))


def scen_a7_toggle_regress(pid, where, switches=10, at=3, double_resync=False, regimes=2):
    """CB-02 toggle 格（0.2 s 一块，线程每 3 s 换一次）。hostNs 在第 at + 1 次切换处（at-switch）或切换之后 1.6 s 处（away）整体回退 7.3 s，
    两端同样回退。explicit：线程不变，切换点来自显式事件，回退在第 at + 1 次切换处。switches 是切换次数，默认 10，at 默认 3（第 4 次）。
    double_resync：第 at + 1 次切换块上重同步 2 次（A2 不通过）。regimes：线程轮流用几个（2 = 预取与实时，3 = 三种处理形态）。
    where="none"：hostNs 不回退。"""
    sw = switch_blocks(switches)
    k0 = 10**9 if where == "none" else sw[at] + (8 if where == "away" else 0)

    def host_of(k):
        return HOST0 + k * LONG - (7_300_000_000 if k >= k0 else 0)

    thread = (lambda k: THREADS[0]) if where == "explicit" else alternating(sw, kinds=regimes)
    return files_of(*a2_long(pid, thread, count=sw[-1] + 20, rs_of=lambda k: (2 if double_resync and k == sw[at] else 1) if k in sw else 0,
                             explicit_at=sw if where == "explicit" else (), host_of=host_of))  # fmt: skip


def scen_a7_steady_regress(pid):
    """CB-01 steady 格里有一个多余的监听切换事件，hostNs 恰好在事件处回退：非 toggle 格没有切换点，回退照常算非递增。"""

    def hook(r, k, row):
        if k >= 20:
            row["host_ns"] -= 4_000_000_000

    li, lo = pair("CB-01", "steady", 256, "low", hook=hook, pid=pid)
    li.event(T_START + 20 * PERIOD, "monitor_toggle", 1)
    return files_of(li, lo)


def scen_a8_crashed(pid, what):
    """CB-11 的一对实例，Out 端没有收尾（整机崩溃把日志截断）。what：stall = 另外摘要间隔 7 s；trips = 另外摘要里记了跳闸。"""
    li, lo = pair("CB-11", "constrain", 256, "low", pid=pid)
    lo.lines = [ln for ln in lo.lines if not ln.startswith("Z,")]
    sums = [k for k, ln in enumerate(lo.lines) if ln.startswith("S,")]
    f = lo.lines[sums[-1]].split(",")
    if what == "stall":
        f[1] = str(T_START + 8_000_000_000)
    elif what == "trips":
        f[6] = "2"
    lo.lines[sums[-1]] = ",".join(f)
    return files_of(li, lo)


def scen_a8_partial_tail(pid, tail):
    """CB-11 的一对实例，Out 端没有收尾，文件末尾是整机崩溃时一次写盘只落下前半截留下的一行（探针一批行一次 write，不是逐行落盘），没有换行结尾。
    tail：summary = 最后一条 S 行写到一半；block = 最后一个 B 行写到一半（摘要都排在它前面，所以摘要行数够判卡死）；
    nonl = 最后一行整行都写完了，只缺末尾的换行（也算残缺：分不清它是不是写完了）。"""
    li, lo = pair("CB-11", "constrain", 256, "low", pid=pid)
    lo.lines = [ln for ln in lo.lines if not ln.startswith("Z,")]
    if tail == "block":
        k = max(i for i, ln in enumerate(lo.lines) if ln.startswith("B,"))
        lo.lines = lo.lines[:k] + [ln for ln in lo.lines[k + 1 :] if ln.startswith("S,")] + [lo.lines[k][:23]]
    elif tail == "summary":
        lo.lines[-1] = lo.lines[-1][:14]
    return {**files_of(li), lo.name: "\n".join(lo.lines)}  # 末尾不补换行


def scen_a8_trips_elsewhere(pid):
    """CB-11 的一对实例，Out 端最后一条摘要里记了跳闸 2 次（A8 不通过）。别的段、别的实例的失败不能记到被排除的段名下。"""
    li, lo = pair("CB-11", "constrain", 256, "low", pid=pid)
    sums = [k for k, ln in enumerate(lo.lines) if ln.startswith("S,")]
    f = lo.lines[sums[-1]].split(",")
    f[6] = "2"
    lo.lines[sums[-1]] = ",".join(f)
    return files_of(li, lo)


def others_fail_excluded_clean(pid):
    """一对实例连续记三格，中间一格（面板默认 Case 误记，只有 5 块）是干净的，用来被 --exclude 排除；
    此外有别的实例让 A3（导出比对不过）、A5（两端不在同一进程）、A8（摘要里记了跳闸）、A9（重开后没有配对）各自不通过。
    这些失败都不属于被排除的段：excluded_fail 里一项都不该有。"""
    good, stray, good3 = ("CB-01", "steady", 64, "off", 40, "ok"), ("CB-01", "steady", "-", "na", 5, "ok"), ("CB-01", "steady", 1024, "high", 40, "ok")
    return {
        **files_of(*multi_cells([good, stray, good3], pid=pid)),
        **scen_a3("fail"),
        **scen_a5("fail", pid=pid + 10),
        **scen_a8_trips_elsewhere(pid + 1),
        **scen_a9("fail", pid=pid + 20),
    }


def toggle_excluded_beside_main(pid, double_resync=False, regimes=2, where="at-switch"):
    """CB-02 toggle 格录了两遍（同一个键）：第一遍（pid）10 次切换、hostNs 没有回退，是要留下的；第二遍（pid + 1）是误记的一遍，
    只有 3 次切换，第 2 次切换处两端的 hostNs 整体回退 7.3 s（ASIO-Guard 的现场现象），要被 --exclude 排除。
    --toggles CB-02:10 是整个 case 的现场按键次数，对得上留下的那一遍；被排除的那一遍自己只数出 3 次，对不上这个次数（也不足 10 次）。
    被排除的段单独判时，A7 的容忍要沿用主会话的确认，不能拿它自己的 3 次去对整场的 10 次。
    double_resync：被排除的那一遍第 2 次切换块上重同步 2 次，A2 若计入就不通过（逐格的判定，与现场次数无关）。
    regimes：被排除的那一遍用几种处理形态（线程轮流用几个）：处理形态数是逐段的性质，留下的那一遍只有 2 种，不能替被排除的 3 种背书。
    where：被排除的那一遍 hostNs 回退的位置（none = 不回退）。"""
    return {**scen_a2_thread("pass", pid=pid), **scen_a7_toggle_regress(pid + 1, where, switches=3, at=1, double_resync=double_resync, regimes=regimes)}


def disable_track_replaced(pid, *, variant="disable-track", new_pid=None, set_state_after=7_500_000_000, pair_after=300_000_000,
                           pair_new=True, born_before=False, roles="IO"):  # fmt: skip
    """CB-05 停用格：旧实例在本段末尾 get_state、release、destruct（Disable Track 销毁实例），
    随后同 pid 同角色的新实例（新日志文件、没有 C 行）set_state、prepare、pair_after 之后 paired。
    set_state_after 是新实例 set_state 离旧 Out 实例 destruct 的间隔；roles 里没有的那一端不建新实例；born_before：新实例在旧实例析构之前就出生了。"""
    li, lo = cb05_cell(variant, pid=pid, gap_at=None, events=[(1_850_000_000, "release", ""), (1_850_100_000, "destruct", "")])
    anchor = T_START + 39 * PERIOD + 1000
    li.event(anchor + 1_849_000_000, "get_state")
    li.event(anchor + 1_849_100_000, "release")
    li.event(anchor + 1_849_200_000, "destruct")
    t_d = anchor + 1_850_100_000
    logs = [li, lo]
    for role in roles:
        nl = Log(role, pid=new_pid or pid, tok="5e01" if role == "I" else "5e02")
        t_set = t_d + set_state_after
        nl.event(t_d - 1_000_000_000 if born_before else t_set - 1000, "prepare", 48000, 256)
        nl.event(t_set, "set_state", "A")
        if pair_new:
            nl.event(t_set + pair_after, "paired", pid, "A")
        for s in range(1, 4):
            nl.summary(t_set + s * 1_000_000_000)
        nl.close(t_set + 3_100_000_000)
        logs.append(nl)
    return files_of(*logs)


def scen_a3(state, pid=4100):
    if state == "pass":
        rows = ["CB-04,export-offline,0,-132.5,-98.0,0"]
    elif state == "fail":
        rows = ["CB-04,export-offline,0,-60.2,-40.0,0"]
    else:
        rows = ["CB-04,freeze,0,-140.0,-120.0,0"]  # 缺导出比对这一格
    return {"compare-1.csv": compare_text(*rows)}


def scen_a4(state, pid=4100):
    def hook(r, k, row):
        row["n"] = 64 * (1 + k % 4)
        if r == "I" and k == 30 and state == "fail":
            row["n"] = row["n_decl"] = 512  # prepare 声明的最大块长是 256
    li, lo = pair("CB-01", "steady", 256, "off", hook=hook, pid=pid)
    if state == "incomplete":  # 没有 prepare 事件
        for lg in (li, lo):
            lg.lines = [ln for ln in lg.lines if not ln.startswith("E,")]
    return files_of(li, lo)


def scen_a5(state, pid=7000, early=False, early_denied=False):
    combos = [("au", "au"), ("vst3", "vst3"), ("au", "vst3")]
    if state == "incomplete":
        combos = combos[:2]
    logs = []
    for i, (fi, fo) in enumerate(combos):
        p = pid + i
        li, lo = pair("LU-02", f"{fi}-{fo}", host_kind="luna", os_name="mac", fmt=(fi, fo), pid=p,
                      tok=(f"b{i}01", f"b{i}02"), blocks=False)  # fmt: skip
        if state == "fail" and i == 1:  # 两端不在同一进程
            lo.lines[1] = lo.lines[1].replace(f",{p},", f",{p + 50},")
            lo.pid = p + 50
        t_ev = T_START - 300 if early else T_START + 10  # 真实顺序：插件加载时就配对，之后才选 case
        li.event(t_ev, "backend", "posix", 0)
        lo.event(t_ev, "backend", "posix", 0)
        li.event(t_ev + 10, "paired", lo.pid, "A")
        if early_denied and i == 1:
            lo.event(T_START - 400, "shm_denied")
        logs += [li, lo]
    return files_of(*logs)


def scen_a6(state, pid=4100):
    li, lo = pair("LU-06", "rename", host_kind="luna", os_name="mac", blocks=False, pid=pid)
    if state == "pass":
        li.event(T_START + 5, "track_props", "Track%201", "ff8800")
        lo.event(T_START + 5, "track_props", "Track%201", "ff8800")
    elif state == "fail":
        li.event(T_START + 5, "track_props", "Track%201", "ff8800")
        lo.event(T_START + 5, "track_props", "", "")  # Out 端没收到
    return files_of(li, lo)


def scen_a7(state, pid=4100):
    if state == "pass":
        hook = None
    elif state == "fail":
        def hook(r, k, row):
            if r == "O" and k >= 10:
                row["host_ns"] += 777
    else:
        hook = _drop_at(8, "O")
    return files_of(*pair("CB-03", "guard-out", 256, "low", hook=hook, pid=pid))


def scen_a8(state, pid=4100):
    li, lo = pair("CB-11", "constrain", 256, "low", pid=pid)
    if state == "fail":
        lo.lines = [ln for ln in lo.lines if not ln.startswith("Z,")]  # 没有正常收尾
    elif state == "incomplete":
        lo.lines = [ln for ln in lo.lines if not ln.startswith("S,")]  # 收了尾但没有 1 Hz 摘要
    return files_of(li, lo)


def scen_a9(state, pid=4100, early=False):
    variant = "dup" if state == "incomplete" else "reopen"  # 没做保存重开这一格
    li, lo = pair("CB-07", variant, blocks=False, pid=pid)
    t_set, t_pair = (T_START - 300, T_START - 250) if early else (T_START + 30, T_START + 60)
    if state != "incomplete":
        li.event(t_set, "set_state", "A")
        lo.event(t_set, "set_state", "A")
    if state == "pass":
        li.event(t_pair, "paired", lo.pid, "A")
    return files_of(li, lo)


SCENARIOS = {
    "A1": scen_a1, "A2": scen_a2, "A3": scen_a3, "A4": scen_a4, "A5": scen_a5,
    "A6": scen_a6, "A7": scen_a7, "A8": scen_a8, "A9": scen_a9,
}  # fmt: skip


STOP_GAP = 5_000_000_000  # 清单让用户把插件停用 5 s


def cb05_cell(variant, *, pid, events=(), pre_events=(), gap_at=20, gap_ns=STOP_GAP, count=40, blocks=True,
              tok=("c0a1", "c0a2")):  # fmt: skip
    """CB-05 停用格。Out 端在第 gap_at 块之后断开 gap_ns（宿主停用了它），In 端一直在跑。
    events = [(相对断开前 Out 最后一块的偏移 ns, 事件名, a)]，写在 Out 端的本段内；gap_at=None 时没有断开，
    偏移相对 Out 的最后一块。pre_events 在第一个 C 行之前（实例级的事件）。"""

    def hook(r, k, row):
        if r == "O" and gap_at is not None and k >= gap_at:
            row["mono_ns"] += gap_ns
            row["host_ns"] += gap_ns

    li, lo = pair("CB-05", variant, pid=pid, tok=tok, hook=hook, count=count, blocks=blocks)
    anchor = T_START + ((gap_at if gap_at is not None else count) - 1) * PERIOD + 1000
    for off, name, a in events:
        lo.event(anchor + off, name, a)
    for name, a in pre_events:
        lo.event(T_START - 400, name, a)
    return li, lo


def cb05_single_end(event_at, *, pid, name="set_processing", a=0, variant="deactivate"):
    """Out 整段没有块（一直停用），In 慢节奏跑 4 s；Out 的停用事件在 event_at（相对第一块）。"""
    li, lo = pair("CB-05", variant, pid=pid, tok=("c0a1", "c0a2"), period=100_000_000)
    lo.lines = [ln for ln in lo.lines if not ln.startswith("B,")]
    lo.event(T_START + event_at, name, a)
    return li, lo


def full_pass(skip_cell=None, cb05_blocks=True, skip_rip=False, deact="stop", a2=None, replaced=False):
    """所有判据都通过，且 F1 要求的七个 case 与两张矩阵都齐。每个场景用自己的 pid，文件名不冲突。
    deact：CB-05 停用与禁用轨道两格怎么做 stop = Out 断开 5 s、事件在断开处；play-reset = 没有停用，只有起播的 reset；
    teardown = 没有停用，只有析构写的 release。a2：换掉 CB-02 的文件（dict），None 用显式事件的 scen_a2。
    replaced=True：禁用轨道这一格的证据改成「旧实例析构、同 pid 新实例 set_state 并配对」。"""
    out = {}
    cb01 = [c for c in _matrix("CB-01") if c != skip_cell]
    out.update(files_of(*multi_pair(cb01, pid=4100)))
    out.update(files_of(*multi_pair(_matrix("LU-03"), pid=4300, host_kind="luna", os_name="mac")))
    for n, item in enumerate(("A2", "A6", "A9"), start=1):
        if item == "A2" and a2 is not None:
            out.update(a2)
            continue
        out.update(SCENARIOS[item]("pass", pid=4100 + n))
    out.update(scen_a5("pass", pid=7000))
    out["compare-1.csv"] = compare_text("CB-04,export-offline,1,-inf,-inf,0", "LU-04,export-offline,0,-128.0,-99.5,0")

    def bypass(r, k, row):
        row["bypassed"] = 1 if k >= 20 else 0

    for n, v in enumerate(("bypass-out", "bypass-in")):
        out.update(files_of(*pair("CB-05", v, hook=bypass, pid=4200 + n, tok=(f"b{n}a1", f"b{n}a2"), blocks=cb05_blocks)))
    for n, v in enumerate(("deactivate", "disable-track")):
        kw = dict(pid=4210 + n, tok=(f"c{n}a1", f"c{n}a2"), blocks=cb05_blocks)
        if replaced and v == "disable-track":
            out.update(disable_track_replaced(4211))
            continue
        if deact == "stop":
            li, lo = cb05_cell(v, events=[(10_000, "set_processing", 0)], **kw)
        elif deact == "play-reset":  # 起播时 setProcessing(false) 转成的 reset，块流没有断
            li, lo = cb05_cell(v, gap_at=None, events=[(-(39 * PERIOD), "reset", "")], **kw)
        else:  # 析构：块停下来 1.85 s 之后 release、destruct，之后才是 Z
            li, lo = cb05_cell(v, gap_at=None, events=[(1_850_000_000, "release", ""), (1_850_100_000, "destruct", "")], **kw)
        out.update(files_of(li, lo))
    out.update(files_of(*pair("CB-04", "freeze", pid=4220, tok=("e0a1", "e0a2"))))
    if not skip_rip:
        out.update(files_of(*pair("CB-04", "rip", pid=4221, tok=("e1a1", "e1a2"))))
    return out


def _matrix(case):
    """F1 要求的矩阵：CB-01 是 3 档缓冲乘 4 档 ASIO-Guard，LU-03 是 3 档缓冲。"""
    if case == "CB-01":
        return [("CB-01", "steady", b, g) for b in (64, 256, 1024) for g in ("off", "low", "normal", "high")]
    return [("LU-03", "steady", b, "na") for b in (64, 512, 2048)]


def obs_fixtures():
    """格观察的数字：正常值一组、异常值一组。"""

    def byp(r, k, row):
        row["bypassed"] = 1 if k >= 20 else 0

    def out_first(r, k, row):
        row["_cs"] = 2 * k + 1 if r == "I" else 2 * k

    def some_out_first(r, k, row):
        if k % 4 == 0:
            out_first(r, k, row)

    def w_bad(r, k, row):
        byp(r, k, row)
        if r == "O":
            row["w"] = 4800 + (24 if k in (25, 26, 27) else k) * 256  # 停滞三次
            if k == 30:
                row["w"] = 100  # 回退一次

    def late(r, k, row):
        if k >= 20:
            row["mono_ns"] += 1_200_000_000  # 回调中间断了 1.2 s

    steady = pair("CB-01", "steady", 256, "low", pid=4900)
    def byp_zero(r, k, row):
        byp(r, k, row)
        if r == "O":  # 第 10 块是 0 样本回调：不推进写头，之后的 w 整体少前进一块
            row["n"] = 0 if k == 10 else 256
            row["w"] = 4800 + (k - (1 if k >= 11 else 0)) * 256

    bypass = pair("CB-05", "bypass-out", hook=byp_zero, pid=4901)
    smart = pair("CB-06", "smart", pid=4902)
    for t in (1000, 2000):
        smart[0].event(T_START + t, "ping", 9600)
    mis = pair("CB-08", "misplaced-order", pid=4903, hook=out_first)
    normal = files_of(*steady, *bypass, *smart, *mis)
    steady2 = pair("CB-01", "steady", 256, "low", pid=4910, hook=some_out_first)
    bypass2 = pair("CB-05", "bypass-in", hook=w_bad, pid=4911)
    smart2 = pair("CB-06", "smart", pid=4912, hook=late)
    smart2[0].event(T_START + 1000, "ping", 9600)
    mis2 = pair("CB-08", "misplaced-post", pid=4913)[0]  # 只有 In 一端
    abnormal = files_of(*steady2, *bypass2, *smart2, mis2)
    return {"obs-normal": (normal, {}), "obs-abnormal": (abnormal, {})}


def short_with_c(pid):
    """有 C 行、活不到 2 秒、也没有 1 Hz 摘要的 Out 实例：克隆豁免只给没有 C 行的实例，这个仍是数据不全。"""
    li, lo = pair("CB-11", "constrain", 256, "low", pid=pid, blocks=False)
    lo.lines = [ln for ln in lo.lines if not ln.startswith(("S,", "Z,"))]
    lo.lines.append(f"Z,{T_START + 500_000_000},0,0")
    return li, lo


def extra_cells():
    """补充的边界夹具：{目录名: (文件, {判据: 期望})}。"""

    def bufchange(r, k, row):
        row["n"] = 256 if k < 20 else 1024
        row["n_decl"] = row["n"]

    li, lo = pair("CB-10", "bufchange", 256, "low", hook=bufchange, pid=4400)
    for lg in (li, lo):  # 播放中改缓冲：第 20 块之前重新 prepare 成 1024
        lg.event(T_START + 20 * PERIOD - 1000, "prepare", 48000, 1024)
    li2, lo2 = pair("CB-02", "toggle", 256, "low", blocks=False, pid=4500)
    for i in range(10):  # 有 10 次切换，却没有任何块记录
        li2.event(T_START + i * PERIOD, "monitor_toggle", i % 2)
    still = pair("CB-03", "guard-out", 256, "low", hook=lambda r, k, row: row.update(playing=0), pid=4600)
    def drift(r, k, row):
        if r == "I" and k >= 20:
            row["m"] = 3

    sp = pair("CB-01", "steady", 64, "off", hook=drift, pid=4700)
    sp[0].event(T_START + 5 * PERIOD, "monitor_toggle", 1)  # 多余的切换事件不能把失败的格排除掉
    only_in = pair("CB-01", "steady", 256, "low", pid=4710)[0]
    dup_a = pair("CB-01", "steady", 256, "low", pid=4720)
    dup_b = pair("CB-01", "steady", 256, "low", pid=4721, tok=("a9b8", "c7d6"))
    windows = pair("CB-02", "toggle", 256, "low", count=120, pid=4730)
    for i in range(10):  # 切换事件落在所有块之后：窗口里没有块
        windows[0].event(T_START + 50_000_000_000 + i * PERIOD, "monitor_toggle", i % 2)
    few = pair("CB-01", "steady", 256, "low", count=3, pid=4740)


    callseq = pair("CB-01", "steady", 256, "low", pid=4750)
    callseq[1].lines = [
        ln if not ln.startswith("B,") else ",".join(["B", ln.split(",")[1], str(int(ln.split(",")[2]) + 1000)] + ln.split(",")[3:])
        for ln in callseq[1].lines
    ]
    fewplaying = pair("CB-01", "steady", 256, "low", pid=4760, hook=lambda r, k, row: row.update(playing=1 if k == 0 else 0))
    name = "%E4%B8%AD" * 67  # 67 个汉字，编码前 201 字节
    long_i, long_o = pair("LU-06", "rename", host_kind="luna", os_name="mac", blocks=False, pid=4770)
    for lg in (long_i, long_o):
        lg.event(T_START + 5, "track_props", name, "ff8800")
    broken = scen_a8("pass", pid=4780)
    broken["probe-I-9999-dead.csv"] = ""  # 实例刚创建就崩溃：0 字节
    dead_a, dead_b = pair("CB-05", "deactivate", pid=4810, blocks=False)
    for lg in (dead_a, dead_b):
        lg.event(T_START + 10, "set_processing", 0)
    dup1 = pair("CB-07", "dup", blocks=False, pid=4820)
    dup2 = pair("CB-07", "dup", blocks=False, pid=4820, tok=("a9b8", "c7d6"))
    for li_, lo_ in (dup1, dup2):
        li_.event(T_START + 20, "paired", lo_.pid, "A")
    undup = pair("CB-07", "dup", blocks=False, pid=4830)
    def drift2(r, k, row):
        if r == "I" and k >= 20:
            row["m"] = 3

    tp1, tp2, tp3 = (pair("CB-12", "twoproj", blocks=False, pid=4860, tok=(f"d{n}01", f"d{n}02")) for n in range(3))
    tp_ok = []
    for li_, lo_ in (tp1, tp2):
        li_.event(T_START + 20, "paired", lo_.pid, "A")
        tp_ok += [li_, lo_]
    bad_dup = pair("CB-07", "dup", blocks=False, pid=4890, tok=("a9b8", "c7d6"))
    dup_good = pair("CB-07", "dup", blocks=False, pid=4890)
    dup_good[0].event(T_START + 20, "paired", dup_good[1].pid, "A")
    cat2_only = cb05_cell("deactivate", pid=4870, events=[(10_000, "set_processing", 0)])
    guard_fail = pair("CB-03", "guard-in", 256, "low", hook=drift2, pid=4872)
    names_a = pair("LU-06", "rename", host_kind="luna", os_name="mac", blocks=False, pid=4880)
    names_b = pair("LU-06", "rename", host_kind="luna", os_name="mac", blocks=False, pid=4881, tok=("a9b8", "c7d6"))
    for lg in names_a:
        lg.event(T_START + 5, "track_props", "Track%201", "ff8800")
    names_b[0].event(T_START + 5, "track_props", "Track%202", "ff8800")
    names_b[1].event(T_START + 5, "track_props", "", "")  # 第二个实例没收到，第一个实例的好数据不能掩盖它
    lone_out = pair("CB-12", "twoproj", blocks=False, pid=4860, tok=("d901", "d902"))[1]
    two_bypass = [pair("CB-05", "bypass-out", pid=4895, tok=(f"f{n}01", f"f{n}02"), hook=lambda r, k, row: row.update(bypassed=1 if k >= 20 else 0)) for n in range(2)]
    cat2_full = pair("CB-04", "export-offline", pid=4930)
    gap_a = pair("CB-05", "deactivate", pid=4931, hook=_drop_at(15))
    gap_a[1].event(T_START + 10, "set_processing", 0)
    one_cell_missing = [
        *pair("LU-06", "rename", host_kind="luna", os_name="mac", blocks=False, pid=4932),
        *pair("CB-07", "rename", blocks=False, pid=4933, tok=("a9b8", "c7d6")),
    ]
    for lg in one_cell_missing[:2]:
        lg.event(T_START + 5, "track_props", "Track%201", "ff8800")

    def stop(variant, events=(), pre=(), pid=4940, **kw):
        """CB-05 停用格（Out 端断开 5 s）：events 写在本段里，pre 写在第一个 C 行之前。"""
        return files_of(*cb05_cell(variant, pid=pid, events=events, pre_events=pre, **kw))

    reset_at = [(10_000, "reset", "")]  # 断开前最后一块之后 10 us
    play_reset = [(-(39 * PERIOD), "reset", "")]  # 起播时的 reset：在第一块上，块流没有断
    teardown = [(1_850_000_000, "release", ""), (1_850_100_000, "destruct", "")]  # 块停下 1.85 s 后析构

    pre_ping = pair("CB-06", "smart", pid=4950)
    pre_ping[0].event(T_START - 400, "ping", 9600)  # 第一个 C 行之前的 Ping（第 0 步留下的）
    pre_ping[0].event(T_START + 1000, "ping", 9600)  # 本段里只有这一条
    good = ("CB-01", "steady", 64, "off", 40, "ok")
    good2 = ("CB-01", "steady", 256, "low", 40, "ok")
    good3 = ("CB-01", "steady", 1024, "high", 40, "ok")
    stray = ("CB-01", "steady", "-", "na", 5, "ok")  # 面板默认 Case 误记的一格：只有几块
    stray_unbound = ("CB-01", "steady", "-", "na", 40, "unbound")
    return {
        "a2-incomplete-thread-unconfirmed": (scen_a2_thread("pass", pid=5000), {"A2": "INCOMPLETE"}),
        "a2-fail-thread-inferred": (scen_a2_thread("fail", pid=5001), {"A2": "FAIL"}),
        "a2-incomplete-thread-constant": (scen_a2_thread("incomplete", pid=5002), {"A2": "INCOMPLETE"}),
        "a2-incomplete-thread-few": (scen_a2_thread("pass", pid=5003, switches=3), {"A2": "INCOMPLETE"}),
        "a2-incomplete-thread-flap": (scen_a2_thread("flap", pid=5005), {"A2": "INCOMPLETE"}),
        "a2-incomplete-one-cell-no-switch": (  # 一格有 10 次切换，另一格一次都没有：后者不能被前者的次数盖住
            {**scen_a2_thread("pass", pid=5006), **files_of(*pair("CB-02", "toggle", 64, "low", count=120, pid=5007))},
            {"A2": "INCOMPLETE"},
        ),
        "a2-pass-explicit-and-thread": (scen_a2_thread("pass", pid=5004, explicit=True), {"A2": "PASS"}),
        "a2-pass-explicit-skewed-thread": (scen_a2_thread("pass", pid=5008, explicit=True, skew=2), {"A2": "PASS"}),
        "a2-incomplete-thread-pool3": (scen_a2_pool(5009, (*THREADS, THREAD3)), {"A2": "INCOMPLETE"}),
        "a2-fail-thread-split-window": (scen_a2_split(5030), {"A2": "FAIL"}),
        "a2-fail-thread-double-resync": (scen_a2_split(5031, spurious=False), {"A2": "FAIL"}),
        "a2-fail-thread-presync": (scen_a2_presync(5032), {"A2": "FAIL"}),
        "a2-fail-mixed-explicit-and-inferred": (scen_a2_mixed(5034), {"A2": "FAIL"}),
        "a2-incomplete-thread-tail-thin": (scen_a2_tail(5033), {"A2": "INCOMPLETE"}),
        "a2-incomplete-regimes-unconfirmed": (scen_a2_regimes(5070, REAL_LIKE), {"A2": "INCOMPLETE"}),  # 预取 / 实时来回 13 段：给 --toggles CB-02:12 才通过
        "a2-pass-regimes-explicit": (scen_a2_regimes(5071, REAL_LIKE, explicit=True), {"A2": "PASS"}),
        "a2-incomplete-regimes-three-kinds": (  # 专用线程、两线程轮换、线程池三种形态隔着专用线程轮着来：次数对得上也多于两种
            scen_a2_regimes(5075, THREE_KINDS),
            {"A2": "INCOMPLETE"},
        ),
        "a2-incomplete-pool-rotation-only": (scen_a2_regimes(5072, [("pool", 40.0)]), {"A2": "INCOMPLETE"}),
        "a2-incomplete-regime-blip-short": (scen_a2_blip(5073, 2), {"A2": "INCOMPLETE"}),
        "a2-incomplete-regime-blip-long": (scen_a2_blip(5074, 3), {"A2": "INCOMPLETE"}),
        "a2-fail-explicit-presync": (scen_a2_explicit_presync(5052), {"A2": "FAIL"}),
        "a2-pass-unsorted-timestamps": (scen_a2_unsorted(5053), {"A2": "PASS"}),
        "a1-pass-out-stall": (scen_a1_out(5060, stall_off(10)), {"A1": "PASS"}),
        "a1-pass-out-stall-jump-only": (scen_a1_out(5061, stall_off(1)), {"A1": "PASS"}),
        "a1-pass-out-two-runs": (scen_a1_out(5062, two_runs_off(), count=60, pause=(20, 25)), {"A1": "PASS"}),
        "a1-fail-out-stall-too-long": (scen_a1_out(5063, stall_off(30)), {"A1": "FAIL"}),
        "a1-fail-out-off-keeps-moving": (scen_a1_out(5064, lambda k: 480 + 3 * (k // 5)), {"A1": "FAIL"}),
        "a1-fail-out-off-shrinks": (scen_a1_out(5065, lambda k: 480 + 256 * k if k < 5 else 480 + 256 * 4 - 100), {"A1": "FAIL"}),
        "a1-fail-out-timeline-backward": (scen_a1_out(5066, lambda k: 480 if k < 5 else 836), {"A1": "FAIL"}),
        "a1-fail-in-stall": (scen_a1_out(5067, stall_off(10), role="I"), {"A1": "FAIL"}),
        "a1-fail-out-two-runs-different-off": (scen_a1_out(5068, two_runs_off(second_settled=1156), count=60, pause=(20, 25)), {"A1": "FAIL"}),
        "a1-incomplete-misrecorded-headdrop-drift": (  # 误记的一格头部丢块（缺口归给它）又有 drift：影子判定也只能算数据不全
            files_of(*multi_cells([good, ("CB-01", "steady", "-", "na", 40, "headdrop-drift"), good3], pid=5050)),
            {"A1": "INCOMPLETE"},
        ),
        "a1-incomplete-misrecorded-nosummary-drift": (  # 同上，但一条摘要都没有：缺口无从归属，被排除的一格也要记缺口，影子判定才不会把它当成可信的 drift
            files_of(*multi_cells([good, ("CB-01", "steady", "-", "na", 40, "headdrop-drift"), good3], pid=5054, summaries=False)),
            {"A1": "INCOMPLETE"},
        ),
        "a1-fail-misrecorded-drift-main-fail": (  # 未排除的格自己已经不通过，被排除的格也不通过：两件事都要记
            files_of(*multi_cells([good, ("CB-01", "steady", "-", "na", 40, "drift"), ("CB-01", "steady", 1024, "high", 40, "drift")], pid=5051)),
            {"A1": "FAIL"},
        ),
        "a7-pass-out-opens-first": (scen_a7_out_opens_first(5080), {"A7": "PASS"}),
        "a7-pass-out-first-reported": (
            files_of(*pair("CB-01", "steady", 256, "low", pid=5081, hook=lambda r, k, row: row.update(_cs=2 * k + 1 if r == "I" else 2 * k))),
            {"A7": "PASS"},
        ),
        "a7-fail-toggle-hostns-regress-unconfirmed": (scen_a7_toggle_regress(5090, "at-switch"), {"A7": "FAIL"}),  # 推断的切换点，没给 --toggles：不给容忍；给 CB-02:10 对上才容忍
        "a7-pass-toggle-hostns-regress-explicit": (scen_a7_toggle_regress(5091, "explicit"), {"A7": "PASS"}),
        "a7-fail-toggle-hostns-regress-away": (scen_a7_toggle_regress(5092, "away"), {"A7": "FAIL"}),
        "a7-fail-steady-hostns-regress-at-event": (scen_a7_steady_regress(5093), {"A7": "FAIL"}),
        "a7-fail-toggle-hostns-regress-three-regimes": (  # 三种形态、12 次，次数对得上也不确认：回退在第 4 次切换处，不给容忍
            scen_a2_regimes(5094, THREE_KINDS, host_back=(9_000_000_000, 7_300_000_000)),
            {"A7": "FAIL"},
        ),
        "a2-incomplete-stray-between-unconfirmed": (scen_a2_stray_between(5076), {"A2": "INCOMPLETE"}),  # 给 --toggles CB-02:10 才通过
        "exclude-clean-while-others-fail": (  # 被排除的一格干净；别的实例让 A3 A5 A8 A9 各自不通过
            others_fail_excluded_clean(5120),
            {"A3": "FAIL", "A5": "FAIL", "A8": "FAIL", "A9": "FAIL"},
        ),
        "a8-fail-partial-tail-summary": (scen_a8_partial_tail(5102, "summary"), {"A8": "FAIL"}),  # 末行写到一半：解析时丢掉，--crashed 照样能标注
        "a8-fail-partial-tail-block": (scen_a8_partial_tail(5103, "block"), {"A8": "FAIL"}),
        "a8-fail-partial-tail-nonl": (scen_a8_partial_tail(5104, "nonl"), {"A8": "FAIL"}),
        "a7-exclude-toggle-beside-main": (  # 不带 --exclude 时同键两遍：留下的与误记的各占一格，同键重复记录；带 --exclude 的判定见自测
            toggle_excluded_beside_main(5150),
            {"A2": "INCOMPLETE", "A7": "INCOMPLETE"},
        ),
        "a7-exclude-toggle-three-regimes": (  # 同上，但被排除的那一遍有 3 种处理形态：形态数是逐段的性质，不沿用主会话的确认
            toggle_excluded_beside_main(5150, regimes=3),
            {"A2": "INCOMPLETE", "A7": "INCOMPLETE"},
        ),
        "a1-incomplete-orphan-gaps-before-excluded": (  # 第一个 C 行之前有孤块、孤块之间缺块：不归任何格，也不是尾部丢块（排除误记的一格后 A1 仍是通过）
            files_of(*multi_cells([good, stray, good3], pid=5160, pre="ooxxooxo")),
            {"A1": "INCOMPLETE"},
        ),
        "a1-incomplete-orphan-gaps-and-taildrop": (  # 同上，最后一格自己另有 3 块尾部丢块：只有这 3 块归给它
            files_of(*multi_cells([good, stray, ("CB-01", "steady", 256, "low", 40, "taildrop")], pid=5161, pre="ooxxooxo")),
            {"A1": "INCOMPLETE"},
        ),
        "a1-incomplete-orphan-head-before-excluded": (  # 实例开头先丢了两块、之后才有孤块：开头缺的块也早于第一个 C 行，不归第一格
            files_of(*multi_cells([good, stray, good3], pid=5162, pre="xxoo")),
            {"A1": "INCOMPLETE"},
        ),
        "a1-pass-orphan-gaps-only": (  # 不带 --exclude：探针报告的丢弃全在孤块之间，已经定位，各格不因此按数据不全算
            files_of(*multi_cells([good, good3], pid=5163, pre="ooxxooxo")),
            {"A1": "PASS"},
        ),
        "a8-fail-crashed-stall": (scen_a8_crashed(5100, "stall"), {"A8": "FAIL"}),
        "a8-fail-crashed-trips": (scen_a8_crashed(5101, "trips"), {"A8": "FAIL"}),
        "cb05-disable-track-replaced": (disable_track_replaced(5110), {"OBS": "PASS"}),
        "cb05-disable-track-replaced-unpaired": (disable_track_replaced(5111, pair_new=False), {"OBS": "INCOMPLETE"}),
        "cb05-disable-track-replaced-timeout": (disable_track_replaced(5112, set_state_after=16_000_000_000), {"OBS": "INCOMPLETE"}),
        "cb05-disable-track-replaced-in-only": (disable_track_replaced(5113, roles="I"), {"OBS": "INCOMPLETE"}),
        "cb05-disable-track-replaced-other-pid": (disable_track_replaced(5114, new_pid=5115), {"OBS": "INCOMPLETE"}),
        "cb05-disable-track-replaced-born-earlier": (disable_track_replaced(5116, born_before=True), {"OBS": "INCOMPLETE"}),
        "cb05-disable-track-destruct-only": (disable_track_replaced(5117, roles=""), {"OBS": "INCOMPLETE"}),
        "cb05-deactivate-replaced-ignored": (disable_track_replaced(5118, variant="deactivate"), {"OBS": "INCOMPLETE"}),
        "cb05-deactivate-reset-only": (stop("deactivate", reset_at), {"OBS": "PASS"}),
        "cb05-deactivate-release-only": (stop("deactivate", [(10_000, "release", "")]), {"OBS": "PASS"}),
        "cb05-deactivate-set-processing-off": (stop("deactivate", [(10_000, "set_processing", 0)]), {"OBS": "PASS"}),
        "cb05-disable-track-reset-only": (stop("disable-track", reset_at), {"OBS": "PASS"}),
        "cb05-deactivate-reset-before-last-block": (stop("deactivate", [(-100_000_000, "reset", "")]), {"OBS": "PASS"}),
        "cb05-deactivate-set-processing-on": (stop("deactivate", [(10_000, "set_processing", 1)]), {"OBS": "INCOMPLETE"}),
        "cb05-deactivate-no-event": (stop("deactivate"), {"OBS": "INCOMPLETE"}),
        "cb05-deactivate-pre-release": (stop("deactivate", pre=[("release", "")], gap_at=1), {"OBS": "INCOMPLETE"}),
        "cb05-deactivate-pre-reset": (stop("deactivate", pre=[("reset", "")], gap_at=1), {"OBS": "INCOMPLETE"}),
        "cb05-deactivate-pre-set-processing": (stop("deactivate", pre=[("set_processing", 0)], gap_at=1), {"OBS": "INCOMPLETE"}),
        "cb05-deactivate-reset-at-play-only": (stop("deactivate", play_reset, gap_at=None), {"OBS": "INCOMPLETE"}),
        "cb05-deactivate-teardown-release-only": (stop("deactivate", teardown, gap_at=None), {"OBS": "INCOMPLETE"}),
        "cb05-deactivate-short-gap": (stop("deactivate", reset_at, gap_ns=600_000_000), {"OBS": "INCOMPLETE"}),
        "cb05-deactivate-event-late-in-gap": (stop("deactivate", [(3_750_000_000, "reset", "")]), {"OBS": "INCOMPLETE"}),
        "cb05-deactivate-single-end": (files_of(*cb05_single_end(2_000_000_000, pid=4800)), {"OBS": "PASS"}),
        "cb05-deactivate-single-end-at-start": (files_of(*cb05_single_end(10, pid=4801)), {"OBS": "INCOMPLETE"}),
        "cb05-deactivate-single-end-at-end": (files_of(*cb05_single_end(3_900_000_000, pid=4802)), {"OBS": "INCOMPLETE"}),
        "cb06-smart-pre-ping": (files_of(*pre_ping), {"OBS": "INCOMPLETE"}),
        "a8-pass-short-clones": (
            files_of(*pair("CB-11", "constrain", 256, "low", pid=5010), clone("I", 5011, "f0f1"), clone("O", 5011, "f0f2", 120_000_000)),
            {"A8": "PASS"},
        ),
        "a8-incomplete-long-no-c": (
            files_of(*pair("CB-11", "constrain", 256, "low", pid=5012), clone("I", 5013, "f0f3", 2_500_000_000)),
            {"A8": "INCOMPLETE"},
        ),
        "a8-incomplete-long-no-c-2200ms": (
            files_of(*pair("CB-11", "constrain", 256, "low", pid=5017), clone("I", 5018, "f0f5", 2_200_000_000)),
            {"A8": "INCOMPLETE"},
        ),
        "a8-pass-short-clone-1900ms": (
            files_of(*pair("CB-11", "constrain", 256, "low", pid=5019), clone("I", 5036, "f0f6", 1_900_000_000)),
            {"A8": "PASS"},
        ),
        "a8-fail-short-clone-crash": (
            files_of(*pair("CB-11", "constrain", 256, "low", pid=5014), clone("I", 5015, "f0f4", close=False)),
            {"A8": "FAIL"},
        ),
        "a8-incomplete-short-with-c": (files_of(*short_with_c(pid=5016)), {"A8": "INCOMPLETE"}),
        "a1-incomplete-misrecorded-mid": (files_of(*multi_cells([good, stray, good3], pid=5020)), {"A1": "INCOMPLETE"}),
        "a1-incomplete-misrecorded-unbound": (files_of(*multi_cells([good, stray_unbound, good2], pid=5021)), {"A1": "INCOMPLETE"}),
        "a1-incomplete-misrecorded-tail": (
            files_of(*multi_cells([good, ("CB-01", "steady", "-", "na", 5, "taildrop"), good3], pid=5025)),
            {"A1": "INCOMPLETE"},
        ),
        "a1-incomplete-misrecorded-trailing": (files_of(*multi_cells([good, good2, stray_unbound], pid=5022)), {"A1": "INCOMPLETE"}),
        "a1-incomplete-gap-beside-excluded": (
            files_of(*multi_cells([good, stray, ("CB-01", "steady", 1024, "high", 40, "gap:15")], pid=5023)),
            {"A1": "INCOMPLETE"},
        ),
        "a1-incomplete-unlocated-drop": (
            files_of(*multi_cells([good, ("CB-01", "steady", 256, "low", 40, "taildrop")], pid=5024)),
            {"A1": "INCOMPLETE"},
        ),
        "a1-fail-misrecorded-drift": (  # 误记的一格有 40 块，In 端从第 20 块起 m=3：排除它也拿不掉不通过
            files_of(*multi_cells([good, ("CB-01", "steady", "-", "na", 40, "drift"), good3], pid=5040)),
            {"A1": "FAIL"},
        ),
        "a1-incomplete-headdrop-after-excluded": (  # 误记格后面一格自己丢了第 0 块（两端）
            files_of(*multi_cells([good, stray, ("CB-01", "steady", 1024, "high", 40, "headdrop")], pid=5041)),
            {"A1": "INCOMPLETE"},
        ),
        "a1-incomplete-headdrop-in-after-excluded": (
            files_of(*multi_cells([good, stray, ("CB-01", "steady", 1024, "high", 40, "headdrop-in")], pid=5042)),
            {"A1": "INCOMPLETE"},
        ),
        "a1-incomplete-taildrop-after-unrelated-excluded": (  # 被排除的段没有丢弃，最后一个真格尾部丢块
            files_of(*multi_cells([stray, good, ("CB-01", "steady", 256, "low", 40, "taildrop")], pid=5043)),
            {"A1": "INCOMPLETE"},
        ),
        "a1-incomplete-taildrop-after-unbound-excluded": (  # 被排除的段整段没拿到绑定，后面的真格尾部另有丢块
            files_of(*multi_cells([good, stray_unbound, ("CB-01", "steady", 256, "low", 40, "taildrop")], pid=5044)),
            {"A1": "INCOMPLETE"},
        ),
        "a1-incomplete-two-excluded-taildrop": (  # 同一实例里两个被排除的段，丢块只归第二个，最后的真格尾部丢块不能被吞
            files_of(*multi_cells([stray, good, stray_unbound, ("CB-01", "steady", 256, "low", 40, "taildrop")], pid=5045)),
            {"A1": "INCOMPLETE"},
        ),
        "a4-fail-misrecorded-bign": (  # 误记的一格每块 n 都超过 prepare 声明的最大块长：排除它也拿不掉 A4 不通过
            files_of(*multi_cells([good, ("CB-01", "steady", "-", "na", 40, "bign"), good3], pid=5048)),
            {"A4": "FAIL"},
        ),
        "a7-fail-misrecorded-hostns": (  # 误记的一格两端 hostNs 对不上：排除它也拿不掉 A7 不通过
            files_of(*multi_cells([good, ("CB-01", "steady", "-", "na", 40, "hostns"), good3], pid=5049)),
            {"A7": "FAIL"},
        ),
        "a1-incomplete-misrecorded-nosummary": (  # 一条摘要都没有：缺失的块序号无从归属，各个未排除的段都记一次
            files_of(*multi_cells([good, stray_unbound, good2], pid=5047, summaries=False)),
            {"A1": "INCOMPLETE"},
        ),
        "a1-incomplete-two-excluded-ok": (  # 两个被排除的段各是一种误记（整段丢、几块），排除之后 A1 回到通过
            files_of(*multi_cells([stray_unbound, good, stray, good2], pid=5046)),
            {"A1": "INCOMPLETE"},
        ),
        "a7-incomplete-cat2-full": (files_of(*cat2_full), {"A7": "INCOMPLETE"}),
        "a4-incomplete-cat2-gap": (files_of(*gap_a), {"A4": "INCOMPLETE"}),
        "a6-incomplete-one-cell-missing": (files_of(*one_cell_missing), {"A6": "INCOMPLETE"}),
        "f1-cb04-no-rip": (full_pass(skip_rip=True), {}),
        "f1-cb05-reset-at-play-only": (full_pass(deact="play-reset"), {}),
        "f1-cb05-teardown-release-only": (full_pass(deact="teardown"), {}),
        "f1-cb05-disable-track-replaced": (full_pass(replaced=True), {}),
        "f1-a2-inferred": (full_pass(a2=scen_a2_thread("pass", pid=4101)), {}),
        "f1-a2-pool3": (full_pass(a2=scen_a2_pool(4101, (*THREADS, THREAD3))), {}),
        "cb12-twoproj-stray-out": (files_of(*tp_ok, lone_out), {"OBS": "INCOMPLETE"}),
        "f1-cb05-bypass-dup": (files_of(*two_bypass[0], *two_bypass[1]), {"OBS": "INCOMPLETE"}),
        "cb12-twoproj-two-pairs": (files_of(*tp_ok), {"OBS": "PASS"}),
        "cb12-twoproj-unpaired": (files_of(*tp_ok, *tp3), {"OBS": "INCOMPLETE"}),
        "cb07-dup-one-bad": (files_of(*dup_good, *bad_dup), {"OBS": "INCOMPLETE"}),
        "a4-incomplete-only-cat2": (files_of(*cat2_only), {"A4": "INCOMPLETE", "OBS": "PASS"}),
        "a1-fail-guard-variant": (files_of(*guard_fail), {"A1": "FAIL"}),
        "a6-fail-multi-instance": (files_of(*names_a, *names_b), {"A6": "FAIL"}),
        **obs_fixtures(),
        "cb05-deactivate-no-blocks": (files_of(dead_a, dead_b), {"OBS": "INCOMPLETE"}),
        "cb07-dup-two-pairs": (files_of(*dup1, *dup2), {"OBS": "PASS"}),
        "cb07-dup-unpaired": (files_of(*undup), {"OBS": "INCOMPLETE"}),
        "a1-fail-spurious-toggle": (files_of(*sp), {"A1": "FAIL"}),
        "a1-incomplete-in-only": (files_of(only_in), {"A1": "INCOMPLETE"}),
        "a1-incomplete-dup": (files_of(*dup_a, *dup_b), {"A1": "INCOMPLETE"}),
        "a2-incomplete-emptywindows": (files_of(*windows), {"A2": "INCOMPLETE"}),
        "a3-fail-lag": ({"compare-1.csv": compare_text("CB-04,export-offline,1,-inf,-inf,480")}, {"A3": "FAIL"}),
        "a3-incomplete-export-rt-only": ({"compare-1.csv": compare_text("LU-04,export-rt,1,-inf,-inf,0")}, {"A3": "INCOMPLETE"}),
        "a4-incomplete-fewblocks": (files_of(*few), {"A4": "INCOMPLETE"}),
        "a5-pass-early": (scen_a5("pass", pid=7100, early=True), {"A5": "PASS"}),
        "a5-fail-early-denied": (scen_a5("pass", pid=7200, early=True, early_denied=True), {"A5": "FAIL"}),
        "a6-pass-longname": (files_of(long_i, long_o), {"A6": "PASS"}),
        "a7-incomplete-callseq": (files_of(*callseq), {"A7": "INCOMPLETE"}),
        "a7-incomplete-fewplaying": (files_of(*fewplaying), {"A7": "INCOMPLETE"}),
        "a8-fail-broken-file": (broken, {"A8": "FAIL"}),
        "a9-pass-early": (scen_a9("pass", pid=4790, early=True), {"A9": "PASS"}),
        "f1-cb05-no-blocks": (full_pass(cb05_blocks=False), {}),
        "f1-missing-cell": (full_pass(skip_cell=("CB-01", "steady", 256, "low")), {}),
        "orphan-blocks": (files_of(*pair("CB-01", "steady", 256, "low", pid=4800, pre_blocks=2)), {}),
        "a4-pass-bufchange": (files_of(li, lo), {"A4": "PASS"}),
        "a2-incomplete-noblocks": (files_of(li2, lo2), {"A2": "INCOMPLETE"}),
        "a7-incomplete-notplaying": (files_of(*still), {"A7": "INCOMPLETE"}),
    }


def fixtures():
    """{目录名: {文件名: 文本}} 与 expect.json 的内容。"""
    tree = {}
    expect = {}
    for item in ITEMS:
        for st in STATES:
            tree[f"{item.lower()}-{st}"] = SCENARIOS[item](st)
            expect[f"{item.lower()}-{st}"] = {item: st.upper()}
    for d, (files, want) in extra_cells().items():
        tree[d] = files
        expect[d] = {k: v.upper() for k, v in want.items()}
    tree["full-pass"] = full_pass()
    return tree, expect


def _render():
    tree, expect = fixtures()
    files = {}
    for d, fs in tree.items():
        for name, text in fs.items():
            files[f"{d}/{name}"] = text
    files["expect.json"] = json.dumps(expect, indent=2, sort_keys=True) + "\n"
    return files


def write():
    # 去掉已不再生成的旧夹具。只清脚本旁写死的 fixtures 目录：真实路径必须正好是它，且本身不是链接
    if os.path.islink(FIXTURE_DIR) or os.path.realpath(FIXTURE_DIR) != os.path.join(os.path.realpath(HERE), "fixtures"):
        raise SystemExit(f"拒绝清理 {FIXTURE_DIR}：不是脚本旁写死的 fixtures 目录")
    # rinne-delete-ok: 上面核对过真实路径正好是脚本旁的 fixtures 目录
    shutil.rmtree(FIXTURE_DIR, ignore_errors=True)
    for rel, text in _render().items():
        p = os.path.join(FIXTURE_DIR, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="ascii", newline="\n") as f:
            f.write(text)


def check():
    """已入库的 fixtures/ 必须与生成器逐字节一致，多一个少一个文件都算漂移。"""
    want = _render()
    have = {}
    for root, _, names in os.walk(FIXTURE_DIR):
        for nm in names:
            p = os.path.join(root, nm)
            rel = os.path.relpath(p, FIXTURE_DIR).replace(os.sep, "/")
            with open(p, "rb") as f:
                have[rel] = f.read().decode("ascii")
    bad = sorted(set(want) ^ set(have)) + sorted(k for k in want if k in have and want[k] != have[k])
    return bad


if __name__ == "__main__":
    if "--write" in sys.argv:
        write()
    elif "--check" in sys.argv:
        drift = check()
        print("\n".join(f"漂移：{d}" for d in drift) or "fixtures 与生成器一致")
        sys.exit(1 if drift else 0)
    else:
        print(__doc__)
