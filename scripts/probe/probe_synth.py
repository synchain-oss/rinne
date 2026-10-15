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
         sr=48000):  # fmt: skip
    """一对 In / Out 实例。hook(role, k, row) 可改行；row["_drop"] = True 表示丢掉这一条（制造序号缺口）。"""
    li = Log("I", fmt[0], host_kind, os_name, pid=pid, tok=tok[0])
    lo = Log("O", fmt[1], host_kind, os_name, pid=pid, tok=tok[1])
    for lg in (li, lo):
        lg.event(T_START - 500, "prepare", sr, prepare_max)
        lg.case(T_START - 100, case, variant, buf, guard, sr)
        lg.summary(T_START)
    if blocks:
        for k in range(count):
            t = T_START + k * PERIOD
            for lg, cs, dt in ((li, 2 * k, 0), (lo, 2 * k + 1, 1000)):
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
    ks = [5 + 6 * i for i in range(toggles)]

    def hook(r, k, row):
        if r == "I" and k in ks:  # 切换后的第一块记一次重同步；失败格里第 3 次切换记两次
            row["rs"] = 2 if (state == "fail" and k == ks[2]) else 1

    li, lo = pair("CB-02", "toggle", 256, "low", count=70, hook=hook, pid=pid)
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


def scen_a5(state, pid=7000):
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
        li.event(T_START + 10, "backend", "posix", 0)
        lo.event(T_START + 10, "backend", "posix", 0)
        li.event(T_START + 20, "paired", lo.pid, "A")
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


def scen_a9(state, pid=4100):
    variant = "dup" if state == "incomplete" else "reopen"  # 没做保存重开这一格
    li, lo = pair("CB-07", variant, blocks=False, pid=pid)
    if state != "incomplete":
        li.event(T_START + 30, "set_state", "A")
        lo.event(T_START + 30, "set_state", "A")
    if state == "pass":
        li.event(T_START + 60, "paired", lo.pid, "A")
    return files_of(li, lo)


SCENARIOS = {
    "A1": scen_a1, "A2": scen_a2, "A3": scen_a3, "A4": scen_a4, "A5": scen_a5,
    "A6": scen_a6, "A7": scen_a7, "A8": scen_a8, "A9": scen_a9,
}  # fmt: skip


def full_pass():
    """所有判据都通过，且 F1 要求的七个 case 都有数据。每个场景用自己的 pid，文件名不冲突。"""
    out = {}
    for n, item in enumerate(("A1", "A2", "A4", "A6", "A7", "A8", "A9")):
        out.update(SCENARIOS[item]("pass", pid=4100 + n))
    out.update(scen_a5("pass", pid=7000))
    out["compare-1.csv"] = compare_text("CB-04,export-offline,1,-inf,-inf,0", "LU-04,export-offline,0,-128.0,-99.5,0")

    def bypass(r, k, row):
        row["bypassed"] = 1 if k >= 20 else 0

    out.update(files_of(*pair("CB-05", "bypass-out", hook=bypass, pid=4200)))
    out.update(files_of(*pair("LU-03", "steady", 512, "na", host_kind="luna", os_name="mac", pid=4300)))
    return out


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
    return {
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
