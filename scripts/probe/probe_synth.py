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


def full_pass(skip_cell=None):
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

    out.update(files_of(*pair("CB-05", "bypass-out", hook=bypass, pid=4200)))
    return out


def _matrix(case):
    """F1 要求的矩阵：CB-01 是 3 档缓冲乘 4 档 ASIO-Guard，LU-03 是 3 档缓冲。"""
    if case == "CB-01":
        return [("CB-01", "steady", b, g) for b in (64, 256, 1024) for g in ("off", "low", "normal", "high")]
    return [("LU-03", "steady", b, "na") for b in (64, 512, 2048)]


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
    dup2 = pair("CB-07", "dup", blocks=False, pid=4821, tok=("a9b8", "c7d6"))
    for li_, lo_ in (dup1, dup2):
        li_.event(T_START + 20, "paired", lo_.pid, "A")
    undup = pair("CB-07", "dup", blocks=False, pid=4830)
    return {
        "cb05-deactivate-single-end": (files_of(*one_end(pid=4800)), {"A4": "PASS"}),
        "cb05-deactivate-no-blocks": (files_of(dead_a, dead_b), {"A4": "INCOMPLETE"}),
        "cb05-deactivate-no-event": (files_of(*one_end(pid=4840, event=False)), {"A4": "INCOMPLETE"}),
        "cb07-dup-two-pairs": (files_of(*dup1, *dup2), {"A4": "PASS"}),
        "cb07-dup-unpaired": (files_of(*undup), {"A4": "INCOMPLETE"}),
        "a1-fail-spurious-toggle": (files_of(*sp), {"A1": "FAIL"}),
        "a1-incomplete-in-only": (files_of(only_in), {"A1": "INCOMPLETE"}),
        "a1-incomplete-dup": (files_of(*dup_a, *dup_b), {"A1": "INCOMPLETE"}),
        "a2-incomplete-emptywindows": (files_of(*windows), {"A2": "INCOMPLETE"}),
        "a3-fail-lag": ({"compare-1.csv": compare_text("CB-04,export-offline,1,-inf,-inf,480")}, {"A3": "FAIL"}),
        "a3-incomplete-bounce-only": ({"compare-1.csv": compare_text("CB-04,bounce,1,-inf,-inf,0")}, {"A3": "INCOMPLETE"}),
        "a4-incomplete-fewblocks": (files_of(*few), {"A4": "INCOMPLETE"}),
        "a5-pass-early": (scen_a5("pass", pid=7100, early=True), {"A5": "PASS"}),
        "a5-fail-early-denied": (scen_a5("pass", pid=7200, early=True, early_denied=True), {"A5": "FAIL"}),
        "a6-pass-longname": (files_of(long_i, long_o), {"A6": "PASS"}),
        "a7-incomplete-callseq": (files_of(*callseq), {"A7": "INCOMPLETE"}),
        "a7-incomplete-fewplaying": (files_of(*fewplaying), {"A7": "INCOMPLETE"}),
        "a8-fail-broken-file": (broken, {"A8": "FAIL"}),
        "a9-pass-early": (scen_a9("pass", pid=4790, early=True), {"A9": "PASS"}),
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
    shutil.rmtree(FIXTURE_DIR, ignore_errors=True)  # 去掉已不再生成的旧夹具
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
