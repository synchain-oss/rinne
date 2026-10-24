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
        if not drop:
            self.lines.append("B," + ",".join(str(row[c]) for c in BLOCK_COLS))

    def close(self, t, blocks=None, dropped=0):
        self.lines.append(f"Z,{t},{self.seq if blocks is None else blocks},{dropped}")
        self.closed = True

    def text(self):
        return "\n".join(self.lines) + "\n"


def pair(case, variant="-", buf="-", guard="na", *, count=40, hook=None, fmt=("vst3", "vst3"), pid=4100,
         tok=("a1b2", "c3d4"), host_kind="cubase", os_name="win", prepare_max=256, blocks=True, close=True,
         sr=48000, pre_blocks=0):  # fmt: skip
    """一对 In / Out 实例。hook(role, k, row) 可改行；row["_drop"] = True 表示丢掉这一条（制造序号缺口）。"""
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
            t = T_START + k * PERIOD
            k2 = k + pre_blocks
            for lg, cs, dt in ((li, 2 * k2, 0), (lo, 2 * k2 + 1, 1000)):
                row = dict(host_ns=HOST0 + k * PERIOD, mono_ns=t + dt, rs=1 if k == 0 and lg is li else 0)
                if hook:
                    hook(lg.role, k, row)
                drop = row.pop("_drop", False)
                cs = row.pop("_cs", cs)
                lg.block(cs, row.pop("mono_ns"), drop=drop, **row)
    for lg in (li, lo):
        lg.summary(T_START + 1_000_000_000, blocks=count if blocks else 0)
        lg.summary(T_START + 2_000_000_000, blocks=count if blocks else 0)
        if close:
            lg.close(T_START + 2_100_000_000)
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


def multi_cells(cells, *, pid, tok=("e1f2", "e3f4"), sr=48000, prepare_max=1024):
    """像 multi_pair，但每格单独指定块数与记录方式，用来造「现场误记了一格」的日志。
    cells = [(case, variant, buf, guard, 块数, 方式)]，方式：
      ok       正常写出；
      unbound  窗口里拿不到绑定：块不写 B 行，却占 seq、计丢弃（探针对没取到绑定的块就是这么记的）；
      gap:K    In 端第 K 块丢掉（序号缺口，同时计丢弃）；
      taildrop 正常写完之后，窗口末尾又有 3 块没写出（占 seq、计丢弃，但之后没有任何块可以让缺口露出来）。
    S 行与 Z 行带累计丢弃数。"""
    li = Log("I", "vst3", "cubase", "win", pid=pid, tok=tok[0])
    lo = Log("O", "vst3", "cubase", "win", pid=pid, tok=tok[1])
    span = 1_000_000_000
    dropped = {"I": 0, "O": 0}
    for lg in (li, lo):
        lg.event(T_START - 500, "prepare", sr, prepare_max)
        lg.summary(T_START)
    g = 0
    for c, (case, variant, buf, guard, count, mode) in enumerate(cells):
        base = T_START + c * span
        for lg in (li, lo):
            lg.case(base - 100, case, variant, buf, guard, sr)
            lg.summary(base + span // 2, dropped=dropped[lg.role])
        for k in range(count):
            host = HOST0 + c * span + k * PERIOD
            drop_in = mode == "unbound" or mode == f"gap:{k}"
            drop_out = mode == "unbound"
            li.block(2 * g, base + k * PERIOD, host_ns=host, rs=1 if g == 0 else 0, drop=drop_in)
            lo.block(2 * g + 1, base + k * PERIOD + 1000, host_ns=host, drop=drop_out)
            dropped["I"] += drop_in
            dropped["O"] += drop_out
            g += 1
        if mode == "taildrop":
            for lg in (li, lo):
                for _ in range(3):
                    lg.block(0, 0, drop=True)
                dropped[lg.role] += 3
    end = T_START + len(cells) * span
    for lg in (li, lo):
        lg.summary(end, dropped=dropped[lg.role])
        lg.close(end + 100_000_000, dropped=dropped[lg.role])
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


def scen_a2_thread(state, pid=4100, switches=10, explicit=False):
    """宿主不通知监听键，日志里没有 monitor_toggle：切换点只能从 In 端 B 行 thread 的变化推断。
    state：pass；fail（第 3 次切换后重同步两次）；incomplete（thread 一直不变，只有块长 n 来回抖，抖动不算切换）；
    flap（thread 每 3 块换一次，一共几十次，每个窗口都不足 8 块）。
    explicit=True 另外写出显式事件，与 thread 变化落在同一块上：同一次切换不能数两遍。"""
    ks = [5 + 10 * i for i in range(switches)]

    def hook(r, k, row):
        if r != "I":
            return
        if state == "incomplete":
            row["n"] = 128 if k % 2 else 256
            return
        if state == "flap":  # 线程池里每 3 块换一次线程：窗口都太短，不能当成十几次像样的切换去判通过
            row["thread"] = THREADS[(k // 3) % 2]
            return
        row["thread"] = THREADS[sum(1 for x in ks if x <= k) % 2]
        if k in ks:
            row["rs"] = 2 if (state == "fail" and k == ks[2]) else 1

    li, lo = pair("CB-02", "toggle", 256, "low", count=120, hook=hook, pid=pid)
    if explicit:
        for i, k in enumerate(ks):
            li.event(T_START + k * PERIOD, "monitor_toggle", i % 2)
    return files_of(li, lo)


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


def full_pass(skip_cell=None, cb05_blocks=True, skip_rip=False):
    """所有判据都通过，且 F1 要求的七个 case 与两张矩阵都齐。每个场景用自己的 pid，文件名不冲突。"""
    out = {}
    cb01 = [c for c in _matrix("CB-01") if c != skip_cell]
    out.update(files_of(*multi_pair(cb01, pid=4100)))
    out.update(files_of(*multi_pair(_matrix("LU-03"), pid=4300, host_kind="luna", os_name="mac")))
    for n, item in enumerate(("A2", "A6", "A9"), start=1):
        out.update(SCENARIOS[item]("pass", pid=4100 + n))
    out.update(scen_a5("pass", pid=7000))
    out["compare-1.csv"] = compare_text("CB-04,export-offline,1,-inf,-inf,0", "LU-04,export-offline,0,-128.0,-99.5,0")

    def bypass(r, k, row):
        row["bypassed"] = 1 if k >= 20 else 0

    for n, v in enumerate(("bypass-out", "bypass-in")):
        out.update(files_of(*pair("CB-05", v, hook=bypass, pid=4200 + n, tok=(f"b{n}a1", f"b{n}a2"), blocks=cb05_blocks)))
    for n, v in enumerate(("deactivate", "disable-track")):
        li, lo = pair("CB-05", v, pid=4210 + n, tok=(f"c{n}a1", f"c{n}a2"), blocks=cb05_blocks)
        lo.event(T_START + 10, "set_processing", 0)
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
    def one_end(**kw):
        a, b = pair("CB-05", "deactivate", pid=kw["pid"], blocks=kw.get("blocks", True))
        b.lines = [ln for ln in b.lines if not ln.startswith("B,")]  # Out 被停用，没有块
        if kw.get("event", True):
            b.event(T_START + 10, "set_processing", 0)
        return a, b

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
    cat2_only = pair("CB-05", "deactivate", pid=4870)
    cat2_only[1].event(T_START + 10, "set_processing", 0)
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

    def cb05_stop(variant, name, a="", pid=4940, pre=False):
        """CB-05 停用格：Out 端在本段里（或段之前，pre=True）记一个停用相关的事件。"""
        a_, b_ = pair("CB-05", variant, pid=pid, tok=("c0a1", "c0a2"))
        b_.event(T_START - 400 if pre else T_START + 10, name, a)
        return files_of(a_, b_)

    pre_ping = pair("CB-06", "smart", pid=4950)
    pre_ping[0].event(T_START - 400, "ping", 9600)  # 第一个 C 行之前的 Ping（第 0 步留下的）
    pre_ping[0].event(T_START + 1000, "ping", 9600)  # 本段里只有这一条
    good = ("CB-01", "steady", 64, "off", 40, "ok")
    good2 = ("CB-01", "steady", 256, "low", 40, "ok")
    good3 = ("CB-01", "steady", 1024, "high", 40, "ok")
    stray = ("CB-01", "steady", "-", "na", 5, "ok")  # 面板默认 Case 误记的一格：只有几块
    stray_unbound = ("CB-01", "steady", "-", "na", 40, "unbound")
    return {
        "a2-pass-thread-inferred": (scen_a2_thread("pass", pid=5000), {"A2": "PASS"}),
        "a2-fail-thread-inferred": (scen_a2_thread("fail", pid=5001), {"A2": "FAIL"}),
        "a2-incomplete-thread-constant": (scen_a2_thread("incomplete", pid=5002), {"A2": "INCOMPLETE"}),
        "a2-incomplete-thread-few": (scen_a2_thread("pass", pid=5003, switches=3), {"A2": "INCOMPLETE"}),
        "a2-incomplete-thread-flap": (scen_a2_thread("flap", pid=5005), {"A2": "INCOMPLETE"}),
        "a2-incomplete-one-cell-no-switch": (  # 一格有 10 次切换，另一格一次都没有：后者不能被前者的次数盖住
            {**scen_a2_thread("pass", pid=5006), **files_of(*pair("CB-02", "toggle", 64, "low", count=120, pid=5007))},
            {"A2": "INCOMPLETE"},
        ),
        "a2-pass-explicit-and-thread": (scen_a2_thread("pass", pid=5004, explicit=True), {"A2": "PASS"}),
        "cb05-deactivate-reset-only": (cb05_stop("deactivate", "reset"), {"OBS": "PASS"}),
        "cb05-deactivate-release-only": (cb05_stop("deactivate", "release"), {"OBS": "PASS"}),
        "cb05-disable-track-reset-only": (cb05_stop("disable-track", "reset"), {"OBS": "PASS"}),
        "cb05-deactivate-set-processing-on": (cb05_stop("deactivate", "set_processing", 1), {"OBS": "INCOMPLETE"}),
        "cb05-deactivate-pre-release": (cb05_stop("deactivate", "release", pre=True), {"OBS": "INCOMPLETE"}),
        "cb06-smart-pre-ping": (files_of(*pre_ping), {"OBS": "INCOMPLETE"}),
        "a8-pass-short-clones": (
            files_of(*pair("CB-11", "constrain", 256, "low", pid=5010), clone("I", 5011, "f0f1"), clone("O", 5011, "f0f2", 120_000_000)),
            {"A8": "PASS"},
        ),
        "a8-incomplete-long-no-c": (
            files_of(*pair("CB-11", "constrain", 256, "low", pid=5012), clone("I", 5013, "f0f3", 2_500_000_000)),
            {"A8": "INCOMPLETE"},
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
        "a7-incomplete-cat2-full": (files_of(*cat2_full), {"A7": "INCOMPLETE"}),
        "a4-incomplete-cat2-gap": (files_of(*gap_a), {"A4": "INCOMPLETE"}),
        "a6-incomplete-one-cell-missing": (files_of(*one_cell_missing), {"A6": "INCOMPLETE"}),
        "f1-cb04-no-rip": (full_pass(skip_rip=True), {}),
        "cb12-twoproj-stray-out": (files_of(*tp_ok, lone_out), {"OBS": "INCOMPLETE"}),
        "f1-cb05-bypass-dup": (files_of(*two_bypass[0], *two_bypass[1]), {"OBS": "INCOMPLETE"}),
        "cb12-twoproj-two-pairs": (files_of(*tp_ok), {"OBS": "PASS"}),
        "cb12-twoproj-unpaired": (files_of(*tp_ok, *tp3), {"OBS": "INCOMPLETE"}),
        "cb07-dup-one-bad": (files_of(*dup_good, *bad_dup), {"OBS": "INCOMPLETE"}),
        "a4-incomplete-only-cat2": (files_of(*cat2_only), {"A4": "INCOMPLETE", "OBS": "PASS"}),
        "a1-fail-guard-variant": (files_of(*guard_fail), {"A1": "FAIL"}),
        "a6-fail-multi-instance": (files_of(*names_a, *names_b), {"A6": "FAIL"}),
        **obs_fixtures(),
        "cb05-deactivate-single-end": (files_of(*one_end(pid=4800)), {"OBS": "PASS"}),
        "cb05-deactivate-no-blocks": (files_of(dead_a, dead_b), {"OBS": "INCOMPLETE"}),
        "cb05-deactivate-no-event": (files_of(*one_end(pid=4840, event=False)), {"OBS": "INCOMPLETE"}),
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
