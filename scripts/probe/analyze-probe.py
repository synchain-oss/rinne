# SPDX-License-Identifier: GPL-3.0-or-later
"""探针日志分析：读一个目录里的 CSV，按 A1-A9 出结论表，并给出冻结条件 F1 的判定。

    python analyze-probe.py <日志目录> [--json] [--hash-salt <文本>] [--redact <词>]... [--exclude <文件名>:<C 行序号或 t_ns>]...
    python analyze-probe.py --self-test [--json]     # 夹具、严格格式、脱敏的自测
    python analyze-probe.py --mutation-check         # 逐条屏蔽判据与脱敏，每条都必须让自测变红
    python analyze-probe.py --spec                   # 以 JSON 打印日志格式的机读规格

退出码：0 = A1-A9 全部通过且 F1 满足；1 = 有不通过；3 = 无不通过但有数据不全或 F1 未满足；2 = 输入不合格式或用法错误；4 = 内部错误；
5 = `--exclude` 指定的段不存在或不唯一（参数指向的东西没找到，与 2 的「写法或日志格式不对」分开）。
`--json` 的 `stop` 字段为真表示 A1 不通过，要停工上报。输出固定为 UTF-8。

`--exclude <文件名>:<选择子>` 把现场误记的一段（一个 C 行起、到下一个 C 行或文件尾止）整段拿出判据，可重复。文件名是日志目录里的 CSV 文件名；
选择子是该文件里 C 行的 1 起算序号（按文件里出现的顺序），或 C 行的 `t_ns`；两者都可能指同一个数时写 `#3`（序号）或 `t=3`（t_ns）。
In 与 Out 各有一份文件，一格误记要在两个文件里各排除一次。被排除的段不进任何判据，输出的「已排除的段」逐条列出参数与对应的格；
段与段之间的序号缺口照旧按块序号算，但落在被排除段上的缺口（含探针报告丢弃却定位不到缺口时）记在被排除段身上，不记给相邻的段。

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
import shutil
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
    "prepare": ("sr", "blk"),  # a = 采样率，b = 声明的最大块长
    "release": ("empty", "empty"),
    "reset": ("empty", "empty"),
    "set_processing": ("bool", "empty"),
    "offline_render": ("bool", "empty"),
    "set_state": ("group", "empty"),
    "get_state": ("empty", "empty"),
    "first_block": ("empty", "empty"),
    "first_paired_block": ("empty", "empty"),
    "paired": ("pid", "group"),  # a = 对端 pid，b = 组
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
A3_REQUIRED = {"CB-04": ("export-offline",), "LU-04": ("export-offline", "bounce")}
MIN_SLICE_BLOCKS = 8
SHORT_LIVED_NS = 2_000_000_000  # 没有 C 行、首尾时间跨度短于此值的实例：Render in Place / Freeze 的临时克隆，A8 只记说明
EXIT_EXCLUDE = 5  # --exclude 指定的段不存在或不唯一
CLONE_VARIANTS = ("rip", "freeze")  # 宿主可能克隆实例：同键出现多个实例是预期内的
MULTI_OK = ("rip", "freeze", "dup", "undo", "twoproj")  # 同键出现多对实例是预期内的

# 规则表：(case, variant) → (类别, 喂给哪些判据, 本格自己的证据)。分析器与 LOG-FORMAT 读同一张表，run-checks 逐行对拍；
# 表外的组合在解析时判格式错。类别 1 = 成对满块：两端各恰一个实例、各至少 MIN_STEADY_BLOCKS 块，缺了判数据不全；
# 类别 2 = 各自证据：不要求成对和块数，但必须有表里写的证据，一条都没有就判数据不全；类别 3 = 只记录，不阻断。
CAT_LABEL = {1: "1 成对满块", 2: "2 各自证据", 3: "3 只记录"}
RULES = {}


def _r(cases, variants, cat, feeds, evidence):
    for c in cases.split():
        for v in variants.split():
            RULES[(c, v)] = (cat, feeds, evidence)


_STEADY = "两端各至少 32 块"
_EXPORT = "导出比对行见 A3；块只查块长上限，不进 A7"
_DEACT = "至少一端 32 块，且本段内有 set_processing（a 为 0）、reset 或 release 事件"
_PAIRED_EV = "每个 In 实例都有 paired 事件（同一宿主里两端同 pid，所以不核对对端 pid）"
_r("CB-01 LU-03", "steady", 1, "A1 A4 A7", _STEADY)
_r("CB-02 LU-03", "toggle", 1, "A2 A4 A7", "两端各至少 32 块，每次切换后的窗口里至少 8 块")
_r("CB-03", "guard-in guard-out guard-both", 1, "A1 A4 A7", _STEADY)
_r("CB-09", "mono", 1, "A4 A7", _STEADY)
_r("CB-11", "constrain", 1, "A4 A7", _STEADY)
_r("CB-10", "bufchange", 1, "A4", _STEADY)
_CLONE = "至少一个实例有块或事件；格观察列出实例数（克隆实例就看这个）"
_r("CB-04", "export-rt export-offline dop", 2, "A3 A4", _EXPORT)
_r("LU-04", "export-rt export-offline bounce", 2, "A3 A4", _EXPORT)
_r("CB-04 LU-04", "freeze", 2, "A4 格观察", _CLONE)
_r("CB-04", "rip", 2, "A4 格观察", _CLONE)
_r("CB-05 LU-05", "bypass-in bypass-out", 2, "格观察", "至少一端 32 块，且有 bypassed 为 1 的块")
_r("CB-05 LU-05", "deactivate", 2, "格观察", _DEACT)
_r("CB-05", "disable-track", 2, "格观察", _DEACT)
_r("CB-06", "smart", 2, "格观察", "本段内至少两条 ping 事件（前后各一次）")
_r("CB-07 LU-09", "reopen", 2, "A9", "set_state 之后出现 paired")
_r("CB-07 LU-06", "rename", 2, "A6", "每个实例都收到非空轨道名")
_r("CB-07", "dup undo", 2, "格观察", _PAIRED_EV)
_r("CB-07", "preset fxchain", 3, "格观察", "只记录，不阻断")
_r("CB-08", "misplaced-order misplaced-track misplaced-post", 2, "格观察", "至少一端有记录；paired 有无见格观察")
_r("CB-12", "twoproj", 2, "格观察", "每个 In 实例都有 paired 事件，且 In 与 Out 实例数相等")
_r("LU-01 LU-08", "-", 3, "格观察", "只记录，不阻断")
_r("LU-02", "au-au vst3-vst3 au-vst3", 2, "A5", "同 pid、已配对、无 shm_denied")
_r("LU-07", "coreaudio", 2, "格观察", "有 backend 事件或任一事件，不要求块")
assert {c for c, _ in RULES} == set(CASES) and {v for _, v in RULES} == set(VARIANTS), "规则表必须覆盖全部 case 与 variant"


def _rule(key):
    return RULES[(key[0], key[1])]


def is_cat1(key):
    return _rule(key)[0] == 1


def feeds_of(key):
    return _rule(key)[1].split()


MATRIX = {
    "CB-01": [("steady", b, g) for b in (64, 256, 1024) for g in ("off", "low", "normal", "high")],
    "LU-03": [("steady", b, "na") for b in (64, 512, 2048)],
}
LIMITS = {
    "buf": 65536, "sr": 768000, "block": 1 << 20, "d": 1 << 31, "pid": (1 << 32) - 1, "name_bytes": 200,
    "sample_pos": 1 << 40,
}  # fmt: skip
F1_VARIANTS = {  # F1 所列 case 里，这些 variant 各至少要有一格得出本格证据（dop 是可选项，不强制）
    "CB-05": ("bypass-out", "bypass-in", "deactivate", "disable-track"),
    "CB-04": ("rip", "freeze"),
}
A6_CASES = ("CB-07", "LU-06")
A9_CASES = ("CB-07", "LU-09")
F1_CASES = ("CB-01", "CB-02", "CB-04", "CB-05", "LU-02", "LU-03", "LU-04")
A5_COMBOS = (("au", "au"), ("vst3", "vst3"), ("au", "vst3"))

ITEM_NAMES = {
    "A1": "稳态对齐", "A2": "切换切片", "A3": "导出一致", "A4": "块长", "A5": "进程与后端",
    "A6": "轨道信息", "A7": "hostNs", "A8": "稳定性", "A9": "工程恢复",
}  # fmt: skip
VERDICT_ZH = {"PASS": "通过", "FAIL": "不通过", "INCOMPLETE": "数据不全"}

SPEC = {  # noqa: E501
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
    "rules": [[c, v, CAT_LABEL[r[0]], r[1], r[2]] for (c, v), r in sorted(RULES.items())],
    "guards": list(GUARDS),
    "compare_head": COMPARE_HEAD,
    "h_enums": {
        "role": ["I", "O"], "format": ["vst3", "au"], "host_kind": ["cubase", "luna", "other"],
        "os": ["win", "mac"], "arch": ["x64", "arm64"],
    },
    "event_args": {k: list(v) for k, v in EVENTS.items()},
    "limits": LIMITS,
    "f1_variants": {k: list(v) for k, v in F1_VARIANTS.items()},
    "thresholds": {
        "min_steady_blocks": MIN_STEADY_BLOCKS, "min_toggles": MIN_TOGGLES, "min_slice_blocks": MIN_SLICE_BLOCKS,
        "slice_ms": SLICE_NS // 1_000_000, "residual_limit_dbfs": RESIDUAL_LIMIT_DB, "stall_ms": STALL_NS // 1_000_000,
    },
    "exit_codes": {
        "0": "A1-A9 全部通过且 F1 满足", "1": "有不通过", "2": "格式或用法错误", "3": "无不通过但有数据不全或 F1 未满足",
        "4": "内部错误", "5": "--exclude 指定的段不存在或不唯一",
    },
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


class Incomplete(FormatError):
    """文件太短或没有合法的 H 行：实例刚创建就崩溃时的样子，不是格式错误，记入 A8 后其余文件照常分析。"""


_UINT = re.compile(r"^(?:0|[1-9][0-9]{0,18})$")
_SINT = re.compile(r"^-?(?:0|[1-9][0-9]{0,18})$")
_PCT = re.compile(r"^(?:[A-Za-z0-9._~-]|%[0-9A-Fa-f]{2}){0,4096}$")
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


def _trunc_pct(s, limit):
    """按解码后的字节数截断百分号编码文本（不拆开 %XX）。"""
    out, n, k = [], 0, 0
    while k < len(s) and n < limit:
        tok = s[k : k + 3] if s[k] == "%" else s[k]
        out.append(tok)
        k += len(tok)
        n += 1
    return "".join(out)


def _pct(s):
    if not _PCT.match(s):
        raise ValueError
    return _trunc_pct(s, LIMITS["name_bytes"])  # 超长的轨道名只用于比较，截断即可


def _ub(hi):
    def f(s):
        v = _uint(s)
        if v > hi:
            raise ValueError
        return v

    return f


def _sb(hi):
    def f(s):
        v = _sint(s)
        if abs(v) > hi:
            raise ValueError
        return v

    return f


def _buf(s):
    return s if s == "-" else _ub(LIMITS["buf"])(s)


V_BOOL = _enum("0", "1")
V_GROUP = _enum("A", "B", "C", "D", "E", "F", "G", "H", "-")
ARG_TYPES = {
    "uint": _uint, "sr": _ub(LIMITS["sr"]), "blk": _ub(LIMITS["block"]), "pid": _ub(LIMITS["pid"]), "bool": lambda s: int(V_BOOL(s)), "empty": _empty, "group": V_GROUP, "pct": _pct,
    "colour": _re(r"^(?:|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$"), "backend": _enum("win32", "posix"),
    "mode": _enum("realtime", "offline"),
}  # fmt: skip
ROW_VALIDATORS = {
    "H": [
        _enum("I", "O"), _enum("vst3", "au"), _enum("cubase", "luna", "other"), _enum("win", "mac"),
        _enum("x64", "arm64"), _ub(LIMITS["pid"]), _ub(LIMITS["pid"]), _re(r"^[0-9a-f]{4}$"), _re(r"^[0-9a-f]{8}$"),
    ],
    "C": [_uint, _enum(*CASES), _enum(*VARIANTS), _buf, _enum(*GUARDS), _ub(LIMITS["sr"])],
    "B": [
        _uint, _uint, _enum("I", "O"), _ub(LIMITS["block"]), _ub(LIMITS["block"]), _uint, lambda s: int(V_BOOL(s)),
        lambda s: int(V_BOOL(s)), lambda s: int(V_BOOL(s)), lambda s: int(V_BOOL(s)), _uint, _re(r"^[0-9a-f]{8}$"),
        _sb(LIMITS["sample_pos"]), _ub(LIMITS["sample_pos"]), _sb(LIMITS["sample_pos"]), _uint, _ub(LIMITS["d"]),
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
        self.pre_events = []  # 第一个 C 行之前的事件：实例级
        self.orphans = 0  # 早于第一个 C 行、没法归入任何格的块
        self.name = None  # 日志目录里的文件名：只用于匹配 --exclude，不进任何输出

    def span_ns(self):
        """文件里所有带时间戳的行（C、E、S、Z 的 t_ns，B 的 mono_ns）的首尾跨度；没有带时间戳的行是 0。"""
        ts = [r["t_ns"] for r in self.cases + self.events + self.summaries]
        ts += [b["mono_ns"] for b in self.blocks]
        if self.closing is not None:
            ts.append(self.closing["t_ns"])
        return max(ts) - min(ts) if ts else 0

    def is_short_clone(self):
        """没有 C 行、又活不到 SHORT_LIVED_NS 的实例：宿主为 Render in Place / Freeze 临时建的克隆，来不及写 1 Hz 摘要。"""
        return not self.cases and self.span_ns() < SHORT_LIVED_NS


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
        raise Incomplete(label, 1, "空文件")
    lines = text.split("\n")
    lg = Log(label)
    if lines[-1] == "":
        lines.pop()
    else:  # 最后一行没有换行：写到一半被截断，丢掉它并记下
        lines.pop()
        lg.truncated = True
    if not lines:
        raise Incomplete(label, 1, "只有写到一半的首行")
    if lines[0] != MAGIC:
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
            if (row["case"], row["variant"]) not in RULES:
                raise FormatError(label, ln, "case 与 variant 的组合不在规则表内")
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
        raise Incomplete(label, 2, "缺 H 行")
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
        row = _convert(label, ln, "比对", cols, vs, f)
        if (row["case"], row["variant"]) not in RULES:
            raise FormatError(label, ln, "case 与 variant 的组合不在规则表内")
        if (row["bit_exact"] == 1) != (row["residual_dbfs"] == float("-inf")):
            raise FormatError(label, ln, "bit_exact 与残差不自洽（逐位相同时残差须为 -inf，反之亦然）")
        rows.append(row)
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
        self.excluded = False  # --exclude 点名的误记段：留在 _segment 里算缺口，算完即拿出，不进任何判据

    def has(self, name):
        return any(e["name"] == name for e in self.events)


def key_label(key):
    return f"{key[0]}/{key[1]}/buf={key[2]}/guard={key[3]}"


class Session:
    def __init__(self, logs, compares, broken=(), exclude=()):
        """exclude = [(Log, 该 Log.cases 里的下标, 参数原文)]，由 resolve_excludes 解析而来。"""
        self.logs = logs
        self.compares = compares  # 比对行
        self.broken = list(broken)  # 不完整的日志文件（只有标签，没有内容）
        self.segments = []
        self.cells = {}
        self.orphans = 0
        self.excluded = []  # 被排除的段：{param, instance, role, cell, c_index}
        for lg in logs:
            self._segment(lg, [(k, param) for g, k, param in exclude if g is lg])
        for seg in self.segments:
            self.cells.setdefault(seg.key, {"I": [], "O": []})[seg.role].append(seg)
        self.dups = sorted(
            (k for k, c in self.cells.items() if k[1] not in MULTI_OK and (len(c["I"]) > 1 or len(c["O"]) > 1)),
            key=lambda k: (k[0], k[1], str(k[2]), k[3]),
        )

    def _segment(self, lg, exclude=()):
        times = [c["t_ns"] for c in lg.cases]
        order = sorted(range(len(times)), key=lambda i: times[i])
        times = [times[i] for i in order]
        segs = [Segment(lg, lg.cases[i]) for i in order]
        where = {c: i for i, c in enumerate(order)}  # C 行在文件里的下标 → 按时间排序后的段号
        for k, _ in exclude:
            segs[where[k]].excluded = True
        at = lambda t: (bisect_right(times, t) - 1)  # noqa: E731
        for e in lg.events:
            i = at(e["t_ns"])
            if i >= 0:
                segs[i].events.append(e)
            else:
                lg.pre_events.append(e)
        prev = prev_i = None
        for b in lg.blocks:
            i = at(b["mono_ns"])
            if i >= 0:
                segs[i].blocks.append(b)
                if prev is not None and b["seq"] != prev + 1:
                    # 缺口记给后一块所在的段；但缺口前后的块之间若隔着被排除的段（该段的块被探针按丢弃计、占了序号，
                    # 或它自己的尾部丢了块），这处缺口就是被排除段的，不能让相邻的段替它背
                    owner = i
                    if not segs[i].excluded:
                        hidden = [k for k in range(max(prev_i, 0), i) if segs[k].excluded]
                        owner = hidden[-1] if hidden else i
                    segs[owner].gaps += 1
            else:
                lg.orphans += 1
                self.orphans += 1
            prev, prev_i = b["seq"], i
        dropped = max([s["dropped"] for s in lg.summaries] + [lg.closing["dropped_total"] if lg.closing else 0])
        if dropped and not any(s.gaps for s in segs):
            # 探针报告丢过记录、却定位不到缺口：有被排除的段就记在它们身上（现场排除的正是没拿到绑定的误记段），否则整个实例都按缺口算
            for s in [s for s in segs if s.excluded] or segs:
                s.gaps += 1
        for k, param in exclude:
            seg = segs[where[k]]
            self.excluded.append({
                "param": param, "instance": self.logs.index(lg) + 1, "role": lg.header["role"],
                "cell": key_label(seg.key), "c_index": k + 1,
            })  # fmt: skip
        self.segments += [s for s in segs if not s.excluded]


# ---------------------------------------------------------------------------------------------------
# 判据
# ---------------------------------------------------------------------------------------------------


def fold(item, parts, cover=()):
    """parts = [(状态, 说明)]；状态 NOTE 只补充说明、不参与判定。
    可信数据里的不通过压过数据不全：A1 的停工条件不能被别处的缺口盖住。"""
    real = [p for p in parts if p[0] != "NOTE"]
    states = [s for s, _ in real]
    if "FAIL" in states:
        v = "FAIL"
    elif not real or "INCOMPLETE" in states:
        v = "INCOMPLETE"
    else:
        v = "PASS"
    notes = [n for s, n in parts if n and (s in ("NOTE", "FAIL", "INCOMPLETE") or v == "PASS")]
    if not real:
        notes.append("没有可用于本项的数据")
    if len(notes) > 4:
        notes = notes[:3] + [f"另有 {len(notes) - 3} 条同类记录"]
    return {"id": item, "name": ITEM_NAMES[item], "verdict": v, "notes": notes, "cover": sorted(set(cover))}


def _cells(s):
    """按键排序的格；buf 可能是整数也可能是 "-"，统一成字符串再排。"""
    return sorted(s.cells.items(), key=lambda kv: (kv[0][0], kv[0][1], str(kv[0][2]), kv[0][3]))


def _usable(seg, need=None):
    """分段是否有可信的块数据：返回 (None, 说明) 表示可用，否则 ("INCOMPLETE", 说明)。"""
    need = MIN_STEADY_BLOCKS if need is None else need  # 在调用时取全局值，变异检查才改得到
    if seg.gaps:
        return "INCOMPLETE", f"{key_label(seg.key)} {seg.role} 序号缺口 {seg.gaps} 处"
    if len(seg.blocks) < need:
        return "INCOMPLETE", f"{key_label(seg.key)} {seg.role} 块数 {len(seg.blocks)} 不足 {need}"
    return None, ""


def _cell_pair(key, cell):
    """一个格应恰有一个 In 和一个 Out 实例：返回 (i, o, 问题说明)。"""
    ni, no = len(cell["I"]), len(cell["O"])
    if ni > 1 or no > 1:
        return None, None, f"{key_label(key)} 同键重复记录（In {ni}、Out {no}），旧文件须挪走后重采"
    if not ni or not no:
        return None, None, f"{key_label(key)} 只有 {'In' if ni else 'Out'} 一端"
    return cell["I"][0], cell["O"][0], None


def judge_a1(s):
    parts, cover, skipped = [], [], 0
    for key, cell in _cells(s):
        if key[0] not in A1_CASES:
            continue
        if key[1] == "toggle":  # 切换格归 A2，这里不计，但要说清楚
            skipped += 1
            continue
        cover.append(key[0])
        i, o, why = _cell_pair(key, cell)
        if why:
            parts.append(("INCOMPLETE", why))
            continue
        for seg in (i, o):
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
    if skipped:
        parts.append(("NOTE", f"{skipped} 个 variant=toggle 的格归 A2，未计入 A1"))
    return fold("A1", parts, cover)


def switch_points(seg):
    """A2 的切换点：显式的监听 / 录音使能事件，加上 In 端 B 行 thread 的变化点。
    VST3 不通知监听键与录音使能键，探针从不写这两个事件；ASIO-Guard 开着时，这类切换会把轨从预取线程挪到实时线程，
    所以相邻两块的 thread 散列不同，就是一次处理路径切换，切换时刻取新线程上的第一块的 mono_ns。
    显式事件前后 SLICE_NS 内的 thread 变化算同一次切换，不重复数。块长 n 的变化稳态下也会抖，只作旁证，不当切换。
    返回 (显式切换时刻, 推断切换时刻, n 变化次数)。"""
    explicit = sorted(e["t_ns"] for e in seg.events if e["name"] in TOGGLES)
    blocks = sorted(seg.blocks, key=lambda b: b["seq"])
    changed = [b["mono_ns"] for a, b in zip(blocks, blocks[1:]) if a["thread"] != b["thread"]]
    inferred = [t for t in changed if not any(abs(t - e) < SLICE_NS for e in explicit)]
    n_changes = sum(1 for a, b in zip(blocks, blocks[1:]) if a["n"] != b["n"])
    return explicit, inferred, n_changes


def judge_a2(s):
    parts, cover, per_case = [], [], {}
    for key, cell in _cells(s):
        if key[0] not in A2_CASES or key[1] != "toggle":
            continue
        cover.append(key[0])
        label = key_label(key)
        if len(cell["I"]) != 1:
            parts.append(("INCOMPLETE", f"{label} In 端记录数为 {len(cell['I'])}，应恰有 1 个"))
            continue
        seg = cell["I"][0]
        explicit, inferred, n_changes = switch_points(seg)
        ts = sorted(explicit + inferred)
        got = per_case.setdefault(key[0], [0, 0])
        got[0] += len(explicit)
        got[1] += len(inferred)
        how = f"显式事件 {len(explicit)}、按处理线程切换推断 {len(inferred)}"
        bad, why = _usable(seg)
        if bad:
            parts.append((bad, why))
            continue
        if not ts:
            parts.append(("INCOMPLETE", f"{label} 这一格里处理线程没有变化（ASIO-Guard 关着时监听可能不改处理线程），也没有显式切换事件"))
            continue
        worst, thin = 0, 0
        for i, t in enumerate(ts):
            end = min(ts[i + 1] if i + 1 < len(ts) else t + SLICE_NS, t + SLICE_NS)
            window = [b for b in seg.blocks if t <= b["mono_ns"] < end]
            if len(window) < MIN_SLICE_BLOCKS:
                thin += 1
            worst = max(worst, sum(b["rs"] for b in window))
        if thin:
            parts.append(("INCOMPLETE", f"{label} 有 {thin} 次切换后的窗口里块数不足 {MIN_SLICE_BLOCKS}（切换 {len(ts)} 次：{how}）"))
            continue
        ds = {b["d"] for b in seg.blocks}
        detail = f"切换 {len(ts)} 次（{how}），块长 n 变化 {n_changes} 次（旁证，不计入切换）"
        if worst > 1 or len(ds) > 1:
            parts.append(("FAIL", f"{label} 每次切换后重同步最多 {worst} 次、D 取值 {len(ds)} 种；{detail}"))
        else:
            parts.append(("PASS", f"{label} {detail}"))
    for case, (n_explicit, n_inferred) in sorted(per_case.items()):
        n = n_explicit + n_inferred
        if 0 < n < MIN_TOGGLES:  # 一次都没有的格已经各自说明过原因
            parts.append(("INCOMPLETE", f"{case} 只有 {n} 次切换（显式事件 {n_explicit}、按处理线程切换推断 {n_inferred}），不足 {MIN_TOGGLES}"))
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
        if not any(r["variant"] in A3_REQUIRED[case] for r in judged):
            need = "/".join(A3_REQUIRED[case])
            parts.append(("INCOMPLETE", f"{case} 缺导出比对（需要 variant 为 {need} 的行）"))
        for r in judged:
            ok = r["lag_samples"] == 0 and (r["bit_exact"] == 1 or r["residual_dbfs"] <= RESIDUAL_LIMIT_DB)
            head = "逐位相同" if r["bit_exact"] else f"残差 {r['residual_dbfs']:.1f} dBFS"
            msg = f"{case}/{r['variant']} {head}，lag {r['lag_samples']}，开头单列 {r['head_residual_dbfs']:.1f} dBFS"
            parts.append(("PASS" if ok else "FAIL", msg))
    return fold("A3", parts, cover)


def _length_parts(s, key, segs, need):
    """块长检查：每块对照它之前最近一次 prepare 声明的最大块长（中途改缓冲会重新 prepare）。"""
    parts = []
    for seg in segs:
        if not seg.blocks:
            continue
        bad, why = _usable(seg, need)
        if bad:
            parts.append((bad, why))
            continue
        preps = sorted((e["t_ns"], e["b"]) for e in seg.log.events if e["name"] == "prepare")
        times = [t for t, _ in preps]

        def limit(b):
            k = bisect_right(times, b["mono_ns"]) - 1
            return preps[k][1] if k >= 0 else None

        if any(limit(b) is None for b in seg.blocks):
            parts.append(("INCOMPLETE", f"{key_label(key)} {seg.role} 有块早于任何 prepare 事件"))
            continue
        over = sum(1 for b in seg.blocks if b["n"] > limit(b) or b["n"] > b["n_decl"])
        ns = [b["n"] for b in seg.blocks]
        msg = f"{key_label(key)} {seg.role} n∈[{min(ns)},{max(ns)}]，prepare 共 {len(preps)} 次，超限 {over}"
        parts.append(("FAIL" if over else "PASS", msg))
    return parts


def judge_a4(s):
    """A4 只判块长。成对满块的格给通过 / 不通过；其余格只能给不通过或说明，不能单独撑出通过。"""
    parts, cover, concluded = [], [], False
    for key, cell in _cells(s):
        has_blocks = any(g.blocks for g in cell["I"] + cell["O"])
        if is_cat1(key):
            if "A4" not in feeds_of(key) or not has_blocks:
                continue
            cover.append(key[0])
            i, o, why = _cell_pair(key, cell)
            if why:
                parts.append(("INCOMPLETE", why))
                continue
            for st, note in _length_parts(s, key, [i, o], None):
                parts.append((st, note))
                concluded = concluded or st in ("PASS", "FAIL")
        elif has_blocks:
            cover.append(key[0])
            for st, note in _length_parts(s, key, cell["I"] + cell["O"], 1):  # 单端或克隆的实例各查各的
                if st == "FAIL":
                    parts.append((st, note))
                elif st == "INCOMPLETE":
                    parts.append(("NOTE", note))
    if not concluded and not any(st == "FAIL" for st, _ in parts):
        parts.append(("INCOMPLETE", "没有任何成对满块的格得出块长结论"))
    return fold("A4", parts, cover)


def judge_a5(s):
    parts, cover, seen = [], [], set()
    for key, cell in _cells(s):
        if key[0] != "LU-02":
            continue
        cover.append("LU-02")
        i, o, why = _cell_pair(key, cell)
        if why:
            parts.append(("INCOMPLETE", why))
            continue
        combo = (i.log.header["format"], o.log.header["format"])
        seen.add(combo)
        # 配对与后端的事件常发生在用户选 case 之前，所以实例级事件与分段事件一起看
        ei, eo = i.events + i.log.pre_events, o.events + o.log.pre_events
        same = i.log.header["pid"] == o.log.header["pid"]
        paired = any(e["name"] == "paired" and e["a"] == o.log.header["pid"] for e in ei)
        denied = any(e["name"] == "shm_denied" or (e["name"] == "backend" and e["b"] != 0) for e in ei + eo)
        ok = same and paired and not denied
        msg = f"{combo[0]}+{combo[1]}：同 pid {int(same)}、已配对 {int(paired)}、kShmDenied {int(denied)}"
        parts.append(("PASS" if ok else "FAIL", msg))
    for combo in A5_COMBOS:
        if combo not in seen and cover:
            parts.append(("INCOMPLETE", f"缺组合 {combo[0]}+{combo[1]}"))
    return fold("A5", parts, cover)


def judge_a6(s, salt=b"x"):
    parts, cover = [], []
    for key, cell in _cells(s):
        if key[0] not in A6_CASES or key[1] != "rename":
            continue
        segs = cell["I"] + cell["O"]
        last = []
        for g in segs:
            props = [e for e in g.events if e["name"] == "track_props"]
            last.append(max(props, key=lambda e: e["t_ns"]) if props else None)
        cover.append(key[0])
        label = key_label(key)
        if not any(last):
            parts.append(("INCOMPLETE", f"{label} 没有任何 track_props 事件（契约要求每个 C 行之后补写一条快照）"))
            continue
        # 逐实例检查：一个好实例不能掩盖坏实例
        empty = sum(1 for e in last if e is not None and not e["a"])
        if empty:
            parts.append(("FAIL", f"{label} 有 {empty} 个实例没有收到轨道名"))
        if any(e is None for e in last) or {g.role for g in segs} != {"I", "O"}:
            parts.append(("INCOMPLETE", f"{label} 有实例没有记录轨道信息"))
            continue
        if len(cell["I"]) > 1 or len(cell["O"]) > 1:
            parts.append(("INCOMPLETE", f"{label} 同键重复记录，旧文件须挪走后重采"))
            continue
        if empty:
            continue
        ends = {g.role: e for g, e in zip(segs, last)}
        verdict, h = name_verdict(ends["I"]["a"], ends["O"]["a"], salt)
        colour = "有" if all(e["b"] for e in ends.values()) else "缺"
        parts.append(("PASS", f"{label} 名字送达 1（{verdict}" + (f"，散列 {h}" if h else "") + f"），颜色{colour}"))
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
    for key, cell in _cells(s):
        if not (is_cat1(key) and "A7" in feeds_of(key)) or not any(g.blocks for g in cell["I"] + cell["O"]):
            continue
        cover.append(key[0])
        i, o, why = _cell_pair(key, cell)
        if why:
            parts.append(("INCOMPLETE", why))
            continue
        bad = next((x for x in (_usable(i), _usable(o)) if x[0]), None)
        if bad:
            parts.append(bad)
            continue
        playing = [b for sg in (i, o) for b in sg.blocks if b["playing"]]
        if not playing:
            parts.append(("INCOMPLETE", f"{key_label(key)} 两端都没有播放中的块"))
            continue
        valid = sum(b["host_ns_valid"] for b in playing)
        if valid / len(playing) < 0.99:
            parts.append(("FAIL", f"{key_label(key)} hostNs 有效率 {valid}/{len(playing)}，不足 99%"))
            continue
        pairs = _pairs(i.blocks, o.blocks)
        both = [(a[2], b[2]) for a, b in pairs if all(x[2]["playing"] and x[2]["host_ns_valid"] for x in (a, b))]
        if len(both) < MIN_STEADY_BLOCKS:
            n = len(pairs)
            parts.append(("INCOMPLETE", f"{key_label(key)} 交错成对的回调 {n}、其中两端都在播放且 hostNs 有效的 {len(both)}，不足 {MIN_STEADY_BLOCKS}"))
            continue
        mismatch = sum(1 for a, b in both if a["host_ns"] != b["host_ns"])
        nonmono = 0
        for sg in (i, o):
            seq = [b["host_ns"] for b in sorted(sg.blocks, key=lambda b: b["seq"]) if b["playing"] and b["host_ns_valid"]]
            nonmono += sum(1 for x, y in zip(seq, seq[1:]) if y <= x)
        first = sum(1 for a, _ in pairs if a[1] == "I")
        ok = mismatch / len(both) <= HOSTNS_MISMATCH_RATIO and nonmono == 0
        msg = f"{key_label(key)} 配对 {len(both)}，两端不等 {mismatch}，非递增 {nonmono}，In 先于 Out {first}/{len(pairs)}"
        parts.append(("PASS" if ok else "FAIL", msg))
    return fold("A7", parts, cover)


def short_clones(s):
    """[(实例序号, Log)]：没有 C 行、又活不到 SHORT_LIVED_NS 的实例（Render in Place / Freeze 的临时克隆）。"""
    return [(n, lg) for n, lg in enumerate(s.logs, start=1) if lg.is_short_clone()]


def _clone_list(clones):
    shown = "、".join(f"实例 {n}（{lg.header['role']}，{lg.span_ns() // 1_000_000} ms）" for n, lg in clones[:6])
    return shown + (f" 等共 {len(clones)} 个" if len(clones) > 6 else "")


def judge_a8(s):
    parts, clones = [], []
    for label in s.broken:
        parts.append(("FAIL", f"{label} 只有不完整的内容（实例刚创建就崩溃，或日志写到一半被截断）"))
    for n, lg in enumerate(s.logs, start=1):
        who = f"实例 {n}（{lg.header['role']}）"
        ts = [x["t_ns"] for x in lg.summaries]
        stall = max([b - a for a, b in zip(ts, ts[1:])] + [0])
        trips = max([x["trips"] for x in lg.summaries] + [0])
        problems = []
        if lg.truncated or lg.closing is None:
            problems.append("日志没有收尾（疑似崩溃或被强杀）")
        if stall > STALL_NS:
            problems.append(f"摘要间隔超限（最长 {stall // 1_000_000} ms，阈值 {STALL_NS // 1_000_000} ms；也可能是写摘要的线程被宿主占住）")
        if trips:
            problems.append(f"跳闸 {trips} 次")
        if problems:  # 克隆实例也一样：没收尾、跳闸都照常不通过，只豁免「摘要行不足」
            parts.append(("FAIL", f"{who} " + "；".join(problems)))
        elif len(ts) < 2 and lg.is_short_clone():
            clones.append((n, lg))
        elif len(ts) < 2:
            parts.append(("INCOMPLETE", f"{who} 摘要行不足 2 条，无法判断卡死"))
        else:
            parts.append(("PASS", ""))
    if clones:  # 汇成一条放在最后，免得挤掉前面真正的不通过说明
        parts.append(("NOTE", f"{len(clones)} 个没有 C 行、存活短于 {SHORT_LIVED_NS // 1_000_000} ms 的实例不计入稳定性判定"
                              f"（疑似 Render in Place / Freeze 的克隆，来不及写摘要）：{_clone_list(clones)}"))  # fmt: skip
    return fold("A8", parts, [g.case["case"] for g in s.segments])


def judge_a9(s):
    parts, cover = [], []
    for key, cell in _cells(s):
        if key[0] not in A9_CASES or key[1] != "reopen":
            continue
        cover.append(key[0])
        if len(cell["I"]) != 1:
            parts.append(("INCOMPLETE", f"{key_label(key)} In 端记录数为 {len(cell['I'])}，应恰有 1 个；同键重复的旧文件须挪走后重采"))
            continue
        seg = cell["I"][0]
        if seg.gaps:
            parts.append(("INCOMPLETE", f"{key_label(key)} 序号缺口"))
            continue
        evs = sorted(seg.events + seg.log.pre_events, key=lambda e: e["t_ns"])  # 重开后的事件多在选 case 之前
        sets = [e["t_ns"] for e in evs if e["name"] == "set_state"]
        pairs = [e["t_ns"] for e in evs if e["name"] == "paired"]
        ok = bool(sets) and any(p >= min(sets) for p in pairs)
        order = ">".join(e["name"] for e in evs if e["name"] in ("prepare", "set_state", "first_block", "first_paired_block", "paired"))  # fmt: skip
        parts.append(("PASS" if ok else "FAIL", f"{key_label(key)} 事件顺序 {order or '无'}"))
    return fold("A9", parts, cover)


JUDGES = [("A1", judge_a1), ("A2", judge_a2), ("A3", judge_a3), ("A4", judge_a4), ("A5", judge_a5),
          ("A6", judge_a6), ("A7", judge_a7), ("A8", judge_a8), ("A9", judge_a9)]  # fmt: skip


def _case_has_data(s, case):
    """F1 要的是能下结论的数据，而不是一个 C 行：CB-04、LU-04 可只靠导出比对行；LU-02 看配对与后端事件；其余要有块。"""
    if any(r["case"] == case for r in s.compares):
        return True
    segs = [g for g in s.segments if g.case["case"] == case]
    if case == "LU-02":
        return any(e["name"] in ("paired", "backend") for g in segs for e in g.events + g.log.pre_events)
    return any(g.blocks for g in segs)


def _matrix_missing(s, case):
    out = []
    for variant, buf, guard in MATRIX[case]:
        cell = s.cells.get((case, variant, buf, guard))
        ok = bool(cell) and len(cell["I"]) == 1 and len(cell["O"]) == 1
        ok = ok and all(_usable(g)[0] is None for g in (cell["I"] + cell["O"] if cell else []))
        if not ok:
            out.append(f"{variant}/buf={buf}/guard={guard}")
    return out


def freeze_f1(s, items, obs):
    missing = []
    for case in F1_CASES:
        segs = [g for g in s.segments if g.case["case"] == case]
        if not _case_has_data(s, case):
            missing.append(f"{case} 无数据")
        elif any(g.gaps for g in segs):
            missing.append(f"{case} 有序号缺口")
    for case in MATRIX:
        gone = _matrix_missing(s, case)
        if gone:
            shown = "、".join(gone[:6]) + (f" 等共 {len(gone)} 格" if len(gone) > 6 else "")
            missing.append(f"{case} 矩阵缺格：{shown}")
    if s.orphans:
        missing.append(f"有 {s.orphans} 个块不属于任何测试格")
    f1_dups = [k for k in s.dups if k[0] in F1_CASES]  # s.dups 已排除允许多实例的 variant
    if f1_dups:
        missing.append(f"{len(f1_dups)} 个格同键重复记录")
    for case, variants in F1_VARIANTS.items():
        for v in variants:
            if not any(c["case"] == case and c["variant"] == v and c["state"] == "PASS" for c in obs["cells"]):
                missing.append(f"{case} 缺 {v} 格的本格证据")
    for case in F1_CASES:  # 格观察里本格证据缺失的格，所在 case 在 F1 里就是缺数据
        if any(c["state"] == "INCOMPLETE" and c["case"] == case for c in obs["cells"]):
            missing.append(f"{case} 格观察数据不全")
    for it in items[:8]:  # A1-A8 必须有结论；A9（工程恢复）不阻断 F1
        if it["verdict"] == "INCOMPLETE":
            missing.append(f"{it['id']} 数据不全")
    return {"ok": not missing, "missing": missing}


def warnings_of(s):
    out = []
    if s.orphans:
        out.append(f"{s.orphans} 个块早于所在日志的第一个 C 行，未计入任何测试格")
    if s.dups:
        out.append(f"{len(s.dups)} 个格同键重复记录（如 {key_label(s.dups[0])}）：旧文件须挪走后重采，相关判据按数据不全处理")
    if s.broken:
        out.append(f"{len(s.broken)} 个日志文件不完整，已记入 A8")
    lost = [n for n, lg in enumerate(s.logs, start=1)
            if max([x["dropped"] for x in lg.summaries] + [lg.closing["dropped_total"] if lg.closing else 0])]  # fmt: skip
    if lost:
        out.append(f"{len(lost)} 个实例的探针报告过丢弃记录，相关格按数据不全处理")
        both = [n for n in lost if any(x["instance"] == n for x in s.excluded)]
        if both:
            out.append(f"其中 {len(both)} 个实例有被排除的段：定位不到缺口的丢弃记在被排除的段上，其余各格只看自己段内的序号缺口")
    return out


def _cell_evidence(s, key, cell):
    """类别 2 / 3 的格自己的证据。类别 2 一条都没有就是数据不全；类别 3 只留一条说明。返回 [(状态, 说明)]。"""
    v, segs = key[1], cell["I"] + cell["O"]
    label = key_label(key)
    cat = _rule(key)[0]
    if cat == 2 and key in s.dups:  # 同键重采没挪走旧文件：证据不能在两份记录里各取所需
        return [("INCOMPLETE", f"{label} 同键重复记录（In {len(cell['I'])}、Out {len(cell['O'])}），旧文件须挪走后重采")]
    if cat == 3:
        return [("NOTE", f"{label} 只记录：{sum(len(g.blocks) for g in segs)} 块、{sum(len(g.events) for g in segs)} 个事件")]
    if v not in ("deactivate", "disable-track", "bypass-in", "bypass-out", "smart", "dup", "undo", "twoproj",
                 "coreaudio", "rip", "freeze") and not v.startswith("misplaced-"):  # fmt: skip
        return []  # 其余格的证据由 A3、A5、A6、A9 各自下结论
    if any(g.gaps for g in segs):
        return [("INCOMPLETE", f"{label} 序号缺口")]
    full = [g for g in segs if len(g.blocks) >= MIN_STEADY_BLOCKS]
    evs = [e for g in segs for e in g.events + g.log.pre_events]
    here = [e for g in segs for e in g.events]  # 只数本段（本格 C 行到下一个 C 行或文件尾）内的事件，实例级的 pre_events 不算
    if v in ("deactivate", "disable-track"):
        # JUCE 的 VST3 包装层把 setProcessing(false) 转成 reset()，探针记 reset 而不记 set_processing；setActive(false) 才有 release
        found = {
            "set_processing(0)": sum(1 for e in here if e["name"] == "set_processing" and e["a"] == 0),
            "reset": sum(1 for e in here if e["name"] == "reset"),
            "release": sum(1 for e in here if e["name"] == "release"),
        }
        if not full:
            return [("INCOMPLETE", f"{label} 没有一端达到 {MIN_STEADY_BLOCKS} 块")]
        if not any(found.values()):
            return [("INCOMPLETE", f"{label} 本段内缺停用的生命周期事件（set_processing(0)、reset、release 都没有）")]
        seen = "、".join(f"{k} {n}" for k, n in found.items() if n)
        return [("PASS", f"{label} 单端 {len(full)} 个实例有块，本段内停用事件在（{seen}）")]
    if v in ("bypass-in", "bypass-out"):
        if not full:
            return [("INCOMPLETE", f"{label} 没有一端达到 {MIN_STEADY_BLOCKS} 块")]
        if not any(b["bypassed"] for g in full for b in g.blocks):
            return [("INCOMPLETE", f"{label} 没有 bypassed 为 1 的块")]
        return [("PASS", f"{label} 旁路块在")]
    if v == "smart":
        pings = sum(1 for e in here if e["name"] == "ping")
        if pings < 2:
            return [("INCOMPLETE", f"{label} 本段内只有 {pings} 条 ping，前后各一次需要 2 条")]
        return [("PASS", f"{label} ping {pings} 条")]
    if v in ("rip", "freeze"):
        if not any(g.blocks or g.events for g in segs):
            return [("INCOMPLETE", f"{label} 两端都没有记录")]
        return [("PASS", f"{label} 涉及 {len({id(g.log) for g in segs})} 个实例日志")]
    if v == "coreaudio":
        if not evs:
            return [("INCOMPLETE", f"{label} 没有任何事件")]
        return [("PASS", f"{label} 有 {len(evs)} 个事件")]
    if v.startswith("misplaced-"):
        if not any(g.blocks or g.events for g in segs):
            return [("INCOMPLETE", f"{label} 两端都没有记录")]
        return [("PASS", f"{label} 有记录，paired 事件 {sum(1 for e in evs if e['name'] == 'paired')} 条")]
    # dup / undo / twoproj：逐个 In 实例检查，一个配好的实例不能掩盖没配好的。
    # 同一个宿主里两端是同一个 pid，paired 事件的对端 pid 区分不了实例，所以只看有没有 paired，twoproj 另看实例数相等
    if not cell["I"]:
        return [("INCOMPLETE", f"{label} 没有 In 实例")]
    unpaired = sum(1 for g in cell["I"] if not any(e["name"] == "paired" for e in g.events + g.log.pre_events))
    stray = abs(len(cell["I"]) - len(cell["O"])) if v == "twoproj" else 0
    if unpaired or stray:
        return [("INCOMPLETE", f"{label} 有 {unpaired} 个 In 实例缺 paired 事件，In 与 Out 实例数相差 {stray}")]
    return [("PASS", f"{label} {len(cell['I'])} 个 In 实例都已配对")]


def _gap_ms(blocks):
    ts = sorted(b["mono_ns"] for b in blocks)
    return max([(b - a) // 1_000_000 for a, b in zip(ts, ts[1:])] + [0])


def observations(s):
    """格观察：不带 A 编号、不阻断。逐格给本格证据，并把采到的数据算出来（同通道顺序、旁路、Smart、错放）。"""
    cells, bypass, smart, misplaced = [], [], [], []
    pairs_total = first_total = 0
    for key, cell in _cells(s):
        label = key_label(key)
        segs = cell["I"] + cell["O"]
        if is_cat1(key):
            if len(cell["I"]) == 1 and len(cell["O"]) == 1:
                pr = _pairs(cell["I"][0].blocks, cell["O"][0].blocks)
                pairs_total += len(pr)
                first_total += sum(1 for a, _ in pr if a[1] == "I")
        else:
            cells += [
                {"case": key[0], "variant": key[1], "cell": label, "state": st, "note": n}
                for st, n in _cell_evidence(s, key, cell)
            ]
        v = key[1]
        if v in ("bypass-in", "bypass-out"):
            stalls = regress = jumps = 0
            for g in cell["O"]:
                bl = sorted(g.blocks, key=lambda b: b["seq"])
                for a, b in zip(bl, bl[1:]):
                    if b["seq"] != a["seq"] + 1:
                        continue
                    d = b["w"] - a["w"]
                    stalls += d == 0 and a["n"] != 0  # 0 样本回调不推进写头，下一块的 w 本来就相等
                    regress += d < 0
                    jumps += d > 0 and d != a["n"]  # w 是累计帧数，相邻差应等于上一块的 n
            byp = {r: sum(b["bypassed"] for g in segs if g.role == r for b in g.blocks) for r in ("I", "O")}
            bypass.append({"cell": label, "bypassed_blocks": byp, "o_w_stalls": stalls, "o_w_regress": regress, "o_w_jumps": jumps})
        elif v == "smart":
            blocks = [b for g in cell["I"] for b in g.blocks]
            smart.append({
                "cell": label, "max_gap_ms": _gap_ms(blocks), "resync_total": sum(b["rs"] for b in blocks),
                "pings": sum(1 for g in segs for e in g.events if e["name"] == "ping"),  # 与本格证据一样只数本段内的
            })  # fmt: skip
        elif v.startswith("misplaced-"):
            ib, ob = [b for g in cell["I"] for b in g.blocks], [b for g in cell["O"] for b in g.blocks]
            if not ib or not ob:
                misplaced.append({"cell": label, "verdict": "无法判断"})
                continue
            pr = _pairs(ib, ob)
            misplaced.append({
                "cell": label, "verdict": "两端都在", "pairs": len(pr),
                "in_first": sum(1 for a, _ in pr if a[1] == "I"),
                "host_ns_equal": sum(1 for a, b in pr if a[2]["host_ns_valid"] and b[2]["host_ns_valid"] and a[2]["host_ns"] == b[2]["host_ns"]),
                "off_in": sorted({b["off"] for b in ib}), "off_out": sorted({b["off"] for b in ob}),
            })  # fmt: skip
    bad = any(c["state"] == "INCOMPLETE" for c in cells)
    return {
        "status": "INCOMPLETE" if bad else "PASS",
        "order": {"pairs": pairs_total, "in_first": first_total},
        "cells": cells, "bypass": bypass, "smart": smart, "misplaced": misplaced,
    }


def render_observations(obs):
    o = obs["order"]
    out = ["", "## 格观察", "", "不带判据编号、不阻断；本格证据缺失的格记「数据不全」。", ""]
    out.append(f"- 同通道顺序：成对满块的格里，按 call_seq 是 In 先于 Out 的回调 {o['in_first']}/{o['pairs']}")
    for c in obs["cells"]:
        out.append(f"- [{VERDICT_ZH.get(c['state'], '说明')}] {c['note']}")
    for b in obs["bypass"]:
        out.append(f"- 旁路 {b['cell']}：旁路块 In {b['bypassed_blocks']['I']}、Out {b['bypassed_blocks']['O']}；"
                   f"Out 的 w 停滞 {b['o_w_stalls']} 次、回退 {b['o_w_regress']} 次、跳变 {b['o_w_jumps']} 次（相邻差不等于上一块的 n）")  # fmt: skip
    for m in obs["smart"]:
        out.append(f"- Smart {m['cell']}：最长回调间隙 {m['max_gap_ms']} ms，重同步 {m['resync_total']} 次（含 stall_resume），ping {m['pings']} 条")
    for m in obs["misplaced"]:
        if m["verdict"] == "无法判断":
            out.append(f"- 错放 {m['cell']}：只有一端有块，无法判断")
        else:
            out.append(f"- 错放 {m['cell']}：成对 {m['pairs']}，In 先于 Out {m['in_first']}，hostNs 相等 {m['host_ns_equal']}；"
                       f"off In {m['off_in']}、Out {m['off_out']}")  # fmt: skip
    return out


def extras(s):
    """只记录、不下结论的项：RIP/Freeze 是否克隆实例、Ping 一致性。"""
    out = []
    clones = short_clones(s)
    if clones:
        out.append(f"没有 C 行、存活短于 {SHORT_LIVED_NS // 1_000_000} ms 的实例 {len(clones)} 个（疑似 Render in Place / Freeze 的克隆，"
                   f"不属于任何格，rip / freeze 格的实例数里没有它们）：{_clone_list(clones)}")  # fmt: skip
    for key, cell in _cells(s):
        if key[1] in CLONE_VARIANTS:
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
    logs, compares, broken, files = [], [], [], 0
    for nm in names:
        full = os.path.join(path, nm)
        if not os.path.isfile(full):
            continue
        if nm.startswith("probe-") and nm.endswith(".csv"):
            files += 1
            label = f"日志#{files}"
            with open(full, "rb") as f:
                try:
                    logs.append(parse_log(f.read(), label))
                    logs[-1].name = nm
                except Incomplete:
                    broken.append(label)
        elif nm.startswith("compare-") and nm.endswith(".csv"):
            label = f"比对#{len(compares) + 1}"
            with open(full, "rb") as f:
                compares += parse_compare(f.read(), label)
    return logs, compares, broken


class ExcludeError(Exception):
    """--exclude 指向的文件或 C 行找不到、不唯一（退出码 5）。消息只带参数的序号，不回显文件名。"""


_EXCLUDE_SEL = re.compile(r"^(#|t=)?(0|[1-9][0-9]{0,18})$")


def parse_excludes(raw):
    """['文件名:选择子', ...] → [(文件名, 前缀, 数值, 参数)]；写法不对抛 ValueError（用法错误，退出码 2）。
    文件名只取最后一级（带了目录也行）；选择子是 C 行序号或 t_ns，`#N` 强制按序号、`t=N` 强制按 t_ns。"""
    out = []
    for text in raw:
        name, sep, sel = text.rpartition(":")
        name = re.split(r"[\\/]", name)[-1]
        m = _EXCLUDE_SEL.match(sel)
        if not sep or not name or not m:
            raise ValueError
        out.append((name, m.group(1) or "", int(m.group(2)), f"{name}:{sel}"))
    return out


def resolve_excludes(logs, specs):
    """→ [(Log, 该 Log.cases 里的下标, 参数)]。裸数字先按 t_ns 找、找不到再当序号；两种读法指向不同的 C 行就报错，不替用户猜。"""
    out = []
    for n, (name, prefix, value, param) in enumerate(specs, start=1):
        lg = next((g for g in logs if g.name == name), None)
        if lg is None:
            raise ExcludeError(f"第 {n} 个参数：日志目录里没有这个文件（或它是不完整的文件）")
        by_t = [k for k, c in enumerate(lg.cases) if c["t_ns"] == value]
        by_n = [value - 1] if 1 <= value <= len(lg.cases) else []
        if prefix == "#":
            hit = by_n
        elif prefix == "t=":
            hit = by_t
        else:
            if by_t and by_n and by_t != by_n:
                raise ExcludeError(f"第 {n} 个参数：这个数既是某个 C 行的 t_ns、又是另一个 C 行的序号，请写成 #N（序号）或 t=N（t_ns）")
            hit = by_t or by_n
        if not hit:
            raise ExcludeError(f"第 {n} 个参数：这个文件里没有这样的 C 行（该文件共 {len(lg.cases)} 个 C 行）")
        if len(hit) > 1:
            raise ExcludeError(f"第 {n} 个参数：有 {len(hit)} 个 C 行的 t_ns 相同，请改用序号")
        out.append((lg, hit[0], param))
    return out


def analyze(path, salt, exclude=()):
    logs, compares, broken = load_dir(path)
    s = Session(logs, compares, broken, resolve_excludes(logs, exclude))
    items = [(fn(s, salt) if iid == "A6" else fn(s)) for iid, fn in JUDGES]
    obs = observations(s)
    f1 = freeze_f1(s, items, obs)
    roles = [lg.header["role"] for lg in logs]
    return {
        "inputs": {
            "logs": len(logs) + len(broken), "in": roles.count("I"), "out": roles.count("O"),
            "broken": len(broken), "compares": len(compares),
        },
        "items": items,
        "f1": f1,
        "stop": items[0]["verdict"] == "FAIL",
        "extras": extras(s),
        "warnings": warnings_of(s),
        "observations": obs,
        "excluded": s.excluded,
    }


def render_md(res):
    i = res["inputs"]
    out = ["# 探针分析结果", "", f"输入：实例日志 {i['logs']} 个（In {i['in']}、Out {i['out']}、不完整 {i['broken']}），导出比对 {i['compares']} 行", ""]
    if res["excluded"]:
        out += [f"**本次按 `--exclude` 排除了 {len(res['excluded'])} 段，结论不含这些段，见文末「已排除的段」。**", ""]
    if res["stop"]:
        out += ["**A1 不通过：停工并上报**，评估时间线寻址加固定 D 的混合方案，此前不冻结。", ""]
    out += ["| 项 | 内容 | 结论 | 依据 |", "|---|---|---|---|"]
    for it in res["items"]:
        cover = "、".join(it["cover"]) or "无"
        why = "；".join(it["notes"]).replace("|", "/") or "—"
        out.append(f"| {it['id']} | {it['name']}（{cover}） | {VERDICT_ZH[it['verdict']]} | {why} |")
    f1 = res["f1"]
    out += ["", "## 冻结条件", ""]
    out.append("- F1 数据齐全、A1-A8 有结论：" + ("满足" if f1["ok"] else "未满足（" + "；".join(f1["missing"]) + "）"))
    out += ["- F2 无需改布局：人工确认", "- F3 Win/mac 测试与压测全绿：以 gates 与 dispatch 结果为准", "- F4 用户已过目：人工确认"]
    out += render_observations(res["observations"])
    if res["excluded"]:
        out += ["", "## 已排除的段", "", "这些段整段不进任何判据（`--exclude` 的参数照录）：", ""]
        for x in res["excluded"]:
            out.append(f"- `{x['param']}`：实例 {x['instance']}（{x['role']}）的第 {x['c_index']} 个 C 行，{x['cell']}")
    if res["warnings"]:
        out += ["", "## 提示", ""] + [f"- {x}" for x in res["warnings"]]
    if res["extras"]:
        out += ["", "## 附加记录", ""] + [f"- {x}" for x in res["extras"]]
    return "\n".join(out) + "\n"


def _scrub_result(res):
    """JSON 只清洗自由文本的值；键、编号和 PASS / FAIL 之类的枚举不动，免得与用户名撞词后机读输出损坏。"""
    res = json.loads(json.dumps(res))
    for it in res["items"]:
        it["notes"] = [scrub(x) for x in it["notes"]]
        it["cover"] = [scrub(x) for x in it["cover"]]
    res["f1"]["missing"] = [scrub(x) for x in res["f1"]["missing"]]
    res["extras"] = [scrub(x) for x in res["extras"]]
    res["warnings"] = [scrub(x) for x in res["warnings"]]
    ob = res["observations"]
    for c in ob["cells"]:
        c["cell"], c["note"] = scrub(c["cell"]), scrub(c["note"])
    for k in ("bypass", "smart", "misplaced"):
        for m in ob[k]:
            m["cell"] = scrub(m["cell"])
    for x in res["excluded"]:
        x["param"], x["cell"] = scrub(x["param"]), scrub(x["cell"])
    return res


def run(argv, out, err):
    """入口。任何未预期的异常只报一句固定的话：traceback 里有脚本路径和用户名，不能进输出。"""
    try:
        return _run(argv, out, err)
    except Exception:  # noqa: BLE001
        err.write("内部错误：分析器遇到未预期的情况，细节已隐去。请换个目录重试，或把输入交给维护者排查。\n")
        return 4


def _run(argv, out, err):
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
        exclude_args = take("--exclude", many=True)
    except SystemExit:
        err.write("用法错误：选项缺少取值\n")
        return 2
    try:
        exclude = parse_excludes(exclude_args)
    except ValueError:
        err.write("用法错误：--exclude 应写成 <文件名>:<C 行序号或 t_ns>（序号与 t_ns 重名时写 #N 或 t=N）\n")
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
        err.write("用法：analyze-probe.py <日志目录> [--json] [--exclude <文件名>:<C 行序号或 t_ns>]... | --self-test | --mutation-check | --spec\n")
        return 2
    salt = (salt_arg or secrets.token_hex(16)).encode()
    try:
        res = analyze(args[0], salt, exclude)
    except FormatError as e:
        err.write(scrub(f"日志格式错误：{e}") + "\n")
        return 2
    except ExcludeError as e:
        err.write(scrub(f"--exclude 参数错误：{e}") + "\n")
        return EXIT_EXCLUDE
    except OSError:
        err.write("读取日志目录失败\n")
        return 2
    if as_json:
        out.write(json.dumps(_scrub_result(res), ensure_ascii=False, indent=2) + "\n")
    else:
        out.write(scrub(render_md(res)))
    verdicts = {it["verdict"] for it in res["items"]}
    if "FAIL" in verdicts:
        return 1
    # 退出码 0 才表示可以冻结：F1 因缺数据没满足也只能给 3
    return 0 if not verdicts - {"PASS"} and res["f1"]["ok"] else 3


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
    yield "case 与 variant 的组合不在表内", edit(c, 3, "deactivate")
    yield "buf 超上界", edit(c, 4, "99999999")
    yield "off 超上界", edit(b, 15, "9999999999999999")
    yield "w 超上界", edit(b, 14, "9999999999999999")
    yield "pid 超上界", edit(1, 6, "99999999999")
    yield "n 超上界", edit(b, 4, "99999999")


def self_test():
    tmp = tempfile.mkdtemp(prefix="rinne-probe-selftest-")
    try:
        return _self_test(tmp)
    finally:
        # rinne-delete-ok: tmp 是本函数 mkdtemp 新建的目录，变量没有被改过
        shutil.rmtree(tmp, ignore_errors=True)


def _self_test(tmp):
    ps = _synth()
    lines, bad, good = [], 0, 0

    def check(label, ok, is_bad):
        nonlocal bad, good
        bad += is_bad
        good += not is_bad
        if not ok:
            lines.append(f"失败：{label}")
        return ok

    salt = b"selftest"
    # 1. 夹具矩阵：每个判据的通过 / 不通过 / 数据不全
    tree, expect = ps.fixtures()
    for d, files in tree.items():
        base = os.path.join(tmp, d)
        _write_tree(base, files)
        res = analyze(base, salt)
        got = {it["id"]: it["verdict"] for it in res["items"]}
        got["OBS"] = res["observations"]["status"]
        want = expect[d] if d in expect else {i: "PASS" for i in ps.ITEMS}
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
    # 2b. 不完整的文件降级而不是中止；超长轨道名截断而不是判错；比对表自洽
    for label, raw in (("空文件", b""), ("只有魔数", (MAGIC + "\n").encode()), ("H 行写了一半", (MAGIC + "\nH,I,vst").encode())):
        try:
            parse_log(raw, "日志#1")
            check(f"不完整应降级：{label}", False, True)
        except Incomplete:
            check(f"不完整应降级：{label}", True, True)
        except FormatError:
            check(f"不完整应降级而不是格式错：{label}", False, True)
    long_name = "%E4%B8%AD" * 300
    lg_long = parse_log(valid.replace("\nZ,", "\nE,5,track_props," + long_name + ",\nZ,").encode("ascii"), "日志#1")
    tp = [e for e in lg_long.events if e["name"] == "track_props"][0]["a"]
    check("超长轨道名被截断到 200 字节", len(unquote_to_bytes(tp)) <= LIMITS["name_bytes"], False)
    head = COMPARE_HEAD + "\n"
    for label, row in (("逐位相同却有残差", "CB-04,export-offline,1,-50.0,-50.0,0\n"), ("残差 -inf 却不逐位", "CB-04,export-offline,0,-inf,-inf,0\n")):
        try:
            parse_compare((head + row).encode(), "比对#1")
            check(f"比对表应拒绝：{label}", False, True)
        except FormatError:
            check(f"比对表应拒绝：{label}", True, True)
    # 2c. 红旗回归：混 buf、非 UTF-8 控制台、内部错误都不能漏出 traceback 或路径
    mixed = os.path.join(tmp, "mixed-buf")
    ma = ps.pair("CB-01", "steady", 256, "low", pid=4801)
    mb = ps.pair("CB-01", "steady", "-", "low", pid=4802)
    _write_tree(mixed, ps.files_of(*ma, *mb))
    code, so, se = _cli([mixed])
    check("混用 buf 取值不崩溃", code in (0, 1, 3) and "内部错误" not in se, True)
    import subprocess

    env = {**os.environ, "PYTHONIOENCODING": "cp1252"}
    r = subprocess.run([sys.executable, "-B", os.path.abspath(__file__), os.path.join(tmp, "full-pass")], capture_output=True, env=env)
    err_text = r.stderr.decode("utf-8", "replace")
    check("cp1252 控制台也能正常输出", r.returncode == 0 and b"Traceback" not in r.stderr, True)
    check("stderr 不含路径特征", "Users" not in err_text and ".py" not in err_text and "line " not in err_text, True)
    saved_analyze = globals()["analyze"]

    def boom(*a, **k):
        raise RuntimeError(os.path.abspath(__file__))

    globals()["analyze"] = boom
    try:
        code, so, se = _cli([os.path.join(tmp, "full-pass")])
    finally:
        globals()["analyze"] = saved_analyze
    check("内部错误退出码 4", code == 4, True)
    check("内部错误只报固定一句", se.strip().startswith("内部错误") and "Traceback" not in se and ".py" not in se and "line " not in se, True)
    # 3b. 用户名与枚举撞词（USERNAME=pass）时，JSON 的枚举不能被清洗坏
    saved = os.environ.get("USERNAME")
    os.environ["USERNAME"] = "pass"
    try:
        _, so, _ = _cli([os.path.join(tmp, "full-pass"), "--json"])
        ok = False
        try:
            ok = [it["verdict"] for it in json.loads(so)["items"]] == ["PASS"] * 9
        except ValueError:
            pass
        check("用户名撞上枚举时 JSON 仍可解析且枚举完好", ok, True)
    finally:
        if saved is None:
            os.environ.pop("USERNAME", None)
        else:
            os.environ["USERNAME"] = saved
    # 4. 命令行
    check("目录不存在退出码 2", _cli([os.path.join(tmp, "nope")])[0] == 2, True)
    check("全部通过退出码 0", _cli([os.path.join(tmp, "full-pass")])[0] == 0, False)
    check("有不通过退出码 1", _cli([os.path.join(tmp, "a1-fail")])[0] == 1, True)
    check("只有数据不全退出码 3", _cli([os.path.join(tmp, "a1-incomplete")])[0] == 3, True)
    # 4b. 格观察的数字：正常值与异常值各一组
    ob = analyze(os.path.join(tmp, "obs-normal"), salt)["observations"]
    check("正常：同通道顺序全是 In 先", ob["order"] == {"pairs": 40, "in_first": 40}, False)
    check("正常：旁路块与 w 连续", ob["bypass"][0]["bypassed_blocks"] == {"I": 20, "O": 20} and (ob["bypass"][0]["o_w_stalls"], ob["bypass"][0]["o_w_regress"], ob["bypass"][0]["o_w_jumps"]) == (0, 0, 0), False)
    check("正常：Smart 间隙小、ping 两条", ob["smart"][0]["max_gap_ms"] < 10 and ob["smart"][0]["pings"] == 2, False)
    check("正常：错放两端都在时给先后与 off", ob["misplaced"][0]["verdict"] == "两端都在" and ob["misplaced"][0]["in_first"] == 0 and ob["misplaced"][0]["pairs"] == 40 and ob["misplaced"][0]["off_in"] == [480], False)
    ob = analyze(os.path.join(tmp, "obs-abnormal"), salt)["observations"]
    check("异常：Out 先于 In 的回调被数出来", ob["order"] == {"pairs": 40, "in_first": 30}, True)
    check("异常：w 停滞 3 次、回退 1 次、跳变 2 次", (ob["bypass"][0]["o_w_stalls"], ob["bypass"][0]["o_w_regress"], ob["bypass"][0]["o_w_jumps"]) == (3, 1, 2), True)
    check("异常：Smart 最长间隙大于 1 s 且 ping 不足", ob["smart"][0]["max_gap_ms"] >= 1200 and ob["status"] == "INCOMPLETE", True)
    check("异常：错放只有一端时写无法判断", ob["misplaced"][0]["verdict"] == "无法判断", True)
    # 5. F1：缺矩阵格、块早于第一个 C 行都要被指出
    res = analyze(os.path.join(tmp, "f1-missing-cell"), salt)
    check("缺一格 CB-01 时 F1 指出矩阵缺格", (not res["f1"]["ok"]) and any("矩阵缺格" in m for m in res["f1"]["missing"]), True)
    res = analyze(os.path.join(tmp, "f1-cb05-no-blocks"), salt)
    check("CB-05 只有 C 行时 F1 指出无数据", (not res["f1"]["ok"]) and "CB-05 无数据" in res["f1"]["missing"], True)
    check("A1-A9 过了但 F1 没满足时退出码是 3", _cli([os.path.join(tmp, "f1-missing-cell")])[0] == 3, True)
    res = analyze(os.path.join(tmp, "f1-cb05-bypass-dup"), salt)
    check("CB-05 同键重采两对时 F1 不满足且指出重复", (not res["f1"]["ok"]) and any("同键重复" in m for m in res["f1"]["missing"]), True)
    res = analyze(os.path.join(tmp, "f1-cb04-no-rip"), salt)
    check("CB-04 缺 rip 时 F1 点名", (not res["f1"]["ok"]) and any("CB-04 缺 rip" in m for m in res["f1"]["missing"]), True)
    check("同键重复挡 F1 时退出码是 3", _cli([os.path.join(tmp, "f1-cb05-bypass-dup")])[0] == 3, True)
    res = analyze(os.path.join(tmp, "orphan-blocks"), salt)
    check("块早于第一个 C 行时给出提示", any("第一个 C 行" in w for w in res["warnings"]) and not res["f1"]["ok"], True)
    _self_test_gaps(tmp, check)
    return {"ok": not lines, "bad": bad, "good": good, "lines": lines}


def _self_test_gaps(tmp, check):
    """补口：A2 按处理线程推断切换点、CB-05 认 reset、只数段内事件、短命克隆的 A8、--exclude。夹具的判定已在夹具矩阵里查过，这里查说明文字与命令行。"""

    def run_json(d, *extra):
        code, so, se = _cli([os.path.join(tmp, d), "--json", "--hash-salt", "s", *extra])
        empty = {"items": [], "excluded": [], "warnings": [], "extras": [], "observations": {"cells": [], "smart": []}}
        return code, (json.loads(so) if so.strip() else empty), se  # 参数错时没有 JSON：给个空壳，让断言失败而不是自测崩溃

    def said(res, iid):
        return " ".join(next((it["notes"] for it in res["items"] if it["id"] == iid), []))

    def verdict(res, iid):
        return next((it["verdict"] for it in res["items"] if it["id"] == iid), None)

    # A2：切换点 = 显式事件 ∪ 处理线程变化点；块长抖动不算
    said_a2 = lambda d: said(run_json(d)[1], "A2")  # noqa: E731
    check("A2 推断：结论文字写明按处理线程切换推断 10 次", "按处理线程切换推断 10" in said_a2("a2-pass-thread-inferred"), False)
    check("A2 推断：显式事件与线程变化同时出现时只数一遍", "显式事件 10、按处理线程切换推断 0" in said_a2("a2-pass-explicit-and-thread"), False)
    txt = said_a2("a2-incomplete-thread-constant")
    check("A2 推断：线程不变（只有块长抖动）时写明原因", "处理线程没有变化" in txt and "ASIO-Guard 关着" in txt, True)
    txt = said_a2("a2-incomplete-one-cell-no-switch")
    check("A2 推断：某一格一次切换都没有时单独说明，不被另一格的切换数盖住", "buf=64" in txt and "处理线程没有变化" in txt, True)
    txt = said_a2("a2-incomplete-thread-few")
    check("A2 推断：次数不足时分开写显式与推断的次数", "按处理线程切换推断 3" in txt and f"不足 {MIN_TOGGLES}" in txt, True)
    txt = said_a2("a2-incomplete-thread-flap")
    check("A2 推断：线程频繁抖动、窗口太短时数据不全，不因次数多而通过", "窗口里块数不足" in txt, True)
    check("A2 推断：不通过的说明也写明推断", "按处理线程切换推断" in said_a2("a2-fail-thread-inferred"), True)

    # CB-05：set_processing(a=0)、reset、release 各自成立，写明是哪一种；段之前的事件不算
    for d, word in (("cb05-deactivate-reset-only", "reset 1"), ("cb05-deactivate-release-only", "release 1"),
                    ("cb05-disable-track-reset-only", "reset 1"), ("cb05-deactivate-no-event", "")):  # fmt: skip
        cells = [c for c in run_json(d)[1]["observations"]["cells"] if c["variant"] in ("deactivate", "disable-track")]
        ok = bool(cells) and (cells[0]["state"] == "PASS" and word in cells[0]["note"] if word else cells[0]["state"] == "INCOMPLETE")
        check(f"CB-05 停用证据：{d}", ok, not word)
    for d in ("cb05-deactivate-pre-release", "cb05-deactivate-set-processing-on"):
        cells = [c for c in run_json(d)[1]["observations"]["cells"] if c["variant"] == "deactivate"]
        check(f"CB-05 停用证据：{d} 不算", bool(cells) and cells[0]["state"] == "INCOMPLETE" and "本段内" in cells[0]["note"], True)
    ob = run_json("cb06-smart-pre-ping")[1]["observations"]
    check("smart：段前的 ping 不算，格观察与本格证据都只数 1 条", ob["smart"][0]["pings"] == 1 and "本段内只有 1 条 ping" in ob["cells"][0]["note"], True)

    # A8：没有 C 行又活不到 2 s 的实例只记说明
    _, r, _ = run_json("a8-pass-short-clones")
    check("A8：短命克隆只记说明，写明实例与时长", "实例 2（I，300 ms）" in said(r, "A8") and "实例 4（O，120 ms）" in said(r, "A8"), False)
    check("extras：照常数出短命克隆", any("实例 2（I，300 ms）" in x and "2 个" in x for x in r["extras"]), False)
    check("A8：克隆没收尾仍是不通过", verdict(run_json("a8-fail-short-clone-crash")[1], "A8") == "FAIL", True)

    # --exclude
    mid, pids = "a1-incomplete-misrecorded-mid", {"mid": 5020, "unbound": 5021, "trailing": 5022, "gap": 5023, "tail": 5025}

    def both(pid, sel):
        return ["--exclude", f"probe-I-{pid}-e1f2.csv:{sel}", "--exclude", f"probe-O-{pid}-e3f4.csv:{sel}"]

    for tag, d, sel in (("mid", mid, 2), ("unbound", "a1-incomplete-misrecorded-unbound", 2), ("trailing", "a1-incomplete-misrecorded-trailing", 3), ("tail", "a1-incomplete-misrecorded-tail", 2),
                        ("mid", mid, "#2"), ("mid", mid, "t=1999999900"), ("mid", mid, 1999999900)):  # fmt: skip
        _, r, _ = run_json(d, *both(pids[tag], sel))
        ok = verdict(r, "A1") == "PASS" and [x["cell"] for x in r["excluded"]] == ["CB-01/steady/buf=-/guard=na"] * 2
        check(f"--exclude：{d} 排除 {sel} 后 A1 回到通过", ok, True)
    _, r, _ = run_json("a1-incomplete-misrecorded-trailing", *both(pids["trailing"], 3))
    check("--exclude：丢弃记在被排除的段上时，提示里说明", any("记在被排除的段上" in w for w in r["warnings"]), False)
    _, r, _ = run_json(mid, "--exclude", f"probe-I-{pids['mid']}-e1f2.csv:2")
    check("--exclude：只排除 In 一份时，Out 一份仍留着，这一格还是数据不全", verdict(r, "A1") == "INCOMPLETE" and "只有 Out 一端" in said(r, "A1"), True)
    _, r, _ = run_json("a1-incomplete-gap-beside-excluded", *both(pids["gap"], 2))
    check("--exclude：邻段自己的缺口仍按段内连续性算，排除别的段盖不住它", verdict(r, "A1") == "INCOMPLETE" and "序号缺口" in said(r, "A1"), True)
    _, r, _ = run_json(mid)
    check("--exclude：不给参数时 excluded 为空", r["excluded"] == [], False)
    args_md = [os.path.join(tmp, mid), "--hash-salt", "s", *both(pids["mid"], 2)]
    code, so, _ = _cli(args_md)
    check("--exclude：文本输出列出被排除的段与参数", "## 已排除的段" in so and f"probe-I-{pids['mid']}-e1f2.csv:2" in so and "第 2 个 C 行" in so, False)
    for fmt in ([], ["--json"]):
        _, so, se = _cli([*args_md, "--redact", "e1f2", *fmt])
        check(f"--redact：被排除段的文件名照常脱敏（{'json' if fmt else 'md'}）", "e1f2" not in so + se and REDACTED in so, True)
    for label, extra, want in (
        ("文件不存在", ["--exclude", "probe-I-1-zzzz.csv:2"], EXIT_EXCLUDE),
        ("C 行序号超出", ["--exclude", f"probe-I-{pids['mid']}-e1f2.csv:9"], EXIT_EXCLUDE),
        ("t_ns 对不上", ["--exclude", f"probe-I-{pids['mid']}-e1f2.csv:t=12345"], EXIT_EXCLUDE),
        ("缺冒号", ["--exclude", "nocolon"], 2),
        ("选择子不是数字", ["--exclude", f"probe-I-{pids['mid']}-e1f2.csv:abc"], 2),
        ("选项缺取值", ["--exclude"], 2),
    ):  # fmt: skip
        code, so, se = _cli([os.path.join(tmp, mid), *extra])
        check(f"--exclude 参数错：{label}，退出码 {want}", code == want and not so and "zzzz" not in se, True)
    check("--exclude 的参数错有自己的退出码，不与 0 1 2 3 4 混用", EXIT_EXCLUDE not in (0, 1, 2, 3, 4), True)
    def resolved(rows, *params):
        """用只有 name 与 cases 的替身解析选择子：→ C 行下标的列表；报错时返回 None。"""
        stub = type("Stub", (), {})()
        stub.name, stub.cases = "x.csv", [{"t_ns": t} for t in rows]
        try:
            return [h[1] for h in resolve_excludes([stub], parse_excludes(list(params)))]
        except ExcludeError:
            return None

    check("--exclude：序号与 t_ns 撞车时报错，不替用户猜", resolved([2, 5], "x.csv:2") is None, True)
    check("--exclude：#N 按序号、t=N 按 t_ns、裸数字先按 t_ns", resolved([2, 5], "x.csv:#2", "x.csv:t=2", "x.csv:5") == [1, 0, 1], False)
    check("--exclude：两个 C 行的 t_ns 相同时，按 t_ns 选报错、按序号选可以", resolved([7, 7], "x.csv:t=7") is None and resolved([7, 7], "x.csv:#2") == [1], True)
    check("--exclude：带目录的文件名只取最后一级", [p[0] for p in parse_excludes(["a/b/x.csv:3", "a\\b\\y.csv:4"])] == ["x.csv", "y.csv"], False)


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
    for const, loose in (("RESIDUAL_LIMIT_DB", 0.0), ("MIN_TOGGLES", 0), ("HOSTNS_MISMATCH_RATIO", 1.0), ("MIN_SLICE_BLOCKS", 0), ("MIN_STEADY_BLOCKS", 0), ("SHORT_LIVED_NS", 0)):
        orig = globals()[const]
        globals()[const] = loose
        try:
            if self_test()["ok"]:
                lines.append(f"放宽阈值 {const} 后自测仍是绿的")
        finally:
            globals()[const] = orig
    return {"ok": not lines, "lines": lines}


if __name__ == "__main__":
    for _stream in (sys.stdout, sys.stderr):  # 非 CJK 区域设置的控制台编码装不下中文时不能崩
        try:
            _stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    sys.exit(run(sys.argv[1:], sys.stdout, sys.stderr))
