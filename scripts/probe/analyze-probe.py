# SPDX-License-Identifier: GPL-3.0-or-later
"""探针日志分析：读一个目录里的 CSV，按 A1-A9 出结论表，并给出冻结条件 F1 的判定。

    python analyze-probe.py <日志目录> [--json] [--hash-salt <文本>] [--redact <词>]...
    python analyze-probe.py --self-test [--json]     # 夹具、严格格式、脱敏的自测
    python analyze-probe.py --mutation-check         # 逐条屏蔽判据与脱敏，每条都必须让自测变红
    python analyze-probe.py --spec                   # 以 JSON 打印日志格式的机读规格

退出码：0 = A1-A9 全部通过；1 = 有不通过或数据不全；2 = 输入不合格式或用法错误。

输出会被公开粘贴，所以只由枚举、整数、case 编号和 8 位散列拼成：不含路径、用户名、主机名，
轨道名只给 same / different / unknown 和一个 8 位散列。格式出错时的消息只带文件序号、行号和固定的列名，
不回显任何输入内容。最后还有一道兜底：输出全文过一遍 scrub()。
格式契约见同目录 LOG-FORMAT.md；本文件的 SPEC 常量是它的机读版本，两者由 checks.d/probe-analysis 对拍。
只用标准库。
"""

import getpass
import hashlib
import io
import json
import os
import re
import secrets
import socket
import sys
import tempfile
from bisect import bisect_right
from urllib.parse import unquote_to_bytes

sys.dont_write_bytecode = True  # 自测会导入同目录的 probe_synth，不往仓里写 __pycache__
HERE = os.path.dirname(os.path.abspath(__file__))
MAGIC = "rinne-probe-log,1"

# ---------------------------------------------------------------------------------------------------
# 规格
# ---------------------------------------------------------------------------------------------------

CASES = tuple(f"CB-{i:02d}" for i in range(1, 13)) + tuple(f"LU-{i:02d}" for i in range(1, 10))
VARIANTS = (
    "-", "steady", "toggle", "reopen", "dup", "preset", "fxchain", "undo", "rename",
    "export-rt", "export-offline", "bounce", "freeze", "rip", "dop",
    "bypass-in", "bypass-out", "deactivate", "disable-track", "smart",
    "guard-in", "guard-out", "guard-both", "misplaced-order", "misplaced-track", "misplaced-post",
    "mono", "bufchange", "constrain", "twoproj", "coreaudio", "au-au", "vst3-vst3", "au-vst3",
)  # fmt: skip
GUARDS = ("off", "low", "normal", "high", "na")
EVENTS = {
    # 名字: (a 的类型, b 的类型)
    "prepare": ("uint", "uint"),  # a = 采样率，b = 声明的最大块长
    "release": ("empty", "empty"),
    "reset": ("empty", "empty"),
    "set_processing": ("bool", "empty"),
    "offline_render": ("bool", "empty"),
    "set_state": ("group", "empty"),
    "get_state": ("empty", "empty"),
    "first_block": ("empty", "empty"),
    "first_paired_block": ("empty", "empty"),
    "paired": ("uint", "group"),  # a = 对端 pid，b = 组
    "track_props": ("pct", "colour"),  # a = 轨道名（百分号编码），b = 颜色
    "backend": ("backend", "uint"),  # a = win32 | posix，b = errno
    "shm_denied": ("empty", "empty"),
    "monitor_toggle": ("bool", "empty"),
    "arm_toggle": ("bool", "empty"),
    "process_mode": ("mode", "empty"),
    "ping": ("uint", "empty"),  # a = 实测环路延迟（样本）
    "destruct": ("empty", "empty"),
}
TOGGLES = ("monitor_toggle", "arm_toggle")
BLOCK_COLS = (
    "seq call_seq role n n_decl host_ns host_ns_valid playing nonrt bypassed mono_ns thread m w off rs d".split()
)
COMPARE_HEAD = "case,variant,bit_exact,residual_dbfs,head_residual_dbfs,lag_samples"

# 判据的阈值
MIN_STEADY_BLOCKS = 32
MIN_TOGGLES = 10
SLICE_NS = 2_000_000_000
RESIDUAL_LIMIT_DB = -120.0
HOSTNS_MISMATCH_RATIO = 1e-4
STALL_NS = 5_000_000_000
A1_CASES = ("CB-01", "CB-03", "LU-03")
A2_CASES = ("CB-02", "LU-03")
A3_CASES = ("CB-04", "LU-04")
A3_JUDGED = ("export-offline", "bounce")
A6_CASES = ("CB-07", "LU-06")
A9_CASES = ("CB-07", "LU-09")
F1_CASES = ("CB-01", "CB-02", "CB-04", "CB-05", "LU-02", "LU-03", "LU-04")
A5_COMBOS = (("au", "au"), ("vst3", "vst3"), ("au", "vst3"))

ITEM_NAMES = {
    "A1": "稳态对齐", "A2": "切换切片", "A3": "导出一致", "A4": "块长", "A5": "进程与后端",
    "A6": "轨道信息", "A7": "hostNs", "A8": "稳定性", "A9": "工程恢复",
}  # fmt: skip
VERDICT_ZH = {"PASS": "通过", "FAIL": "不通过", "INCOMPLETE": "数据不全"}

SPEC = {
    "magic": MAGIC,
    "rows": {
        "H": "role format host_kind os arch pid ppid tok4 build".split(),
        "C": "t_ns case variant buf guard sr".split(),
        "B": BLOCK_COLS,
        "S": "t_ns blocks dropped max_n resync trips".split(),
        "E": "t_ns name a b".split(),
        "Z": "t_ns blocks_total dropped_total".split(),
    },
    "events": sorted(EVENTS),
    "cases": list(CASES),
    "variants": list(VARIANTS),
    "guards": list(GUARDS),
    "compare_head": COMPARE_HEAD,
}

# ---------------------------------------------------------------------------------------------------
# 脱敏
# ---------------------------------------------------------------------------------------------------

REDACTED = "<已脱敏>"
_PATH_PATTERNS = [
    re.compile(r"[A-Za-z]:[\\/][^\s|,;]*"),
    re.compile(r"\\\\[^\s|,;]+"),
    re.compile(r"(?<![\w.])/(?:Users|home|Volumes|var|tmp|private|mnt|root|opt|Library)/[^\s|,;]*"),
    re.compile(r"(?<![\w.])~[\\/][^\s|,;]*"),
    re.compile(r"%[A-Za-z_]+%[^\s|,;]*"),
    re.compile(r"[\w.+-]+@[\w-]+\.[\w.-]+"),
]
_extra_terms = []


def sensitive_terms():
    """本机的用户名与主机名，以及调用方额外指定的词。太短的词不处理，免得误伤普通字样。"""
    terms = set(_extra_terms)
    for k in ("USERNAME", "USER", "LOGNAME", "COMPUTERNAME", "HOSTNAME", "RINNE_REDACT_EXTRA"):
        terms.update(t for t in os.environ.get(k, "").split(","))
    for fn in (getpass.getuser, socket.gethostname, lambda: os.path.basename(os.path.expanduser("~"))):
        try:
            terms.add(fn())
        except Exception:  # noqa: BLE001 - 取不到就不处理这一项
            pass
    terms.update(t.split(".")[0] for t in list(terms))
    return sorted((t for t in terms if len(t.strip()) >= 3), key=len, reverse=True)


def _scrub(text):
    for pat in _PATH_PATTERNS:
        text = pat.sub(REDACTED, text)
    for term in sensitive_terms():
        text = re.sub(re.escape(term.strip()), REDACTED, text, flags=re.IGNORECASE)
    return text


def _hash_name(raw_bytes, salt):
    return hashlib.sha256(salt + b"\0" + raw_bytes).hexdigest()[:8]


# 自测的变异检查通过替换这里的条目来屏蔽对应的防线
HOOKS = {"scrub": _scrub, "hash": _hash_name}


def scrub(text):
    return HOOKS["scrub"](text)


def name_verdict(a, b, salt):
    """两端轨道名（百分号编码的文本）→ (same|different|unknown, 8 位散列或 None)。"""
    if not a or not b:
        return "unknown", None
    ra, rb = unquote_to_bytes(a), unquote_to_bytes(b)
    h = HOOKS["hash"](ra, salt)
    return ("same" if ra == rb else "different"), h


# ---------------------------------------------------------------------------------------------------
# 严格解析
# ---------------------------------------------------------------------------------------------------


class FormatError(Exception):
    """消息里只放文件序号、行号和固定的说明，不带任何输入内容。"""

    def __init__(self, label, line, what):
        super().__init__(f"{label} 第 {line} 行：{what}")


_UINT = re.compile(r"^(?:0|[1-9][0-9]{0,18})$")
_SINT = re.compile(r"^-?(?:0|[1-9][0-9]{0,18})$")
_PCT = re.compile(r"^(?:[A-Za-z0-9._~-]|%[0-9A-Fa-f]{2}){0,200}$")
_FLOAT = re.compile(r"^(?:-inf|-?(?:0|[1-9][0-9]{0,6})(?:\.[0-9]{1,6})?)$")


def _enum(*vals):
    def f(s):
        if s not in vals:
            raise ValueError
        return s

    return f


def _re(pat, conv=str):
    rx = re.compile(pat)

    def f(s):
        if not rx.match(s):
            raise ValueError
        return conv(s)

    return f


def _uint(s):
    if not _UINT.match(s):
        raise ValueError
    return int(s)


def _sint(s):
    if not _SINT.match(s):
        raise ValueError
    return int(s)


def _float(s):
    if not _FLOAT.match(s):
        raise ValueError
    return float(s)


def _empty(s):
    if s != "":
        raise ValueError
    return ""


def _pct(s):
    if not _PCT.match(s):
        raise ValueError
    return s


def _buf(s):
    return s if s == "-" else _uint(s)


V_BOOL = _enum("0", "1")
V_GROUP = _enum("A", "B", "C", "D", "E", "F", "G", "H", "-")
ARG_TYPES = {
    "uint": _uint, "bool": lambda s: int(V_BOOL(s)), "empty": _empty, "group": V_GROUP, "pct": _pct,
    "colour": _re(r"^(?:|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$"), "backend": _enum("win32", "posix"),
    "mode": _enum("realtime", "offline"),
}  # fmt: skip
ROW_VALIDATORS = {
    "H": [
        _enum("I", "O"), _enum("vst3", "au"), _enum("cubase", "luna", "other"), _enum("win", "mac"),
        _enum("x64", "arm64"), _uint, _uint, _re(r"^[0-9a-f]{4}$"), _re(r"^[0-9a-f]{8}$"),
    ],
    "C": [_uint, _enum(*CASES), _enum(*VARIANTS), _buf, _enum(*GUARDS), _uint],
    "B": [
        _uint, _uint, _enum("I", "O"), _uint, _uint, _uint, lambda s: int(V_BOOL(s)), lambda s: int(V_BOOL(s)),
        lambda s: int(V_BOOL(s)), lambda s: int(V_BOOL(s)), _uint, _re(r"^[0-9a-f]{8}$"), _sint, _uint, _sint,
        _uint, _uint,
    ],
    "S": [_uint] * 6,
    "Z": [_uint] * 3,
}  # fmt: skip


class Log:
    def __init__(self, label):
        self.label = label
        self.header = None
        self.cases = []  # C 行，按出现顺序
        self.blocks = []
        self.events = []
        self.summaries = []
        self.closing = None
        self.truncated = False


def _split_row(label, ln, line, kind, cols):
    fields = line.split(",")
    if len(fields) != len(cols) + 1:
        raise FormatError(label, ln, f"{kind} 行应有 {len(cols) + 1} 列")
    return fields[1:]


def _convert(label, ln, kind, cols, validators, fields):
    out = {}
    for name, v, s in zip(cols, validators, fields):
        try:
            out[name] = v(s)
        except ValueError:
            raise FormatError(label, ln, f"{kind} 行的列 {name} 取值不合规") from None
    return out


def parse_log(raw, label):
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError:
        raise FormatError(label, 0, "含非 ASCII 字节（文本字段须百分号编码）") from None
    if "\r" in text:
        raise FormatError(label, 0, "含回车符，行尾只能是 LF")
    if not text:
        raise FormatError(label, 1, "空文件")
    lines = text.split("\n")
    lg = Log(label)
    if lines[-1] == "":
        lines.pop()
    else:  # 最后一行没有换行：写到一半被截断，丢掉它并记下
        lines.pop()
        lg.truncated = True
    if not lines or lines[0] != MAGIC:
        raise FormatError(label, 1, "首行不是格式魔数")
    last_seq = last_call = None
    for ln, line in enumerate(lines[1:], start=2):
        if lg.closing is not None:
            raise FormatError(label, ln, "Z 行之后不应再有内容")
        kind = line[:2] if line[1:2] == "," else ""
        kind = kind[:1]
        if kind not in SPEC["rows"]:
            raise FormatError(label, ln, "行类型未知")
        if (kind == "H") != (ln == 2):
            raise FormatError(label, ln, "H 行必须且只能出现在第 2 行")
        cols = SPEC["rows"][kind]
        if kind == "E":
            fields = _split_row(label, ln, line, kind, cols)
            row = _convert(label, ln, kind, cols[:2], [_uint, _enum(*EVENTS)], fields[:2])
            ta, tb = EVENTS[row["name"]]
            row.update(_convert(label, ln, kind, cols[2:], [ARG_TYPES[ta], ARG_TYPES[tb]], fields[2:]))
            lg.events.append(row)
            continue
        row = _convert(label, ln, kind, cols, ROW_VALIDATORS[kind], _split_row(label, ln, line, kind, cols))
        if kind == "H":
            lg.header = row
        elif kind == "C":
            lg.cases.append(row)
        elif kind == "S":
            lg.summaries.append(row)
        elif kind == "Z":
            lg.closing = row
        else:
            if row["role"] != lg.header["role"]:
                raise FormatError(label, ln, "B 行的 role 与 H 行不一致")
            if last_seq is not None and (row["seq"] <= last_seq or row["call_seq"] <= last_call):
                raise FormatError(label, ln, "seq / call_seq 必须严格递增")
            last_seq, last_call = row["seq"], row["call_seq"]
            lg.blocks.append(row)
    if lg.header is None:
        raise FormatError(label, 2, "缺 H 行")
    return lg


def parse_compare(raw, label):
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError:
        raise FormatError(label, 0, "含非 ASCII 字节") from None
    if "\r" in text or not text.endswith("\n"):
        raise FormatError(label, 0, "行尾只能是 LF，且末行须有换行")
    lines = text[:-1].split("\n")
    if lines[0] != COMPARE_HEAD:
        raise FormatError(label, 1, "表头不符")
    rows = []
    vs = [_enum(*CASES), _enum(*VARIANTS), lambda s: int(V_BOOL(s)), _float, _float, _sint]
    cols = COMPARE_HEAD.split(",")
    for ln, line in enumerate(lines[1:], start=2):
        f = line.split(",")
        if len(f) != len(cols):
            raise FormatError(label, ln, f"应有 {len(cols)} 列")
        rows.append(_convert(label, ln, "比对", cols, vs, f))
    return rows


# ---------------------------------------------------------------------------------------------------
# 会话：分段与分格
# ---------------------------------------------------------------------------------------------------


class Segment:
    def __init__(self, log, case):
        self.log = log
        self.case = case
        self.key = (case["case"], case["variant"], case["buf"], case["guard"])
        self.role = log.header["role"]
        self.blocks = []
        self.events = []
        self.gaps = 0

    def has(self, name):
        return any(e["name"] == name for e in self.events)


def key_label(key):
    return f"{key[0]}/{key[1]}/buf={key[2]}/guard={key[3]}"


class Session:
    def __init__(self, logs, compares):
        self.logs = logs
        self.compares = compares  # 比对行
        self.segments = []
        self.cells = {}
        for lg in logs:
            self._segment(lg)
        for seg in self.segments:
            self.cells.setdefault(seg.key, {"I": [], "O": []})[seg.role].append(seg)

    def _segment(self, lg):
        times = [c["t_ns"] for c in lg.cases]
        order = sorted(range(len(times)), key=lambda i: times[i])
        times = [times[i] for i in order]
        segs = [Segment(lg, lg.cases[i]) for i in order]
        self.segments += segs
        at = lambda t: (bisect_right(times, t) - 1)  # noqa: E731
        for e in lg.events:
            i = at(e["t_ns"])
            if i >= 0:
                segs[i].events.append(e)
        prev = None
        for b in lg.blocks:
            i = at(b["mono_ns"])
            if i >= 0:
                segs[i].blocks.append(b)
                if prev is not None and b["seq"] != prev + 1:
                    segs[i].gaps += 1
            prev = b["seq"]
        dropped = max([s["dropped"] for s in lg.summaries] + [lg.closing["dropped_total"] if lg.closing else 0])
        if dropped and not any(s.gaps for s in segs):
            for s in segs:
                s.gaps += 1

    def prepare_max(self, seg, t):
        """seg 所在日志里，时刻 t 之前最近一次 prepare 声明的最大块长；没有则 None。"""
        best = None
        for e in seg.log.events:
            if e["name"] == "prepare" and e["t_ns"] <= t and (best is None or e["t_ns"] >= best[0]):
                best = (e["t_ns"], e["b"])
        return None if best is None else best[1]


# ---------------------------------------------------------------------------------------------------
# 判据
# ---------------------------------------------------------------------------------------------------


def fold(item, parts, cover=()):
    """parts = [(状态, 说明)]。可信数据里的不通过压过数据不全：A1 的停工条件不能被别处的缺口盖住。"""
    states = [s for s, _ in parts]
    if "FAIL" in states:
        v = "FAIL"
    elif not parts or "INCOMPLETE" in states:
        v = "INCOMPLETE"
    else:
        v = "PASS"
    notes = [n for s, n in parts if n and (s != "PASS" or v == "PASS")]
    if not parts:
        notes = ["没有可用于本项的数据"]
    elif len(notes) > 4:
        notes = notes[:3] + [f"另有 {len(notes) - 3} 条同类记录"]
    return {"id": item, "name": ITEM_NAMES[item], "verdict": v, "notes": notes, "cover": sorted(set(cover))}


def _usable(seg, need=MIN_STEADY_BLOCKS):
    """分段是否有可信的块数据：返回 (None, 说明) 表示可用，否则 ("INCOMPLETE", 说明)。"""
    if seg.gaps:
        return "INCOMPLETE", f"{key_label(seg.key)} {seg.role} 序号缺口 {seg.gaps} 处"
    if len(seg.blocks) < need:
        return "INCOMPLETE", f"{key_label(seg.key)} {seg.role} 块数 {len(seg.blocks)} 不足 {need}"
    return None, ""


def _has_toggle(cell):
    return any(e["name"] in TOGGLES for s in cell["I"] + cell["O"] for e in s.events)


def judge_a1(s):
    parts, cover = [], []
    for key, cell in sorted(s.cells.items()):
        if key[0] not in A1_CASES or _has_toggle(cell):
            continue
        for seg in cell["I"] + cell["O"]:
            cover.append(key[0])
            bad, why = _usable(seg)
            if bad:
                parts.append((bad, why))
                continue
            blocks = sorted((b for b in seg.blocks if b["playing"] and not b["bypassed"]), key=lambda b: b["seq"])
            if len(blocks) < MIN_STEADY_BLOCKS:
                parts.append(("INCOMPLETE", f"{key_label(key)} {seg.role} 播放中的块 {len(blocks)} 不足"))
                continue
            bad_m = sum(1 for b in blocks if b["m"] != 0)
            bad_rs = sum(1 for b in blocks[1:] if b["rs"] != 0)
            offs = len({b["off"] for b in blocks})
            if bad_m or bad_rs or offs > 1:
                msg = f"{key_label(key)} {seg.role} m≠0 的块 {bad_m}、首块后重同步 {bad_rs}、off 取值 {offs} 种"
                parts.append(("FAIL", msg))
            else:
                parts.append(("PASS", ""))
    return fold("A1", parts, cover)


def judge_a2(s):
    parts, cover = [], []
    per_case = {}
    for key, cell in sorted(s.cells.items()):
        if key[0] not in A2_CASES or not _has_toggle(cell):
            continue
        cover.append(key[0])
        for seg in cell["I"]:
            ts = sorted(e["t_ns"] for e in seg.events if e["name"] in TOGGLES)
            per_case[key[0]] = per_case.get(key[0], 0) + len(ts)
            if seg.gaps:
                parts.append(("INCOMPLETE", f"{key_label(key)} 序号缺口 {seg.gaps} 处"))
                continue
            worst = 0
            for i, t in enumerate(ts):
                end = min(ts[i + 1] if i + 1 < len(ts) else t + SLICE_NS, t + SLICE_NS)
                worst = max(worst, sum(b["rs"] for b in seg.blocks if t <= b["mono_ns"] < end))
            ds = {b["d"] for b in seg.blocks}
            if worst > 1 or len(ds) > 1:
                parts.append(("FAIL", f"{key_label(key)} 每次切换后重同步最多 {worst} 次、D 取值 {len(ds)} 种"))
            else:
                parts.append(("PASS", ""))
    for case, n in sorted(per_case.items()):
        if n < MIN_TOGGLES:
            parts.append(("INCOMPLETE", f"{case} 只有 {n} 次切换，不足 {MIN_TOGGLES}"))
    return fold("A2", parts, cover)


def judge_a3(s):
    parts, cover = [], []
    for case in A3_CASES:
        rows = [r for r in s.compares if r["case"] == case]
        segs = [g for g in s.segments if g.case["case"] == case]
        if not rows and not segs:
            continue
        cover.append(case)
        if any(g.gaps for g in segs):
            parts.append(("INCOMPLETE", f"{case} 日志有序号缺口"))
        judged = [r for r in rows if r["variant"] in A3_JUDGED]
        if not judged:
            parts.append(("INCOMPLETE", f"{case} 缺导出比对（variant 须为 {'/'.join(A3_JUDGED)} 之一）"))
        for r in judged:
            ok = r["bit_exact"] == 1 or r["residual_dbfs"] <= RESIDUAL_LIMIT_DB
            head = "逐位相同" if r["bit_exact"] else f"残差 {r['residual_dbfs']:.1f} dBFS"
            msg = f"{case}/{r['variant']} {head}，开头单列 {r['head_residual_dbfs']:.1f} dBFS"
            parts.append(("PASS" if ok else "FAIL", msg))
    return fold("A3", parts, cover)


def judge_a4(s):
    parts, cover = [], []
    for seg in s.segments:
        if not seg.blocks:
            continue
        cover.append(seg.key[0])
        if seg.gaps:
            parts.append(("INCOMPLETE", f"{key_label(seg.key)} {seg.role} 序号缺口 {seg.gaps} 处"))
            continue
        first = min(b["mono_ns"] for b in seg.blocks)
        pmax = s.prepare_max(seg, first)
        if pmax is None:
            parts.append(("INCOMPLETE", f"{key_label(seg.key)} {seg.role} 没有 prepare 事件"))
            continue
        over = sum(1 for b in seg.blocks if b["n"] > pmax or b["n"] > b["n_decl"])
        ns = [b["n"] for b in seg.blocks]
        msg = f"{key_label(seg.key)} {seg.role} n∈[{min(ns)},{max(ns)}]，prepare max {pmax}，超限 {over}"
        parts.append(("FAIL" if over else "PASS", msg))
    return fold("A4", parts, cover)


def judge_a5(s):
    parts, cover, seen = [], [], set()
    for key, cell in sorted(s.cells.items()):
        if key[0] != "LU-02":
            continue
        cover.append("LU-02")
        if len(cell["I"]) != 1 or len(cell["O"]) != 1:
            parts.append(("INCOMPLETE", f"{key_label(key)} 应恰有一个 In 和一个 Out 实例"))
            continue
        i, o = cell["I"][0], cell["O"][0]
        combo = (i.log.header["format"], o.log.header["format"])
        seen.add(combo)
        same = i.log.header["pid"] == o.log.header["pid"]
        paired = any(e["name"] == "paired" and e["a"] == o.log.header["pid"] for e in i.events)
        denied = any(g.has("shm_denied") or any(e["name"] == "backend" and e["b"] != 0 for e in g.events)
                     for g in (i, o))  # fmt: skip
        ok = same and paired and not denied
        msg = f"{combo[0]}+{combo[1]}：同 pid {int(same)}、已配对 {int(paired)}、kShmDenied {int(denied)}"
        parts.append(("PASS" if ok else "FAIL", msg))
    for combo in A5_COMBOS:
        if combo not in seen and cover:
            parts.append(("INCOMPLETE", f"缺组合 {combo[0]}+{combo[1]}"))
    return fold("A5", parts, cover)


def judge_a6(s, salt=b"x"):
    parts, cover = [], []
    for key, cell in sorted(s.cells.items()):
        if key[0] not in A6_CASES:
            continue
        ends = {}
        for seg in cell["I"] + cell["O"]:
            props = [e for e in seg.events if e["name"] == "track_props"]
            if props:
                ends[seg.role] = props[-1]
        if not ends:
            continue
        cover.append(key[0])
        if set(ends) != {"I", "O"}:
            parts.append(("INCOMPLETE", f"{key_label(key)} 只有一端记录了轨道信息"))
            continue
        got = all(e["a"] for e in ends.values())
        verdict, h = name_verdict(ends["I"]["a"], ends["O"]["a"], salt)
        colour = "有" if all(e["b"] for e in ends.values()) else "缺"
        msg = f"{key_label(key)} 名字送达 {int(got)}（{verdict}" + (f"，散列 {h}" if h else "") + f"），颜色{colour}"
        parts.append(("PASS" if got else "FAIL", msg))
    return fold("A6", parts, cover)


def _pairs(i_blocks, o_blocks):
    merged = sorted([(b["call_seq"], "I", b) for b in i_blocks] + [(b["call_seq"], "O", b) for b in o_blocks],
                    key=lambda t: t[0])  # fmt: skip
    out, k = [], 0
    while k < len(merged) - 1:
        if merged[k][1] != merged[k + 1][1]:
            out.append((merged[k], merged[k + 1]))
            k += 2
        else:
            k += 1
    return out


def judge_a7(s):
    parts, cover = [], []
    for key, cell in sorted(s.cells.items()):
        if len(cell["I"]) != 1 or len(cell["O"]) != 1 or not (cell["I"][0].blocks and cell["O"][0].blocks):
            continue
        i, o = cell["I"][0], cell["O"][0]
        cover.append(key[0])
        bad = next((x for x in (_usable(i), _usable(o)) if x[0]), None)
        if bad:
            parts.append(bad)
            continue
        pairs = _pairs(i.blocks, o.blocks)
        playing = [b for sg in (i, o) for b in sg.blocks if b["playing"]]
        valid = sum(b["host_ns_valid"] for b in playing)
        if not playing or valid / len(playing) < 0.99:
            parts.append(("FAIL", f"{key_label(key)} hostNs 有效率 {valid}/{len(playing)}，顺序判据降为无法判断"))
            continue
        both = [(a[2], b[2]) for a, b in pairs if a[2]["host_ns_valid"] and b[2]["host_ns_valid"]]
        mismatch = sum(1 for a, b in both if a["host_ns"] != b["host_ns"])
        nonmono = 0
        for sg in (i, o):
            seq = [b["host_ns"] for b in sorted(sg.blocks, key=lambda b: b["seq"]) if b["playing"] and b["host_ns_valid"]]
            nonmono += sum(1 for x, y in zip(seq, seq[1:]) if y <= x)
        first = sum(1 for a, _ in pairs if a[1] == "I")
        ratio = mismatch / max(1, len(both))
        ok = ratio <= HOSTNS_MISMATCH_RATIO and nonmono == 0
        msg = f"{key_label(key)} 配对 {len(both)}，两端不等 {mismatch}，非递增 {nonmono}，In 先于 Out {first}/{len(pairs)}"
        parts.append(("PASS" if ok else "FAIL", msg))
    return fold("A7", parts, cover)


def judge_a8(s):
    parts = []
    for n, lg in enumerate(s.logs, start=1):
        who = f"实例 {n}（{lg.header['role']}）"
        ts = [x["t_ns"] for x in lg.summaries]
        stall = max([b - a for a, b in zip(ts, ts[1:])] + [0])
        trips = max([x["trips"] for x in lg.summaries] + [0])
        if lg.truncated or lg.closing is None:
            parts.append(("FAIL", f"{who} 日志没有正常收尾（疑似崩溃或被强杀）"))
        elif stall > STALL_NS or trips:
            parts.append(("FAIL", f"{who} 摘要间隔最长 {stall // 1_000_000} ms、跳闸 {trips} 次"))
        elif len(ts) < 2:
            parts.append(("INCOMPLETE", f"{who} 摘要行不足 2 条，无法判断卡死"))
        else:
            parts.append(("PASS", ""))
    return fold("A8", parts, [g.case["case"] for g in s.segments])


def judge_a9(s):
    parts, cover = [], []
    for key, cell in sorted(s.cells.items()):
        if key[0] not in A9_CASES or key[1] != "reopen":
            continue
        cover.append(key[0])
        for seg in cell["I"]:
            if seg.gaps:
                parts.append(("INCOMPLETE", f"{key_label(key)} 序号缺口"))
                continue
            sets = [e["t_ns"] for e in seg.events if e["name"] == "set_state"]
            pairs = [e["t_ns"] for e in seg.events if e["name"] == "paired"]
            ok = bool(sets) and any(p >= min(sets) for p in pairs)
            order = ">".join(e["name"] for e in sorted(seg.events, key=lambda e: e["t_ns"])
                             if e["name"] in ("prepare", "set_state", "first_block", "first_paired_block", "paired"))  # fmt: skip
            parts.append(("PASS" if ok else "FAIL", f"{key_label(key)} 事件顺序 {order or '无'}"))
    return fold("A9", parts, cover)


JUDGES = [("A1", judge_a1), ("A2", judge_a2), ("A3", judge_a3), ("A4", judge_a4), ("A5", judge_a5),
          ("A6", judge_a6), ("A7", judge_a7), ("A8", judge_a8), ("A9", judge_a9)]  # fmt: skip


def freeze_f1(s, items):
    missing = []
    for case in F1_CASES:
        segs = [g for g in s.segments if g.case["case"] == case]
        has = bool(segs) or any(r["case"] == case for r in s.compares)
        if not has:
            missing.append(f"{case} 无数据")
        elif any(g.gaps for g in segs):
            missing.append(f"{case} 有序号缺口")
    for it in items[:8]:  # P-1…P-9 对应 A1…A8；A9 是 P-10，不阻断 F1
        if it["verdict"] == "INCOMPLETE":
            missing.append(f"{it['id']} 数据不全")
    return {"ok": not missing, "missing": missing}


def extras(s):
    """只记录、不下结论的项：RIP/Freeze 是否克隆实例、Ping 一致性、各 case 的块长范围。"""
    out = []
    for key, cell in sorted(s.cells.items()):
        if key[1] in ("rip", "freeze"):
            n = len({id(seg.log) for seg in cell["I"] + cell["O"]})
            out.append(f"{key_label(key)} 涉及 {n} 个实例日志，{'有' if n > 2 else '无'}克隆迹象")
        pings = sorted({e["a"] for sg in cell["I"] for e in sg.events if e["name"] == "ping"})
        if pings:
            out.append(f"{key_label(key)} Ping 延迟取值 {len(pings)} 种" + ("（一致）" if len(pings) == 1 else ""))
    return out


# ---------------------------------------------------------------------------------------------------
# 读取与输出
# ---------------------------------------------------------------------------------------------------


def load_dir(path):
    names = sorted(os.listdir(path))
    logs, compares = [], []
    for nm in names:
        full = os.path.join(path, nm)
        if not os.path.isfile(full):
            continue
        if nm.startswith("probe-") and nm.endswith(".csv"):
            label = f"日志#{len(logs) + 1}"
            with open(full, "rb") as f:
                logs.append(parse_log(f.read(), label))
        elif nm.startswith("compare-") and nm.endswith(".csv"):
            label = f"比对#{len(compares) + 1}"
            with open(full, "rb") as f:
                compares += parse_compare(f.read(), label)
    return logs, compares


def analyze(path, salt):
    logs, compares = load_dir(path)
    s = Session(logs, compares)
    items = [(fn(s, salt) if iid == "A6" else fn(s)) for iid, fn in JUDGES]
    f1 = freeze_f1(s, items)
    roles = [lg.header["role"] for lg in logs]
    return {
        "inputs": {"logs": len(logs), "in": roles.count("I"), "out": roles.count("O"), "compares": len(compares)},
        "items": items,
        "f1": f1,
        "stop": items[0]["verdict"] == "FAIL",
        "extras": extras(s),
    }


def render_md(res):
    i = res["inputs"]
    out = ["# 探针分析结果", "", f"输入：实例日志 {i['logs']} 个（In {i['in']}、Out {i['out']}），导出比对 {i['compares']} 行", ""]
    if res["stop"]:
        out += ["**A1 不通过：停工并上报**，评估时间线寻址加固定 D 的混合方案，此前不冻结。", ""]
    out += ["| 项 | 内容 | 结论 | 依据 |", "|---|---|---|---|"]
    for it in res["items"]:
        cover = "、".join(it["cover"]) or "无"
        why = "；".join(it["notes"]).replace("|", "/") or "—"
        out.append(f"| {it['id']} | {it['name']}（{cover}） | {VERDICT_ZH[it['verdict']]} | {why} |")
    f1 = res["f1"]
    out += ["", "## 冻结条件", ""]
    out.append("- F1 数据齐全、P-1…P-9 有结论：" + ("满足" if f1["ok"] else "未满足（" + "；".join(f1["missing"]) + "）"))
    out += ["- F2 无需改布局：人工确认", "- F3 Win/mac 测试与压测全绿：以 gates 与 dispatch 结果为准", "- F4 用户已过目：人工确认"]
    if res["extras"]:
        out += ["", "## 附加记录", ""] + [f"- {x}" for x in res["extras"]]
    return "\n".join(out) + "\n"


def run(argv, out, err):
    args = list(argv)

    def take(flag, many=False):
        vals = []
        while flag in args:
            k = args.index(flag)
            if k + 1 >= len(args):
                raise SystemExit(2)
            vals.append(args[k + 1])
            del args[k : k + 2]
        return vals if many else (vals[-1] if vals else None)

    try:
        salt_arg = take("--hash-salt")
        _extra_terms[:] = take("--redact", many=True)
    except SystemExit:
        err.write("用法错误：选项缺少取值\n")
        return 2
    as_json = "--json" in args
    args = [a for a in args if a != "--json"]
    if "--spec" in args:
        out.write(json.dumps(SPEC, indent=2, sort_keys=True) + "\n")
        return 0
    if "--self-test" in args:
        r = self_test()
        for ln in r["lines"]:
            err.write(scrub(ln) + "\n")
        out.write(json.dumps(r) + "\n" if as_json else f"analyze-probe 自测：{'通过' if r['ok'] else '失败'}（坏样例 {r['bad']}、正例 {r['good']}）\n")
        return 0 if r["ok"] else 1
    if "--mutation-check" in args:
        r = mutation_check()
        for ln in r["lines"]:
            err.write(scrub(ln) + "\n")
        out.write(json.dumps(r) + "\n" if as_json else f"analyze-probe 变异检查：{'通过' if r['ok'] else '失败'}\n")
        return 0 if r["ok"] else 1
    if len(args) != 1 or not os.path.isdir(args[0]):
        err.write("用法：analyze-probe.py <日志目录> [--json] | --self-test | --mutation-check | --spec\n")
        return 2
    salt = (salt_arg or secrets.token_hex(16)).encode()
    try:
        res = analyze(args[0], salt)
    except FormatError as e:
        err.write(scrub(f"日志格式错误：{e}") + "\n")
        return 2
    except OSError:
        err.write("读取日志目录失败\n")
        return 2
    out.write(scrub(json.dumps(res, ensure_ascii=False, indent=2) + "\n" if as_json else render_md(res)))
    return 0 if all(it["verdict"] == "PASS" for it in res["items"]) else 1


# ---------------------------------------------------------------------------------------------------
# 自测
# ---------------------------------------------------------------------------------------------------


def _synth():
    sys.path.insert(0, HERE)
    try:
        import probe_synth

        return probe_synth
    finally:
        sys.path.remove(HERE)


def _write_tree(base, files):
    os.makedirs(base, exist_ok=True)
    for nm, text in files.items():
        with open(os.path.join(base, nm), "w", encoding="ascii", newline="\n") as f:
            f.write(text)


def _cli(argv):
    o, e = io.StringIO(), io.StringIO()
    code = run(argv, o, e)
    return code, o.getvalue(), e.getvalue()


def _mutations_of(text):
    """对一份合法 I 端日志做单点破坏，每一条都必须被严格解析拒绝。"""
    lines = text.split("\n")
    b = next(k for k, ln in enumerate(lines) if ln.startswith("B,"))
    e = next(k for k, ln in enumerate(lines) if ln.startswith("E,"))
    c = next(k for k, ln in enumerate(lines) if ln.startswith("C,"))

    def with_line(k, new):
        return "\n".join(lines[:k] + [new] + lines[k + 1 :])

    def edit(k, col, val):
        f = lines[k].split(",")
        f[col] = val
        return with_line(k, ",".join(f))

    yield "魔数不符", text.replace(MAGIC, "rinne-probe-log,2", 1)
    yield "缺 H 行", "\n".join(lines[:1] + lines[2:])
    yield "两个 H 行", "\n".join(lines[:2] + [lines[1]] + lines[2:])
    yield "未知行类型", with_line(c, "Q,1,2")
    yield "B 行列数不足", with_line(b, ",".join(lines[b].split(",")[:-1]))
    yield "B 行多一列", with_line(b, lines[b] + ",0")
    yield "n 为负数", edit(b, 4, "-1")
    yield "n 含前导零", edit(b, 4, "0256")
    yield "role 与 H 不一致", edit(b, 3, "O")
    yield "bool 取 2", edit(b, 7, "2")
    yield "seq 不递增", "\n".join(lines[: b + 1] + [lines[b]] + lines[b + 1 :])
    yield "未知事件名", edit(e, 2, "explode")
    yield "事件参数类型不符", edit(e, 3, "abc")
    yield "事件应为空的参数非空", with_line(e, "E,5,release,x,")
    yield "case 不在表内", edit(c, 2, "XX-99")
    yield "variant 不在表内", edit(c, 3, "my-own-label")
    yield "t_ns 含字母", edit(c, 1, "12ab")
    yield "含非 ASCII", text.replace("steady", "steaédy", 1)
    yield "含回车", text.replace("\n", "\r\n")
    yield "行中间的空行", "\n".join(lines[:b] + [""] + lines[b:])
    yield "Z 行之后还有内容", text + lines[b] + "\n"
    yield "H 行 pid 含路径", edit(1, 6, "x:" + "\\" + "dir")
    yield "空文件", ""


def self_test():
    ps = _synth()
    lines, bad, good = [], 0, 0

    def check(label, ok, is_bad):
        nonlocal bad, good
        bad += is_bad
        good += not is_bad
        if not ok:
            lines.append(f"失败：{label}")
        return ok

    tmp = tempfile.mkdtemp(prefix="rinne-probe-selftest-")
    salt = b"selftest"
    # 1. 夹具矩阵：每个判据的通过 / 不通过 / 数据不全
    tree, expect = ps.fixtures()
    for d, files in tree.items():
        base = os.path.join(tmp, d)
        _write_tree(base, files)
        res = analyze(base, salt)
        got = {it["id"]: it["verdict"] for it in res["items"]}
        want = expect.get(d) or {i: "PASS" for i in ps.ITEMS}
        for item, v in want.items():
            check(f"{d}：{item} 应为 {v}，实为 {got[item]}", got[item] == v, v != "PASS")
        if d == "full-pass":
            check("full-pass：F1 应满足", res["f1"]["ok"], False)
    stop = analyze(os.path.join(tmp, "a1-fail"), salt)["stop"]
    check("A1 不通过时给出停工提示", stop, True)
    check("已入库夹具与生成器一致", not ps.check(), False)
    # 2. 严格格式
    valid = next(iter(tree["a1-pass"].values()))
    for label, mutated in _mutations_of(valid):
        try:
            parse_log(mutated.encode("utf-8"), "日志#1")
            check(f"严格解析应拒绝：{label}", False, True)
        except FormatError as e:
            check(f"拒绝时不回显内容：{label}", "stea" not in str(e) and "dir" not in str(e), True)
    parse_log(valid.encode("ascii"), "日志#1")
    check("合法日志可解析", True, False)
    cut = parse_log(valid.rstrip("\n").encode("ascii"), "日志#1")
    check("末行缺换行视为截断而非格式错", cut.truncated, False)
    # 3. 脱敏：端到端
    user, host = "zorbulax", "QUUXBOX-77"
    saved = {k: os.environ.get(k) for k in ("USERNAME", "USER", "COMPUTERNAME", "HOSTNAME")}
    os.environ.update(USERNAME=user, USER=user, COMPUTERNAME=host, HOSTNAME=host)
    try:
        win_path = "C" + ":" + "\\" + "Users" + "\\" + user + "\\Music\\" + host + "\\song.wav"
        mac_path = "/" + "Users/" + user + "/Projects/" + host
        evil_dir = os.path.join(tmp, user + "-" + host)
        files = ps.pair("LU-06", "rename", host_kind="luna", os_name="mac", blocks=False)
        li, lo = files
        enc = lambda t: "".join(f"%{b:02X}" for b in t.encode())  # noqa: E731
        li.event(ps.T_START + 5, "track_props", enc(win_path), "ff8800")
        lo.event(ps.T_START + 5, "track_props", enc(mac_path), "ff8800")
        _write_tree(evil_dir, ps.files_of(li, lo))
        salts = ["--hash-salt", "s1"]
        banned = [user, host, user.upper(), host.lower(), enc(user), "Music", "song.wav", "Projects", "Users"]
        for fmt in ([], ["--json"]):
            code, so, se = _cli([evil_dir, *salts, *fmt])
            text = so + se
            for w in banned:
                check(f"输出不含 {w[:4]}…（{'json' if fmt else 'md'}）", w.lower() not in text.lower(), True)
            check("输出含 different 与 8 位散列", "different" in text and re.search(r"(?<![0-9a-f])[0-9a-f]{8}(?![0-9a-f])", text) is not None, False)
        same_dir = os.path.join(tmp, "same")
        li2, lo2 = ps.pair("LU-06", "rename", host_kind="luna", os_name="mac", blocks=False)
        for lg in (li2, lo2):
            lg.event(ps.T_START + 5, "track_props", enc(win_path), "")
        _write_tree(same_dir, ps.files_of(li2, lo2))
        _, so, _ = _cli([same_dir, *salts])
        check("两端同名给 same", "same" in so, False)
        h1 = re.findall(r"散列 ([0-9a-f]{8})", _cli([same_dir, "--hash-salt", "s1"])[1])
        h2 = re.findall(r"散列 ([0-9a-f]{8})", _cli([same_dir, "--hash-salt", "s2"])[1])
        check("散列带盐：换盐后不同", bool(h1) and bool(h2) and h1 != h2, True)
        # 格式错误的消息不回显内容
        li3, _ = ps.pair("LU-06", "rename", blocks=False)
        li3.lines[1] = li3.lines[1].replace(",4100,", "," + win_path.replace(",", "") + ",")
        bad_dir = os.path.join(tmp, user + "-bad")
        _write_tree(bad_dir, {li3.name: li3.text()})
        code, so, se = _cli([bad_dir])
        check("格式错误退出码 2", code == 2, True)
        check("格式错误消息不回显路径与用户名", user not in se and "Music" not in se and "Users" not in se, True)
        # 兜底 scrub
        nasty = f"{win_path} {mac_path} 由 {user}@{host}.lan 在 {host} 上 %APPDATA%\\x ~/doc \\\\srv\\share"
        cleaned = scrub(nasty)
        for w in (user, host, "Music", "Projects", "srv", "APPDATA", "doc"):
            check(f"scrub 去掉 {w[:4]}…", w not in cleaned, True)
        check("scrub 不动普通输出", scrub("A1 通过 CB-01/steady/buf=256/guard=low n∈[64,256]") == "A1 通过 CB-01/steady/buf=256/guard=low n∈[64,256]", False)
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    # 4. 命令行
    check("目录不存在退出码 2", _cli([os.path.join(tmp, "nope")])[0] == 2, True)
    check("全部通过退出码 0", _cli([os.path.join(tmp, "full-pass")])[0] == 0, False)
    check("有不通过退出码 1", _cli([os.path.join(tmp, "a1-fail")])[0] == 1, True)
    return {"ok": not lines, "bad": bad, "good": good, "lines": lines}


def mutation_check():
    """逐条屏蔽判据与脱敏防线，自测必须因此变红。"""
    lines = []
    base = self_test()
    if not base["ok"]:
        return {"ok": False, "lines": ["未经变异的自测就不是绿的", *base["lines"]]}
    stub = lambda *a, **k: {"id": "A?", "name": "", "verdict": "PASS", "notes": [], "cover": []}  # noqa: E731
    for k, (iid, fn) in enumerate(list(JUDGES)):
        if iid == "A6":
            JUDGES[k] = (iid, lambda s, salt, _i=iid: {**stub(), "id": _i, "name": ITEM_NAMES[_i]})
        else:
            JUDGES[k] = (iid, lambda s, _i=iid: {**stub(), "id": _i, "name": ITEM_NAMES[_i]})
        try:
            if self_test()["ok"]:
                lines.append(f"屏蔽 {iid} 后自测仍是绿的")
        finally:
            JUDGES[k] = (iid, fn)
    for hook, repl in (("scrub", lambda t: t), ("hash", lambda raw, salt: raw.decode("ascii", "replace")[:8])):
        orig = HOOKS[hook]
        HOOKS[hook] = repl
        try:
            if self_test()["ok"]:
                lines.append(f"屏蔽脱敏防线 {hook} 后自测仍是绿的")
        finally:
            HOOKS[hook] = orig
    # 阈值常量：放宽后对应的坏夹具必须不再被拦住，自测随之变红
    for const, loose in (("RESIDUAL_LIMIT_DB", 0.0), ("MIN_TOGGLES", 0), ("HOSTNS_MISMATCH_RATIO", 1.0)):
        orig = globals()[const]
        globals()[const] = loose
        try:
            if self_test()["ok"]:
                lines.append(f"放宽阈值 {const} 后自测仍是绿的")
        finally:
            globals()[const] = orig
    return {"ok": not lines, "lines": lines}


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:], sys.stdout, sys.stderr))
