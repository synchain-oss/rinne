# SPDX-License-Identifier: GPL-3.0-or-later
"""探针日志分析：读一个目录里的 CSV，按 A1-A9 出结论表，并给出冻结条件 F1 的判定。

    python analyze-probe.py <日志目录> [--json] [--hash-salt <文本>] [--redact <词>]... [--exclude <文件名>:<C 行序号或 t_ns>]...
                            [--toggles <case>:<现场按键次数>]... [--crashed <文件名>]...
    python analyze-probe.py --self-test [--json]     # 夹具、严格格式、脱敏的自测
    python analyze-probe.py --mutation-check         # 逐条屏蔽判据与脱敏，每条都必须让自测变红
    python analyze-probe.py --spec                   # 以 JSON 打印日志格式的机读规格

退出码：0 = A1-A9 全部通过且 F1 满足；1 = 有不通过；3 = 无不通过但有数据不全或 F1 未满足；2 = 输入不合格式或用法错误；4 = 内部错误；
5 = `--exclude` 或 `--crashed` 指向的文件、段不存在或不唯一，或 `--crashed` 标注的实例其实有收尾行（参数指向的东西对不上，与 2 的「写法或日志格式不对」分开）。
`--json` 的 `stop` 字段为真表示 A1 不通过，要停工上报。输出固定为 UTF-8。

`--exclude <文件名>:<选择子>` 把现场误记的一段（一个 C 行起、到下一个 C 行或文件尾止）整段拿出判据，可重复。文件名是日志目录里的 CSV 文件名；
选择子是该文件里 C 行的 1 起算序号（按文件里出现的顺序），或 C 行的 `t_ns`；两者都可能指同一个数时写 `#3`（序号）或 `t=3`（t_ns）。
In 与 Out 各有一份文件，一格误记要在两个文件里各排除一次；同一个 C 行指两次也算参数错。输出里不写文件名，只写「实例 N 的第 k 个 C 行」。
被排除的段不进任何判据，但**排除只能拿掉数据不全的段，拿不掉不通过的段**：被排除的段单独判一遍，本来会不通过的项照样记不通过。
缺失的块序号按探针摘要里的累计块数归到所属的段（见 `Session._segment`）：归给被排除的段的才被吞掉，归不清或归给未排除的段的仍让它数据不全。
早于第一个 C 行的孤块之间（及第一个块是孤块时它之前）缺的块序号属实例级，不归任何格，只在提示里报个数，也不当成尾部丢块记给最后一格。
B 行的 seq / call_seq 按文件顺序严格递增（不递增判格式错，退出码 2），所以缺口、开头与尾部的丢块都按文件顺序算。

`--toggles <case>:<N>` 告诉分析器现场实际按了多少次监听 / 录音使能键（case 取 CB-02 或 LU-03，可重复）。探针看不到这类按键，
切换点只能从处理形态的变化推断（线程池预取与专用线程实时是两种形态，线程池内部的轮换不算切换，见 `regimes_of`）；推断出来的切换点要和
这个次数对得上（差不超过 A2_TOGGLE_TOLERANCE）、而且这一格里形态不超过 A2_MAX_REGIMES 种，A2 才能通过，否则判数据不全：
宿主线程池在稳态下换线程，和用户的按键分不开。次数的对账是整个 case 的性质，主会话算一次（`toggle_reviews`），`--exclude` 的影子判定沿用，不拿只含被排除的段的计数去对；
处理形态数却是逐段的性质，影子沿用之前先核被排除的段自己的形态数（`shadow_reviews`），多于 A2_MAX_REGIMES 种就不沿用。

`--crashed <文件名>`（可重复）由人工标注「这个实例的日志因整机崩溃中断」：没有收尾行不再计为插件崩溃，A8 写一条说明，报告开头醒目提示
请人工确认是否确属整机崩溃；摘要间隔、跳闸照常判。标注了的实例却有收尾行，或文件不存在，按参数错处理（退出码 5）。输出里不写文件名。
末行写到一半（没有换行结尾）的文件照常解析，残行丢掉，A8 的说明写明丢了几字节；文件头都没写完的文件没有可分析的内容，不认（退出码 5）。

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
from bisect import bisect_left, bisect_right
from collections import Counter
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
A1_STALL_MAX_NS = 120_000_000  # Out 端每段播放开头允许的「时间线停顿」上限。现场（Cubase 15）最长一次 56 ms（64 样本缓冲、42 块），取约 2 倍
A2_TOGGLE_TOLERANCE = 1  # --toggles 给的现场次数与分析器数出的次数最多差这么多
A2_MAX_REGIMES = 2  # 靠现场次数确认推断出的切换时，这一格里最多出现几种处理形态（线程池预取与专用线程实时）
A2_REGIME_MIN_NS = 250_000_000  # 一种处理形态要持续这么久才算数（迟滞）。现场：线程池里同一线程连着出现的最长一段 21 ms，实时线程一次占 3.0 s 以上，取两者之间
A2_POOL_MIN_THREADS = 3  # 一段里不同的线程不少于这么多个，算「线程池」形态
A7_SHIFT_MAX = 16  # 两端 call_seq 的对应差（Out − In）绝对值上限。call_seq 是同进程的回调计数，一对 In / Out 前后脚被调用，现场每一格都是 +1；同进程多对实例交错时差会变大，取 16 留余量，超过说明两端不在同一个计数器上
A7_SHIFT_SHARE_MIN = 0.9  # 最近邻的 (In, Out) 里，call_seq 差等于众数的占比下限。现场每一格都是 100%
A7_TOGGLE_ZONE_NS = 500_000_000  # toggle 格里 hostNs 回退落在切换点前后各这么久之内的，容忍并计数。现场的回退都在切换点前 22 ms 到后 9 ms 之内，记录里的上界是 0.5 s
REPLACE_WINDOW_NS = 15_000_000_000  # Disable Track 销毁实例后，新实例要在多久之内 set_state 并配对。清单让用户停用 5 s，现场 7.6 s 后新实例 set_state、7.9 s 后配对，取约 2 倍
DEACT_GAP_NS = 1_000_000_000  # 停用证据要落在至少这么长的块流中断里（清单让用户把插件停用 5 s）
DEACT_LEAD_NS = 250_000_000  # 停用事件可以比中断前最后一块早这么多（事件与块是不同线程取的时间戳）
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
_DEACT = "至少一端 32 块，且本段内的 set_processing（a 为 0）、reset 或 release 事件落在块流中断里（不短于 1 s，前后都有块）"
_REPLACED = (f"；Disable Track 会销毁实例，所以也可以是本段内旧实例 destruct、{REPLACE_WINDOW_NS // 1_000_000_000} s 内同 pid 同角色的新实例 set_state 并配对"
             "（实例被替换）")  # fmt: skip
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
_r("CB-05", "disable-track", 2, "格观察", _DEACT + _REPLACED)
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
        "4": "内部错误", "5": "--exclude 或 --crashed 指向的文件、段不存在或不唯一，或标注与日志对不上",
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
        self.truncated = False  # 最后一行没有换行结尾（写到一半被截断）：这一行已丢掉
        self.partial_bytes = 0  # 丢掉的残缺末行的字节数
        self.pre_events = []  # 第一个 C 行之前的事件：实例级
        self.orphans = 0  # 早于第一个 C 行、没法归入任何格的块
        self.orphan_lost = 0  # 早于第一个 C 行的缺失块序号个数（孤块之间、孤块之前）：属实例级，不归任何格
        self.name = None  # 日志目录里的文件名：只用于匹配 --exclude 与 --crashed，不进任何输出
        self.crashed = False  # --crashed 标注：这个实例的日志因整机崩溃中断，没有收尾不算插件崩溃

    def _stamps(self):
        ts = [r["t_ns"] for r in self.cases + self.events + self.summaries]
        ts += [b["mono_ns"] for b in self.blocks]
        if self.closing is not None:
            ts.append(self.closing["t_ns"])
        return ts

    def span_ns(self):
        """文件里所有带时间戳的行（C、E、S、Z 的 t_ns，B 的 mono_ns）的首尾跨度；没有带时间戳的行是 0。"""
        ts = self._stamps()
        return max(ts) - min(ts) if ts else 0

    def first_ns(self):
        """文件里最早的时间戳（实例的出生时刻）；没有带时间戳的行是 0。"""
        ts = self._stamps()
        return min(ts) if ts else 0

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
    else:  # 最后一行没有换行：写到一半被截断，丢掉它并记下字节数（文件是 ASCII，字符数就是字节数）
        lg.partial_bytes = len(lines.pop())
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
        self.excluded = False  # --exclude 点名的误记段：留在 _segment 里算缺口，算完即拿出，不进判据（但会单独判一遍，见 Session.shadow）

    def has(self, name):
        return any(e["name"] == name for e in self.events)


def key_label(key):
    return f"{key[0]}/{key[1]}/buf={key[2]}/guard={key[3]}"


def dropped_of(lg):
    """探针报告的这个实例的累计丢弃数（各条摘要与收尾行里最大的一个）。"""
    return max([x["dropped"] for x in lg.summaries] + [lg.closing["dropped_total"] if lg.closing else 0])


class Session:
    def __init__(self, logs, compares, broken=(), exclude=()):
        """exclude = [(Log, 该 Log.cases 里的下标, 选择子原文)]，由 resolve_excludes 解析而来。"""
        self.logs = logs
        self.compares = compares  # 比对行
        self.broken = list(broken)  # 不完整的日志文件（只有标签，没有内容）
        self.segments = []  # 进判据的段
        self.hidden = []  # 被 --exclude 拿出去的段：不进判据，只在 shadow() 里单独判一遍
        self.cells = {}
        self.orphans = 0
        self.orphan_lost = self.orphan_gaps = 0  # 早于第一个 C 行的缺失块序号个数与缺口处数（孤块之间、孤块之前）：属实例级，不归任何格
        self.excluded = []  # 被排除的段：{selector, instance, role, cell, c_index}
        self.attribution = {}  # 实例序号 → (归给被排除段的缺失块序号数, 归给未排除段的数, 是否按摘要归属)：有被排除段的实例才有
        for lg in logs:
            self._segment(lg, [(k, sel) for g, k, sel in exclude if g is lg])
        for seg in self.segments:
            self.cells.setdefault(seg.key, {"I": [], "O": []})[seg.role].append(seg)
        self.dups = sorted(
            (k for k, c in self.cells.items() if k[1] not in MULTI_OK and (len(c["I"]) > 1 or len(c["O"]) > 1)),
            key=lambda k: (k[0], k[1], str(k[2]), k[3]),
        )

    def shadow(self):
        """被排除的段单独成一个会话，用来问「它们若不被排除，会不会不通过」：同一个键的 In / Out 按出现顺序两两配成一格。
        --exclude 只许拿掉数据不全的段，不许拿掉本来不通过的段。"""
        sh = Session.__new__(Session)
        sh.logs, sh.compares, sh.broken = self.logs, self.compares, self.broken
        sh.segments, sh.hidden, sh.excluded, sh.orphans, sh.attribution, sh.dups = list(self.hidden), [], [], 0, {}, []
        sh.orphan_lost = sh.orphan_gaps = 0
        sh.cells, nth = {}, {}
        for seg in sh.segments:
            n = nth[(seg.key, seg.role)] = nth.get((seg.key, seg.role), -1) + 1
            sh.cells.setdefault(seg.key + (n,), {"I": [], "O": []})[seg.role].append(seg)
        return sh

    def _segment(self, lg, exclude=()):
        lg.pre_events, lg.orphans = [], 0  # 同一份日志会分段不止一次，先清零
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
        # lg.blocks 在文件里就是按 seq（与 call_seq）严格递增排的：parse_log 对不递增的 B 行判格式错（退出码 2），
        # 所以文件顺序就是 seq 顺序，下面的缺口、实例开头与尾部的丢块都直接按文件顺序算
        lost_total, cross = 0, []  # 缺失的块序号总数（所有缺口都计，包括孤块之间的）；隔着段的缺失区间 [lo, hi)（只有带被排除段的实例才分开记）
        orphan_lost = orphan_gaps = 0  # 早于第一个 C 行的缺失块序号（孤块之间、孤块之前）：属实例级，不归任何格，只报个数
        head = lg.blocks[0]["seq"] if lg.blocks else 0  # 实例开头缺的块序号个数
        if head and at(lg.blocks[0]["mono_ns"]) < 0:  # 第一个块就是孤块：它之前缺的块更早，同样早于第一个 C 行
            orphan_lost, orphan_gaps, lost_total, head = head, 1, head, 0
        prev = prev_i = None
        for b in lg.blocks:
            i = at(b["mono_ns"])
            if i >= 0:
                segs[i].blocks.append(b)
            else:
                lg.orphans += 1
                self.orphans += 1
            if prev is not None and b["seq"] != prev + 1:
                gap = b["seq"] - prev - 1
                lost_total += gap
                if i < 0 and prev_i < 0:  # 缺口两头都是孤块：早于第一个 C 行，不归任何格
                    orphan_lost += gap
                    orphan_gaps += 1
                else:
                    at_i = i if i >= 0 else prev_i  # 后一块在某格里就记给它；它是孤块（时间戳倒着走的文件）时记给前一块所在的格
                    if not exclude or prev_i == i:
                        segs[at_i].gaps += 1  # 没有被排除的段，或缺口两头在同一段里：记给后一块所在的段
                    else:
                        cross.append((prev + 1, b["seq"]))
            prev, prev_i = b["seq"], i
        lg.orphan_lost = orphan_lost
        self.orphan_lost += orphan_lost
        self.orphan_gaps += orphan_gaps
        dropped = dropped_of(lg)
        if not exclude:
            if dropped > orphan_lost and not any(s.gaps for s in segs):
                for s in segs:  # 探针报告丢过记录、却定位不到缺口（早于第一个 C 行的丢块已定位，不算）：整个实例都按缺口算
                    s.gaps += 1
        else:
            self._attribute(lg, segs, times, cross, lost_total, dropped, head)
        for k, sel in exclude:
            seg = segs[where[k]]
            self.excluded.append({
                "selector": sel, "instance": self.logs.index(lg) + 1, "role": lg.header["role"],
                "cell": key_label(seg.key), "c_index": k + 1,
            })  # fmt: skip
        self.hidden += [s for s in segs if s.excluded]
        self.segments += [s for s in segs if not s.excluded]

    def _attribute(self, lg, segs, times, cross, lost_total, dropped, head):
        """有被排除段的实例：把缺失的块序号归到所属的段。
        探针每个块（写出的与丢弃的）都占一个 seq，1 Hz 摘要里的「累计块数 + 累计丢弃数」就是那一刻已经占用的 seq 个数。
        取 C 行之前最近一条摘要的这个数当作该格窗口开始前已占用的 seq 数，一格的窗口就是 [本格的数, 下一格的数)：
        落在这个范围里的缺失块序号归给这一格。归给被排除段的不计入任何判据；归给未排除段的让它数据不全。
        缺口两头在同一段里的已经直接记给那一段。前一格最后一秒之内紧接着下一格时，摘要分不开两格，
        这部分归到后一格（后一格未排除时仍让它数据不全，后一格被排除时前一格最后不到 1 s 内的丢块会被吞掉：见 LOG-FORMAT）。
        探针报告的丢弃数比缺口多出来的部分（尾部丢块，也含丢掉的事件）接在最后一个块之后；
        前面没有摘要可用时无从归属，各个未排除的段都记一次。
        lost_total 是文件里所有缺口的总数（含早于第一个 C 行、不归任何格的那些），尾部丢块 = 探针报告的丢弃数 − lost_total，
        所以不归任何格的缺口也必须算进去，否则它们会被当成尾部丢块记给最后一格。
        head = 实例开头缺的块序号个数（第一个块是孤块时已经算作不归任何格，这里是 0）。"""
        if head:
            cross.append((0, head))
            lost_total += head
        tail = max(0, dropped - lost_total)
        if tail:
            start = lg.blocks[-1]["seq"] + 1 if lg.blocks else 0
            cross.append((start, start + tail))
        st = sorted(lg.summaries, key=lambda s: s["t_ns"])
        stt = [s["t_ns"] for s in st]
        starts = [0]
        for k in range(1, len(segs)):
            j = bisect_left(stt, times[k]) - 1  # 最后一条严格早于该 C 行的摘要
            if j < 0:
                starts = None
                break
            starts.append(max(starts[-1], st[j]["blocks"] + st[j]["dropped"]))
        absorbed = charged = 0
        for lo, hi in cross:
            if starts is None:  # 摘要不可用，无从归属：各个未排除的段都记一次（被排除的段也记，只给 shadow() 用，同上）
                if any(not seg.excluded for seg in segs):
                    charged += hi - lo
                else:
                    absorbed += hi - lo
                for seg in segs:
                    seg.gaps += 1
                continue
            for k, seg in enumerate(segs):
                top = starts[k + 1] if k + 1 < len(segs) else hi
                mine = max(0, min(hi, top) - max(lo, starts[k]))
                if not mine:
                    continue
                if seg.excluded:
                    absorbed += mine
                    seg.gaps += 1  # 不影响主会话（被排除的段不进判据），只让 shadow() 的「若计入」也把它当缺口段：数据不全不是不通过
                else:
                    seg.gaps += 1
                    charged += mine
        if absorbed or charged:
            self.attribution[self.logs.index(lg) + 1] = (absorbed, charged, starts is not None)


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


def out_off_profile(blocks_all):
    """Out 端 off 的形态（A1）。Out 的 off = w − 宿主时间线：Cubase 起播后头一阵子时间线停住不走（或只走了一部分），
    w 却照常前进，off 因此在每段播放的开头变几下，停顿一结束就恒定。所以每段播放（playing 由 0 变 1 起算，
    或段内第一个播放块；旁路块与停播块把一段截断）的开头允许一段「时间线停顿」：这一段里每块的 off 只增不减
    （0 <= Δoff <= Δw，即派生时间线 w − off 没有后退、也没走得比 w 快），而且这一段不超过 A1_STALL_MAX_NS；
    停顿之后本段的 off 必须恒定，各段停顿之后的 off 还要彼此相等。
    → {"stall_ns": 各段停顿里最长的一段（首块到 off 定下来的那一块）, "stall_blocks": 那一段的块数,
       "runs": 播放段数, "settled": 停顿之后的 off 取值集合, "late": 停顿过长的段数, "shrink": 停顿里 off 回退或时间线后退的段数}"""
    runs, cur = [], None
    for b in blocks_all:
        if b["playing"] and not b["bypassed"]:
            if cur is None:
                cur = []
                runs.append(cur)
            cur.append(b)
        else:
            cur = None
    out = {"stall_ns": 0, "stall_blocks": 0, "runs": len(runs), "settled": set(), "late": 0, "shrink": 0}
    for run in runs:
        last = max([k for k in range(1, len(run)) if run[k]["off"] != run[k - 1]["off"]], default=0)  # off 最后一次变化的那一块
        out["settled"].add(run[last]["off"])
        if any(not 0 <= run[k]["off"] - run[k - 1]["off"] <= run[k]["w"] - run[k - 1]["w"] for k in range(1, last + 1)):
            out["shrink"] += 1
        ns = run[last]["mono_ns"] - run[0]["mono_ns"]
        if ns > A1_STALL_MAX_NS:
            out["late"] += 1
        if ns > out["stall_ns"]:
            out["stall_ns"], out["stall_blocks"] = ns, last
    return out


def judge_a1(s):
    parts, cover, skipped, stalls = [], [], 0, []
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
            full = sorted(seg.blocks, key=lambda b: b["seq"])
            blocks = [b for b in full if b["playing"] and not b["bypassed"]]
            if len(blocks) < MIN_STEADY_BLOCKS:
                parts.append(("INCOMPLETE", f"{key_label(key)} {seg.role} 播放中的块 {len(blocks)} 不足"))
                continue
            bad_m = sum(1 for b in blocks if b["m"] != 0)
            bad_rs = sum(1 for b in blocks[1:] if b["rs"] != 0)
            if seg.role == "I":  # In 端的判据不放宽：m 全 0、首块后 rs 0、off 恒定
                offs = len({b["off"] for b in blocks})
                extra = ""
            else:  # Out 端：每段播放开头允许一段时间线停顿，停顿之后 off 恒定
                prof = out_off_profile(full)
                offs = len(prof["settled"])
                stalls.append((key, prof))
                extra = "".join([
                    f"、起播停顿 {prof['stall_ns'] // 1_000_000} ms（{prof['stall_blocks']} 块）超过上限 {A1_STALL_MAX_NS // 1_000_000} ms" if prof["late"] else "",
                    "、停顿里 off 回退或时间线后退" if prof["shrink"] else "",
                ])  # fmt: skip
            if bad_m or bad_rs or offs > 1 or extra:
                what = "off 取值" if seg.role == "I" else "停顿之后 off 取值"
                msg = f"{key_label(key)} {seg.role} m≠0 的块 {bad_m}、首块后重同步 {bad_rs}、{what} {offs} 种{extra}"
                parts.append(("FAIL", msg))
            else:
                parts.append(("PASS", ""))
    if skipped:
        parts.append(("NOTE", f"{skipped} 个 variant=toggle 的格归 A2，未计入 A1"))
    held = [(k, p) for k, p in stalls if p["stall_blocks"]]
    if held:  # 逐格的停顿时长见格观察；这里只给一句总的，并写明上限与来源
        worst = max(held, key=lambda kp: kp[1]["stall_ns"])[1]
        parts.append(("NOTE", f"Out 端起播处的时间线停顿：{len(held)} 格有，最长 {worst['stall_ns'] // 1_000_000} ms（{worst['stall_blocks']} 块），"
                              f"允许上限 {A1_STALL_MAX_NS // 1_000_000} ms（现场最长 56 ms 的约 2 倍）；In 端不放宽"))  # fmt: skip
    return fold("A1", parts, cover)


def regimes_of(blocks):
    """A2 的处理形态。blocks 是 In 端按 seq 排好的块。→ [(形态, 首块下标, 末块下标)]，相邻两项的形态必不相同。
    Cubase 的 ASIO-Guard 预取用线程池（现场约 32 个线程、几乎每块都换），实时只用 1 至 2 个线程；
    只看「相邻块 thread 不同」会把线程池里的轮换数成切换（现场 CB-02 数出 22 次，按键 12 次）。所以按形态数：
    - 先把块按 thread 切成连续的段。一段连续在同一线程上的时间跨度（首块到末块的 mono_ns）不短于 A2_REGIME_MIN_NS，
      这一段本身就是一种形态：专用线程；
    - 其余的段（线程来回换得太快、哪个线程都占不满 A2_REGIME_MIN_NS）连成一片，这一片里出现的不同线程不少于
      A2_POOL_MIN_THREADS 个就是「线程池」形态，否则以这片里用到的线程集合为形态；
    - 迟滞：一种形态要持续 A2_REGIME_MIN_NS 才算数。更短的（现场实时线程切回预取前有 5 块跑在另一个线程上，不到 1 ms；
      线程偶尔多换一次）并进相邻的形态，不算一次切换；两头形态相同的话，中间的杂散段整个吃掉；
    - 线程池内部的轮换不算切换；两个专用线程之间的 A→B 算一次。"""
    if not blocks:
        return []
    runs, i = [], 0  # 连续同线程的段 (线程, 首块, 末块)
    while i < len(blocks):
        j = i
        while j + 1 < len(blocks) and blocks[j + 1]["thread"] == blocks[i]["thread"]:
            j += 1
        runs.append((blocks[i]["thread"], i, j))
        i = j + 1

    def long_run(r):
        return blocks[r[2]]["mono_ns"] - blocks[r[1]]["mono_ns"] >= A2_REGIME_MIN_NS

    pieces, k = [], 0  # (形态 或 None=杂散, 首块, 末块)
    while k < len(runs):
        if long_run(runs[k]):
            pieces.append((("T", runs[k][0]), runs[k][1], runs[k][2]))
            k += 1
            continue
        k2 = k
        while k2 + 1 < len(runs) and not long_run(runs[k2 + 1]):
            k2 += 1
        first, last = runs[k][1], runs[k2][2]
        threads = {r[0] for r in runs[k : k2 + 1]}
        if blocks[last]["mono_ns"] - blocks[first]["mono_ns"] < A2_REGIME_MIN_NS:
            label = None  # 这一片本身也不够久：杂散
        elif len(threads) >= A2_POOL_MIN_THREADS:
            label = ("POOL",)
        else:
            label = ("S", tuple(sorted(threads)))
        pieces.append((label, first, last))
        k = k2 + 1
    regs = []
    for label, first, last in pieces:
        if label is None:
            continue
        if regs and regs[-1][0] == label:
            regs[-1] = (label, regs[-1][1], last)  # 同一种形态隔着杂散段又出现：并成一段
        else:
            regs.append((label, first, last))
    return regs


def _mode_n(blocks):
    return Counter(b["n"] for b in blocks).most_common(1)[0][0] if blocks else None


def switch_points(seg):
    """A2 的切换点：显式的监听 / 录音使能事件，加上 In 端处理形态（regimes_of）的变化点。
    VST3 不通知监听键与录音使能键，探针从不写这两个事件；ASIO-Guard 开着时，这类切换会把轨从预取（线程池）挪到实时线程，
    所以处理形态变了，切换时刻取新形态第一块的 mono_ns，窗口起点取它之前的一块。两种形态之间夹着被迟滞丢弃的杂散段时，
    之前的一块是杂散段的末块，不是旧形态的末块（杂散段本身不是形态，不当切换点）。
    - 显式事件前后 SLICE_NS 内的形态变化算同一次切换，不重复数。
    - 推断出的切换之后 SLICE_NS 内又出现的形态变化并入这一次（folded），不单独算：宿主线程池调度、或者用户按得比窗口还快，
      都分不出来；并入之后，窗口不会被这类变化截断，一次切换后的两次重同步也就拆不开。
    - 块长 n 的变化只作旁证（见 judge_a2：切换点前后各 SLICE_NS 内最常见的块长不同），不当切换。
    返回 {explicit: [时刻], inferred: [(切换时刻, 前一块的时刻)], folded, regimes: 出现过几种形态, threads}。"""
    explicit = sorted(e["t_ns"] for e in seg.events if e["name"] in TOGGLES)
    blocks = sorted(seg.blocks, key=lambda b: b["seq"])
    regs = regimes_of(blocks)
    changes = [(blocks[b1]["mono_ns"], blocks[b1 - 1]["mono_ns"]) for _, (_, b1, _) in zip(regs, regs[1:])]  # (新形态首块, 它之前的一块)
    inferred, folded, last = [], 0, None
    for t, t_prev in changes:
        if any(abs(t - e) < SLICE_NS for e in explicit):
            continue
        if last is not None and t - last < SLICE_NS:
            folded += 1
            continue
        inferred.append((t, t_prev))
        last = t
    return {
        "explicit": explicit, "inferred": inferred, "folded": folded,
        "regimes": len({label for label, _, _ in regs}),
        "threads": len({b["thread"] for b in blocks}),
    }  # fmt: skip


def toggle_counts(s):
    """各 A2 case 的切换点统计 → {case: {explicit, inferred, regimes, folded}}：toggle 格里恰有 1 个 In 端的才数。
    judge_a2 拿它对账现场次数；judge_a7 拿它判断推断出来的切换点有没有经 A2 确认过（两处必须是同一份数字）。"""
    out = {}
    for key, cell in _cells(s):
        if key[0] not in A2_CASES or key[1] != "toggle" or len(cell["I"]) != 1:
            continue
        sw = switch_points(cell["I"][0])
        got = out.setdefault(key[0], {"explicit": 0, "inferred": 0, "regimes": 0, "folded": 0})
        got["explicit"] += len(sw["explicit"])
        got["inferred"] += len(sw["inferred"])
        got["folded"] += sw["folded"]
        if sw["inferred"]:
            got["regimes"] = max(got["regimes"], sw["regimes"])
    return out


def toggle_case_check(case, got, want):
    """一个 case 的切换次数对账（--toggles 现场次数、形态数上限）→ (状态, 说明)，没有话要说返回 None。
    状态 INCOMPLETE = 这个 case 的切换没能确认；NOTE = 次数与现场相符。judge_a2 照此出结论，
    judge_a7 用它判断推断出来的切换点能不能当容忍窗口的依据（只有没有 INCOMPLETE 才行）。"""
    n = got["explicit"] + got["inferred"]
    how = f"显式事件 {got['explicit']}、按处理形态切换推断 {got['inferred']}"
    if 0 < n < MIN_TOGGLES:  # 一次都没有的格已经各自说明过原因
        folded = f"；另有 {got['folded']} 次形态变化挤在上一次切换后 {SLICE_NS // 1_000_000_000} s 内，并入了那一次" if got["folded"] else ""
        return "INCOMPLETE", f"{case} 只有 {n} 次切换（{how}{folded}），不足 {MIN_TOGGLES}"
    if n and want is not None and abs(n - want) > A2_TOGGLE_TOLERANCE:
        return "INCOMPLETE", f"{case} 数出 {n} 次切换（{how}），与现场记录的 {want} 次相差超过 {A2_TOGGLE_TOLERANCE}，对不上"
    if got["inferred"] and want is None:
        return "INCOMPLETE", (f"{case} 的切换点里有 {got['inferred']} 次是按处理形态推断的（{how}），没有给现场按键次数（--toggles {case}:<次数>）可对照，"
                              "推断不能单独撑出通过：宿主线程池在稳态下也会换线程，和用户的按键分不开")  # fmt: skip
    if got["inferred"] and got["regimes"] > A2_MAX_REGIMES:
        return "INCOMPLETE", (f"{case} 的推断切换出现了 {got['regimes']} 种不同的处理形态，多于线程池预取与专用线程实时两种，更像线程池调度而不是监听切换；"
                              "拿到一份真实的形态分布之前不据此通过")  # fmt: skip
    if n and want is not None:
        return "NOTE", f"{case} 数出 {n} 次切换（{how}），与现场记录的 {want} 次相符（相差不超过 {A2_TOGGLE_TOLERANCE}）"
    return None


def toggle_reviews(s, toggles):
    """各 A2 case 的切换次数对账 → {case: (状态, 说明) 或 None}（None = 没有话要说）：toggle_counts 里有的 case 都在。
    这是**整个 case 的性质**：`--toggles` 填的是整个 case 的现场按键次数，不是某一段的，所以只在主会话里算一次，
    A2 拿它出结论，A7 拿它判断推断出来的切换点能不能给容忍（状态是 NOTE 才算确认）；
    --exclude 的影子会话沿用主会话的结论，不再用只含被排除的段的计数去对整场的现场次数（见 shadow_reviews）。"""
    toggles = toggles or {}
    return {case: toggle_case_check(case, got, toggles.get(case)) for case, got in sorted(toggle_counts(s).items())}


def shadow_reviews(reviews, sh, toggles):
    """影子会话（被排除的段单独成会话）用的对账结论：沿用主会话的 reviews，但只沿用对这些段仍然成立的部分。
    - 切换**次数**是整个 case 的性质（`--toggles` 填的是整个 case 的现场按键次数）：只含被排除的段的计数必然对不上，
      拿它去对会把被排除的 toggle 格的切换点一律当成没经确认，切换造成的 hostNs 回退就被说成「被排除的段本来就不通过」，所以沿用主会话的。
    - 处理**形态数**是逐段的性质：主会话的结论只说明留下的格里形态不多于 A2_MAX_REGIMES 种，不说明被排除的格也是。
      沿用之前先用影子自己的 toggle_counts(sh) 核一遍，多于 A2_MAX_REGIMES 种就不沿用，该 case 退回被排除的段自己的对账
      （形态多于上限，对账只会是数据不全，推断点得不到容忍，其前后的回退照常算非递增，A7 在影子里不通过，记进 excluded_fail）。
    主会话对这个 case 没有结论可沿用时（一个 toggle 格都没有，整个 case 的切换格都被排除了；或留下的格里一次切换都没数到，
    toggle_case_check 没有话要说），同样用被排除的段自己的计数对账——此时 `--toggles` 说的只可能是这些段里的按键。"""
    own = toggle_counts(sh)
    inherited = {case: chk for case, chk in reviews.items() if chk is not None and own.get(case, {}).get("regimes", 0) <= A2_MAX_REGIMES}
    return {**toggle_reviews(sh, toggles), **inherited}


def judge_a2(s, toggles=None, reviews=None):
    """toggles = {case: 现场实际按键次数}（--toggles）。显式事件是宿主亲口报的切换；按处理形态推断出来的切换
    可能是用户按的，也可能只是线程池调度，所以推断的切换单独撑不出通过：要现场次数对得上、形态只有预取与实时两种。
    reviews = 各 case 的次数对账（toggle_reviews）：不给就按本会话的格算；影子会话传 {}，不再对账整个 case 的次数
    （对账只会产出数据不全或相符的说明，不会让逐格的判定变成不通过，只会把整场的现场次数误记到被排除的段名下）。"""
    toggles = toggles or {}
    parts, cover = [], []
    reviews = toggle_reviews(s, toggles) if reviews is None else reviews
    for key, cell in _cells(s):
        if key[0] not in A2_CASES or key[1] != "toggle":
            continue
        cover.append(key[0])
        label = key_label(key)
        if len(cell["I"]) != 1:
            parts.append(("INCOMPLETE", f"{label} In 端记录数为 {len(cell['I'])}，应恰有 1 个"))
            continue
        seg = cell["I"][0]
        sw = switch_points(seg)
        explicit, inferred = sw["explicit"], sw["inferred"]
        how = f"显式事件 {len(explicit)}、按处理形态切换推断 {len(inferred)}"
        bad, why = _usable(seg)
        if bad:
            parts.append((bad, why))
            continue
        # 窗口按 mono_ns 排好序后用二分取，不为每个切换点扫整段
        ordered = sorted(seg.blocks, key=lambda b: b["mono_ns"])
        ts = [b["mono_ns"] for b in ordered]
        pref = [0]
        for b in ordered:
            pref.append(pref[-1] + b["rs"])
        # (切换时刻, 窗口起点)：显式事件的窗口也从事件之前的最后一块算起，与推断的切换一致
        points = sorted([(e, ts[bisect_left(ts, e) - 1] if bisect_left(ts, e) else e) for e in explicit] + inferred)
        if not points:
            parts.append(("INCOMPLETE", f"{label} 这一格里处理形态没有变化（ASIO-Guard 关着时监听可能不改处理线程），也没有显式切换事件"))
            continue
        worst, thin = 0, 0
        for t, start in points:
            # 窗口从切换块之前的一块算起（切换瞬间旧线程上最后一块的重同步也算这一次），到 SLICE_NS 之后为止；
            # 只有用户真按了的显式事件才截断窗口，推断出的切换不截断：线程池的又一次调度不能把一次切换后的重同步拆开
            k = bisect_right(explicit, t)
            end = t + SLICE_NS if k >= len(explicit) else min(t + SLICE_NS, explicit[k])
            lo, hi = bisect_left(ts, start), bisect_left(ts, end)
            if hi - lo < MIN_SLICE_BLOCKS:
                thin += 1
            worst = max(worst, pref[hi] - pref[lo])
        if thin:
            parts.append(("INCOMPLETE", f"{label} 有 {thin} 次切换后的窗口里块数不足 {MIN_SLICE_BLOCKS}（切换 {len(points)} 次：{how}）"))
            continue
        ds = {b["d"] for b in seg.blocks}
        # 块长变化只作旁证：切换点前后各 SLICE_NS 内最常见的块长不同
        with_n = 0
        for t, _ in points:
            before, after = _mode_n(ordered[bisect_left(ts, t - SLICE_NS) : bisect_left(ts, t)]), _mode_n(ordered[bisect_left(ts, t) : bisect_left(ts, t + SLICE_NS)])
            with_n += before is not None and after is not None and before != after
        detail = f"切换 {len(points)} 次（{how}），其中 {with_n} 次伴随块长变化（旁证，不计入切换），处理形态 {sw['regimes']} 种、线程 {sw['threads']} 个"
        if sw["folded"]:
            detail += f"，另有 {sw['folded']} 次形态变化落在上一次切换后 {SLICE_NS // 1_000_000_000} s 内，并入那一次"
        if worst > 1 or len(ds) > 1:
            parts.append(("FAIL", f"{label} 每次切换后重同步最多 {worst} 次、D 取值 {len(ds)} 种；{detail}"))
        else:
            parts.append(("PASS", f"{label} {detail}"))
    for case, chk in sorted(reviews.items()):
        if chk:
            parts.append(chk)
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


def _nearest_pairs(i_blocks, o_blocks):
    """每个 In 块配时间上（mono_ns）最近的 Out 块，一个 Out 块只配给离它最近的那个 In 块。→ [(in 块, out 块)]，按 Out 的时间排。
    同一次宿主回调里，链首的 In 与链尾的 Out 前后脚被调用，彼此在时间上最近；这一步只用来求两端 call_seq 的对应关系，
    不看 hostNs（A7 要判的就是 hostNs 两端是否相等，不能拿它来配对）。"""
    om = sorted(o_blocks, key=lambda b: b["mono_ns"])
    ot = [b["mono_ns"] for b in om]
    best = {}
    for b in i_blocks:
        k = bisect_left(ot, b["mono_ns"])
        near = [j for j in (k - 1, k) if 0 <= j < len(om)]
        if not near:
            continue
        j = min(near, key=lambda j: abs(ot[j] - b["mono_ns"]))
        dist = abs(ot[j] - b["mono_ns"])
        if j not in best or dist < best[j][0]:
            best[j] = (dist, b)
    return [(b, om[j]) for j, (_, b) in sorted(best.items())]


def call_seq_shift(i_blocks, o_blocks):
    """两端 call_seq 的对应关系：时间上最近邻的 (In, Out) 的 call_seq 差（Out − In）取众数。
    call_seq 是同进程所有探针实例共享的回调计数，In 与 Out 严格交替，所以「相邻」两块有两种配法（Out 紧跟 In，或 Out 紧挨在 In 之前），
    只看谁挨着谁分不出来；先开窗的一端会多出若干块，按排好序后相邻的 I/O 两两配（旧做法）会错开一位。
    正 = In 先于 Out，负 = Out 先于 In（错放）。次数相同取绝对值小的、再取正的。→ (差, 众数的次数, 最近邻对数)，没有可配的块返回 None。"""
    near = _nearest_pairs(i_blocks, o_blocks)
    if not near:
        return None
    hist = Counter(o["call_seq"] - i["call_seq"] for i, o in near)
    d = max(hist, key=lambda x: (hist[x], -abs(x), x))
    return d, hist[d], len(near)


def aligned_pairs(i_blocks, o_blocks):
    """只按 call_seq_shift 求出的那个差配对：In 块的 call_seq 加上这个差正好是某个 Out 块的 call_seq。
    → ((差, 众数次数, 最近邻对数) 或 None, [(in 块, out 块)])。"""
    sh = call_seq_shift(i_blocks, o_blocks)
    if sh is None:
        return None, []
    by_cs = {b["call_seq"]: b for b in o_blocks}
    return sh, [(b, by_cs[b["call_seq"] + sh[0]]) for b in i_blocks if b["call_seq"] + sh[0] in by_cs]


def _toggle_zone(seg_in, case, reviews, toggles):
    """toggle 格里容忍 hostNs 回退的依据（切换点与 A2 同一套：显式事件加推断）→ (给容忍的切换点, 不给容忍的推断点, 说明)，前两项是排好序的时刻列表。
    显式事件是宿主亲口报的，一律给容忍。按处理形态推断出来的点只有这个 case 的切换次数已被 A2 确认（toggle_case_check 没判数据不全：
    次数对上 --toggles、处理形态数不超过 A2_MAX_REGIMES）才给：没确认的点可能只是线程池调度，拿它前后各放宽 A7_TOGGLE_ZONE_NS、
    回退幅度又没有上限，会把真问题放过；A2 判数据不全的格，A7 也不替它背书。
    确认是整个 case 的性质，读 reviews（toggle_reviews：主会话算一次；影子会话沿用，见 shadow_reviews），不在这里按本会话的格重数。"""
    sw = switch_points(seg_in)
    explicit, inferred = sorted(sw["explicit"]), sorted(t for t, _ in sw["inferred"])
    if not inferred:
        return explicit, [], ""
    want = toggles.get(case)
    chk = reviews.get(case)
    if chk is not None and chk[0] == "NOTE":  # 次数与现场相符，且没有因形态数、次数不足而数据不全：A2 确认了
        return sorted(explicit + inferred), [], f"切换点含 {len(inferred)} 个按处理形态推断的，已由 A2 按现场次数（--toggles {case}:{want}）确认"
    return explicit, inferred, (f"{len(inferred)} 个按处理形态推断的切换点没有经 A2 确认，不给容忍"
                                f"（给 --toggles {case}:<次数>，次数对上、处理形态不多于 {A2_MAX_REGIMES} 种才确认，见 A2 一行）")  # fmt: skip


def _near(zone, t):
    """t 离 zone（排好序的时刻）里最近的一个不超过 A7_TOGGLE_ZONE_NS。"""
    k = bisect_left(zone, t)
    return any(0 <= m < len(zone) and abs(zone[m] - t) <= A7_TOGGLE_ZONE_NS for m in (k - 1, k))


def judge_a7(s, toggles=None, reviews=None):
    toggles = toggles or {}
    parts, cover = [], []
    reviews = toggle_reviews(s, toggles) if reviews is None else reviews  # 影子会话由 analyze 传主会话的结论，见 shadow_reviews
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
        sh, pairs = aligned_pairs(i.blocks, o.blocks)
        if sh is None or abs(sh[0]) > A7_SHIFT_MAX or sh[1] / sh[2] < A7_SHIFT_SHARE_MIN:
            if sh is None:
                how = "两端没有时间上挨着的块"
            elif abs(sh[0]) > A7_SHIFT_MAX:
                how = f"最近邻的 call_seq 差以 {sh[0]:+d} 为众数，绝对值超过 {A7_SHIFT_MAX}，两端不像在同一个回调计数器上"
            else:
                how = f"最近邻的 call_seq 差以 {sh[0]:+d} 为众数，只占 {sh[1]}/{sh[2]}，低于 {A7_SHIFT_SHARE_MIN:.0%}，两端的先后不稳定"
            parts.append(("INCOMPLETE", f"{key_label(key)} 两端的 call_seq 对不上：{how}，没法按 call_seq 配对"))
            continue
        shift = sh[0]
        both = [(a, b) for a, b in pairs if all(x["playing"] and x["host_ns_valid"] for x in (a, b))]
        if len(both) < MIN_STEADY_BLOCKS:
            parts.append(("INCOMPLETE", f"{key_label(key)} 按 call_seq 对齐后成对的回调 {len(pairs)}、其中两端都在播放且 hostNs 有效的 {len(both)}，不足 {MIN_STEADY_BLOCKS}"))
            continue
        mismatch = sum(1 for a, b in both if a["host_ns"] != b["host_ns"])
        # toggle 格：ASIO-Guard 切换时宿主 hostNs 会回退（现场最大一次 7.3 s，两端值相同）。回退落在切换点前后 A7_TOGGLE_ZONE_NS 内的容忍并计数，
        # 窗口外的照常算非递增；非 toggle 格没有切换点，一律不放宽
        zone, unconfirmed, basis = _toggle_zone(i, key[0], reviews, toggles) if (key[0] in A2_CASES and key[1] == "toggle") else ([], [], "")
        nonmono = tolerated = worst_back = unexplained = 0
        for sg in (i, o):
            seq = [(b["host_ns"], b["mono_ns"]) for b in sorted(sg.blocks, key=lambda b: b["seq"]) if b["playing"] and b["host_ns_valid"]]
            for (x, _), (y, t) in zip(seq, seq[1:]):
                if y > x:
                    continue
                if _near(zone, t):
                    tolerated += 1
                    worst_back = max(worst_back, x - y)
                else:
                    nonmono += 1
                    unexplained += _near(unconfirmed, t)
        ok = mismatch / len(both) <= HOSTNS_MISMATCH_RATIO and nonmono == 0
        order = "In 先于 Out" if shift > 0 else "Out 先于 In（错放）"
        zone_ms = A7_TOGGLE_ZONE_NS // 1_000_000
        back = f"（另有 {tolerated} 次落在切换点前后 {zone_ms} ms 内，已容忍，最大回退 {worst_back // 1_000_000} ms）" if tolerated else ""
        if unexplained:
            back += f"（非递增里有 {unexplained} 次落在没经确认的推断切换点前后 {zone_ms} ms 内，没有容忍）"
        msg = f"{key_label(key)} 按 call_seq 对齐（Out = In {shift:+d}，{order}）配对 {len(both)}，两端不等 {mismatch}，非递增 {nonmono}{back}" + (f"；{basis}" if basis else "")
        parts.append(("PASS" if ok else "FAIL", msg))
    return fold("A7", parts, cover)


def short_clones(s):
    """[(实例序号, Log)]：没有 C 行、又活不到 SHORT_LIVED_NS 的实例（Render in Place / Freeze 的临时克隆）。"""
    return [(n, lg) for n, lg in enumerate(s.logs, start=1) if lg.is_short_clone()]


def _clone_list(clones):
    shown = "、".join(f"实例 {n}（{lg.header['role']}，{lg.span_ns() // 1_000_000} ms）" for n, lg in clones[:6])
    return shown + (f" 等共 {len(clones)} 个" if len(clones) > 6 else "")


def judge_a8(s):
    parts, clones, crashed = [], [], []
    for label in s.broken:
        parts.append(("FAIL", f"{label} 只有不完整的内容（实例刚创建就崩溃，或日志写到一半被截断）"))
    for n, lg in enumerate(s.logs, start=1):
        who = f"实例 {n}（{lg.header['role']}）"
        ts = [x["t_ns"] for x in lg.summaries]
        stall = max([b - a for a, b in zip(ts, ts[1:])] + [0])
        trips = max([x["trips"] for x in lg.summaries] + [0])
        problems = []
        if lg.truncated or lg.closing is None:
            if lg.crashed:  # --crashed 人工标注：整机崩溃把日志截断了，不算插件崩溃；其余问题（摘要间隔、跳闸）照常判
                crashed.append((n, lg))
            else:
                problems.append("日志没有收尾（疑似崩溃或被强杀）")
        if stall > STALL_NS:
            problems.append(f"摘要间隔超限（最长 {stall // 1_000_000} ms，阈值 {STALL_NS // 1_000_000} ms；也可能是写摘要的线程被宿主占住）")
        if trips:
            problems.append(f"跳闸 {trips} 次")
        if problems:  # 克隆实例也一样：没收尾、摘要里记了跳闸都照常不通过，只豁免「摘要行不足」
            parts.append(("FAIL", f"{who} " + "；".join(problems)))
        elif len(ts) < 2 and lg.is_short_clone():
            clones.append((n, lg))
        elif len(ts) < 2:
            parts.append(("INCOMPLETE", f"{who} 摘要行不足 2 条，无法判断卡死"))
        else:
            parts.append(("PASS", ""))
    if clones:  # 汇成一条放在最后，免得挤掉前面真正的不通过说明
        parts.append(("NOTE", f"{len(clones)} 个没有 C 行、存活短于 {SHORT_LIVED_NS // 1_000_000} ms 的实例不计入稳定性判定"
                              f"（疑似 Render in Place / Freeze 的克隆，来不及写摘要）：{_clone_list(clones)}；"
                              "跳闸只记在摘要行里，它们没有摘要行，跳闸与卡死无从判断（没收尾的仍判不通过）"))  # fmt: skip
    if crashed:
        # 整机崩溃时一次写盘（探针一批 B 行一次 write）可能只落下前半截，文件末尾留下写到一半的一行：解析时已丢掉，这里写明丢了几字节
        who = "、".join(f"实例 {n}（{lg.header['role']}" + (f"，末行没有换行结尾，已丢弃 {lg.partial_bytes} 字节残行" if lg.truncated else "") + "）" for n, lg in crashed)
        parts.append(("NOTE", f"{len(crashed)} 个实例按人工标注（--crashed）因整机崩溃中断，没有收尾不计为插件崩溃：{who}；是否确属整机崩溃请人工确认"
                              "（其余问题，摘要间隔与跳闸，照常判）"))  # fmt: skip
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
    if s.orphan_lost:  # 孤块之间、孤块之前缺的块序号：实例级，不归任何格，也不当成尾部丢块记给最后一格
        out.append(f"早于第一个 C 行的块之间（含实例开头）缺 {s.orphan_lost} 个块序号、{s.orphan_gaps} 处：属实例级，不归任何格，不计入各格的序号缺口")
    if s.dups:
        out.append(f"{len(s.dups)} 个格同键重复记录（如 {key_label(s.dups[0])}）：旧文件须挪走后重采，相关判据按数据不全处理")
    if s.broken:
        out.append(f"{len(s.broken)} 个日志文件不完整，已记入 A8")
    lost = [n for n, lg in enumerate(s.logs, start=1) if dropped_of(lg) > lg.orphan_lost]  # 丢弃全在孤块之间（上一条已单独说）的实例，不算影响各格
    shown = [n for n in lost if s.attribution.get(n, (0, 1, True))[1]]  # 丢弃全部归给了被排除的段（归给未排除段的个数是 0）的实例另说
    if shown:
        out.append(f"{len(shown)} 个实例的探针报告过丢弃记录，相关格按数据不全处理")
    if len(shown) < len(lost):
        out.append(f"{len(lost) - len(shown)} 个实例的探针报告过丢弃记录，已全部归给被排除的段（不计入判据）")
    for n, (absorbed, charged, by_summary) in sorted(s.attribution.items()):
        if by_summary:
            out.append(f"实例 {n} 有被排除的段：缺失的块序号按摘要里的累计块数归属，{absorbed} 个归给被排除的段（不计入判据），"
                       f"{charged} 个归给未排除的段（这些段按数据不全处理）")  # fmt: skip
        else:
            out.append(f"实例 {n} 有被排除的段，但某个 C 行之前没有摘要行可用，{charged} 个缺失的块序号无从归属，各个未排除的段都按数据不全处理")
    return out


def _stop_name(e):
    if e["name"] == "set_processing" and e["a"] == 0:
        return "set_processing(0)"
    return e["name"] if e["name"] in ("reset", "release") else None


def stop_evidence(segs):
    """CB-05 停用格：停用事件要落在块流中断里才算数。JUCE 的 VST3 包装层把 setProcessing(false) 转成 reset()，
    析构时 terminate() 与 setActive(false) 都会写 release，宿主起播时也会 reset；这些事件不带「用户停用了插件」的信息，
    只有它们和块流的中断对得上（停用 5 s，实例不再收到回调）才是停用。
    - 实例自己有块：事件落在它的某一次块间隔里（间隔不短于 DEACT_GAP_NS，前后都有块），位置在间隔的前半（停用发生在断开的时候；
      起播的 reset 在间隔末尾，不算），可以比间隔前的最后一块早 DEACT_LEAD_NS；
    - 实例整段没有块（一直停用）：本格另一端的块流在事件前后都至少延续 DEACT_GAP_NS（起播的 reset、析构的 release 在块流两头，不算）。
    返回 (落在中断里的 {名: 条数}, 不在中断里的 {名: 条数}, 说明中断的文字, 各端最长块间隙 ms)。"""
    all_t = [b["mono_ns"] for g in segs for b in g.blocks]
    matched, loose, via, longest = {}, {}, [], 0
    for g in segs:
        ts = sorted(b["mono_ns"] for b in g.blocks)
        gaps = [(a, b) for a, b in zip(ts, ts[1:]) if b - a >= DEACT_GAP_NS]
        longest = max([longest] + [b - a for a, b in zip(ts, ts[1:])])
        for e in g.events:
            name, at = _stop_name(e), e["t_ns"]
            if name is None:
                continue
            if ts:
                hit = [b - a for a, b in gaps if a - DEACT_LEAD_NS <= at <= a + (b - a) // 2]
                how = f"块流中断 {max(hit, default=0) // 1_000_000} ms"
            else:
                hit = bool(all_t) and min(all_t) + DEACT_GAP_NS <= at <= max(all_t) - DEACT_GAP_NS
                how = "该端整段没有块、另一端的块流在事件前后都在"
            if hit:
                matched[name] = matched.get(name, 0) + 1
                if how not in via:
                    via.append(how)
            else:
                loose[name] = loose.get(name, 0) + 1
    return matched, loose, "；".join(via), longest // 1_000_000


_ROLE_ZH = {"I": "In", "O": "Out"}


def replaced_instances(s, segs):
    """Disable Track 的证据。Cubase 15 的 Disable Track 会销毁插件实例（get_state、release、destruct），Enable 时在同一个进程里新建实例
    （新日志文件：set_state 组 A、prepare、不久后 paired）。所以：本段内旧实例写了 destruct，同 pid、同角色、在它析构之后才出生的新实例
    在 REPLACE_WINDOW_NS 内出现 set_state，并在同一时限内（从旧实例析构起算）配对，就是「实例被替换」，是这一格的停用证据。
    → 每个写了 destruct 的实例一条 {role, found: 找到新实例, set_state_ms, paired_ms}（从 destruct 起算；没找到或没配对是 None）。"""
    out = []
    for g in segs:
        dts = [e["t_ns"] for e in g.events if e["name"] == "destruct"]
        if not dts:
            continue
        t_d, hit = max(dts), None
        for lg in s.logs:
            if lg is g.log or lg.header["pid"] != g.log.header["pid"] or lg.header["role"] != g.role or lg.first_ns() < t_d:
                continue
            sets = [e["t_ns"] for e in lg.events if e["name"] == "set_state" and t_d <= e["t_ns"] <= t_d + REPLACE_WINDOW_NS]
            if sets and (hit is None or min(sets) < hit[0]):
                paired = [e["t_ns"] for e in lg.events if e["name"] == "paired" and min(sets) <= e["t_ns"] <= t_d + REPLACE_WINDOW_NS]
                hit = (min(sets), min(paired) if paired else None)
        out.append({
            "role": g.role, "found": hit is not None,
            "set_state_ms": (hit[0] - t_d) // 1_000_000 if hit else None,
            "paired_ms": (hit[1] - t_d) // 1_000_000 if hit and hit[1] is not None else None,
        })  # fmt: skip
    return out


def _replaced_text(r):
    who = f"{_ROLE_ZH[r['role']]} 端"
    if not r["found"]:
        return f"{who}旧实例 destruct 后 {REPLACE_WINDOW_NS // 1_000_000_000} s 内没有同 pid 同角色的新实例 set_state（超时或没有新实例）"
    if r["paired_ms"] is None:
        return f"{who}旧实例 destruct 后 {r['set_state_ms']} ms 新实例 set_state，但在 {REPLACE_WINDOW_NS // 1_000_000_000} s 内没有重新配对"
    return f"{who}旧实例 destruct 后 {r['set_state_ms']} ms 同 pid 新实例 set_state、{r['paired_ms']} ms 重新配对（已恢复配对）"


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
        if not full:
            return [("INCOMPLETE", f"{label} 没有一端达到 {MIN_STEADY_BLOCKS} 块")]
        matched, loose, via, longest = stop_evidence(segs)
        rep = replaced_instances(s, segs) if v == "disable-track" else []
        moved = "；".join(_replaced_text(r) for r in rep)
        if matched:  # 块流中断里有停用事件，已经够了；旧实例被替换的情况另附在后面
            seen = "、".join(f"{k} {n}" for k, n in matched.items())
            return [("PASS", f"{label} 单端 {len(full)} 个实例有块，停用事件落在块流中断里（{seen}）：{via}" + (f"；{moved}" if moved else ""))]
        if rep and all(r["paired_ms"] is not None for r in rep):
            return [("PASS", f"{label} 单端 {len(full)} 个实例有块，实例被替换（Disable Track 销毁实例）：{moved}")]
        if rep:
            return [("INCOMPLETE", f"{label} 本段内旧实例 destruct 了（Disable Track 会销毁实例），但新实例没有按时恢复：{moved}")]
        if not loose:
            return [("INCOMPLETE", f"{label} 本段内缺停用的生命周期事件（set_processing(0)、reset、release 都没有）")]
        seen = "、".join(f"{k} {n}" for k, n in loose.items())
        return [("INCOMPLETE", f"{label} 本段内有停用事件（{seen}），但都不在不短于 {DEACT_GAP_NS // 1_000_000} ms 的块流中断里"
                               f"（各端最长块间隙 {longest} ms）：起播的 reset、析构的 release 不算停用")]  # fmt: skip
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
    cells, bypass, smart, misplaced, a1_stall, replaced = [], [], [], [], [], []
    pairs_total = first_total = 0
    for key, cell in _cells(s):
        label = key_label(key)
        segs = cell["I"] + cell["O"]
        if is_cat1(key):
            if len(cell["I"]) == 1 and len(cell["O"]) == 1:
                pr = _nearest_pairs(cell["I"][0].blocks, cell["O"][0].blocks)
                pairs_total += len(pr)
                first_total += sum(1 for i, o in pr if o["call_seq"] > i["call_seq"])
                if key[0] in A1_CASES and key[1] != "toggle" and cell["O"][0].blocks:  # A1 的 Out 端起播停顿：逐格报出
                    prof = out_off_profile(sorted(cell["O"][0].blocks, key=lambda b: b["seq"]))
                    a1_stall.append({"cell": label, "stall_ms": prof["stall_ns"] // 1_000_000, "stall_blocks": prof["stall_blocks"], "runs": prof["runs"]})
        else:
            cells += [
                {"case": key[0], "variant": key[1], "cell": label, "state": st, "note": n}
                for st, n in _cell_evidence(s, key, cell)
            ]
        v = key[1]
        if v == "disable-track":
            replaced += [{"cell": label, **r} for r in replaced_instances(s, segs)]
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
            pr = _nearest_pairs(ib, ob)
            misplaced.append({
                "cell": label, "verdict": "两端都在", "pairs": len(pr),
                "in_first": sum(1 for i, o in pr if o["call_seq"] > i["call_seq"]),
                "host_ns_equal": sum(1 for i, o in pr if i["host_ns_valid"] and o["host_ns_valid"] and i["host_ns"] == o["host_ns"]),
                "off_in": sorted({b["off"] for b in ib}), "off_out": sorted({b["off"] for b in ob}),
            })  # fmt: skip
    bad = any(c["state"] == "INCOMPLETE" for c in cells)
    return {
        "status": "INCOMPLETE" if bad else "PASS",
        "order": {"pairs": pairs_total, "in_first": first_total},
        "cells": cells, "bypass": bypass, "smart": smart, "misplaced": misplaced, "a1_stall": a1_stall, "replaced": replaced,
    }


def render_observations(obs):
    o = obs["order"]
    out = ["", "## 格观察", "", "不带判据编号、不阻断；本格证据缺失的格记「数据不全」。", ""]
    out.append(f"- 同通道顺序：成对满块的格里，按 call_seq 是 In 先于 Out 的回调 {o['in_first']}/{o['pairs']}")
    for c in obs["cells"]:
        out.append(f"- [{VERDICT_ZH.get(c['state'], '说明')}] {c['note']}")
    for a in obs["a1_stall"]:
        out.append(f"- A1 Out 端起播时间线停顿 {a['cell']}：{a['runs']} 段播放里最长 {a['stall_ms']} ms（{a['stall_blocks']} 块），上限 {A1_STALL_MAX_NS // 1_000_000} ms")
    for r in obs["replaced"]:
        out.append(f"- 实例替换 {r['cell']}：" + _replaced_text(r))
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
    """['文件名:选择子', ...] → [(文件名, 前缀, 数值, 选择子原文)]；写法不对抛 ValueError（用法错误，退出码 2）。
    文件名只取最后一级（带了目录也行），只用来在日志目录里找文件，不进任何输出；
    选择子是 C 行序号或 t_ns，`#N` 强制按序号、`t=N` 强制按 t_ns。"""
    out = []
    for text in raw:
        name, sep, sel = text.rpartition(":")
        name = re.split(r"[\\/]", name)[-1]
        m = _EXCLUDE_SEL.match(sel)
        if not sep or not name or not m:
            raise ValueError
        out.append((name, m.group(1) or "", int(m.group(2)), sel))
    return out


_TOGGLES_ARG = re.compile(r"^([A-Z]{2}-[0-9]{2}):(0|[1-9][0-9]{0,3})$")


def parse_toggles(raw):
    """['CB-02:12', ...] → {case: 现场按键次数}；case 不是 A2 判的 case、写法不对、同一个 case 写两次抛 ValueError（退出码 2）。"""
    out = {}
    for text in raw:
        m = _TOGGLES_ARG.match(text)
        if not m or m.group(1) not in A2_CASES or m.group(1) in out:
            raise ValueError
        out[m.group(1)] = int(m.group(2))
    return out


def resolve_excludes(logs, specs):
    """→ [(Log, 该 Log.cases 里的下标, 选择子)]。裸数字先按 t_ns 找、找不到再当序号；两种读法指向不同的 C 行就报错，不替用户猜；
    同一个文件的同一个 C 行指两次也报错（多半是写错了，数段数时不能算两遍）。"""
    out, seen = [], {}
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
        if (id(lg), hit[0]) in seen:
            raise ExcludeError(f"第 {n} 个参数与第 {seen[(id(lg), hit[0])]} 个参数指向同一个 C 行")
        seen[(id(lg), hit[0])] = n
        out.append((lg, hit[0], param))
    return out


def parse_crashed(raw):
    """['文件名', ...] → [文件名]；文件名只取最后一级，只用来在日志目录里找文件，不进任何输出；空的抛 ValueError（用法错误，退出码 2）。"""
    out = []
    for text in raw:
        name = re.split(r"[\\/]", text)[-1]
        if not name:
            raise ValueError
        out.append(name)
    return out


def mark_crashed(logs, names):
    """--crashed：把这些实例标成「日志因整机崩溃中断」。文件不在目录里、同一个文件写两次、
    被标注的实例其实有收尾行（Z 行：它是正常结束的，不是被崩溃截断的），都按 --exclude 的办法报 ExcludeError（退出码 5）。
    消息只带参数的序号，不回显文件名。
    能标注的是 logs 里的文件，包括末行写到一半的：parse_log 对没有换行结尾的末行丢掉并记下字节数（Log.partial_bytes），其余照常解析，
    所以整机崩溃留下的残缺末行不挡标注；只有连文件头（魔数行与 H 行）都没写完的文件进不了 logs（load_dir 把它们记成 broken：没有角色、
    没有 pid，什么都分析不了，也分不清是整机崩溃还是插件刚创建就崩溃），--crashed 不认它们，A8 照样不通过。"""
    seen = {}
    for n, name in enumerate(names, start=1):
        lg = next((g for g in logs if g.name == name), None)
        if lg is None:
            raise ExcludeError(f"第 {n} 个 --crashed 参数：日志目录里没有这个文件（或它在文件头写完之前就被截断了，没有可分析的内容）")
        if id(lg) in seen:
            raise ExcludeError(f"第 {n} 个 --crashed 参数与第 {seen[id(lg)]} 个参数指向同一个文件")
        if lg.closing is not None:
            raise ExcludeError(f"第 {n} 个 --crashed 参数：这个实例的日志有收尾行，是正常结束的，不是被整机崩溃截断的")
        seen[id(lg)] = n
        lg.crashed = True


# 不参与「被排除的段若计入」归因的判据。A3 读导出比对行（不属于任何段），A8 读每个实例的收尾行、摘要与跳闸（实例级，记不到某个 C 行的段上）；
# 影子会话共用同一批 logs / compares，别的实例的失败（未排除实例跳闸、比对不过）会被当成被排除的段「本来就不通过」。
# 这两项在主会话里本来就读全部输入，排除一个段不会让它们少看任何东西，所以不必再算一遍。其余判据只读段（格）里的块与事件。
# 唯一读整场口径输入的另一处是 --toggles（整个 case 的现场按键次数）：它是 case 的性质，不是某一段的，影子会话不重数、沿用主会话的对账结论
# （toggle_reviews / shadow_reviews），所以 A7 在影子里的容忍取决于主会话的确认，A2 在影子里不再对账次数
SHADOW_SESSION_WIDE = ("A3", "A8")


def _judge(sess, iid, fn, salt, toggles, reviews=None):
    if iid == "A6":
        return fn(sess, salt)
    return fn(sess, toggles, reviews) if iid in ("A2", "A7") else fn(sess)  # A7 的 toggle 格要知道 A2 有没有确认推断出来的切换点


def analyze(path, salt, exclude=(), toggles=None, crashed=()):
    logs, compares, broken = load_dir(path)
    s = Session(logs, compares, broken, resolve_excludes(logs, exclude))
    mark_crashed(logs, crashed)
    reviews = toggle_reviews(s, toggles)  # 各 case 的切换次数对账：整个 case 的性质，主会话算一次，A2 出结论、A7 判断推断的切换点有没有确认都用它
    items = [_judge(s, iid, fn, salt, toggles, reviews) for iid, fn in JUDGES]
    would_fail = []
    if s.hidden:  # --exclude 只许拿掉数据不全的段：被排除的段单独判一遍，本来会不通过的项照样记不通过
        sh = s.shadow()
        inherited = shadow_reviews(reviews, sh, toggles)  # A7 沿用主会话的确认；A2 在影子里不对账次数（传 {}）
        for item, (iid, fn) in zip(items, JUDGES):
            if iid in SHADOW_SESSION_WIDE:
                continue
            was = _judge(sh, iid, fn, salt, toggles, {} if iid == "A2" else inherited)
            if was["verdict"] == "FAIL":
                would_fail.append(item["id"])  # 主会话里这一项已经不通过也要记：被排除的段本来就不通过，是单独的一条事实
                if item["verdict"] != "FAIL":
                    item["verdict"] = "FAIL"
                    item["notes"] = ["被排除的段若计入，本项不通过（--exclude 拿不掉不通过的段）：" + "；".join(was["notes"][:2])] + item["notes"]
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
        "excluded_fail": would_fail,
        "toggles": dict(sorted((toggles or {}).items())),
        "crashed": [{"instance": n, "role": lg.header["role"]} for n, lg in enumerate(logs, start=1) if lg.crashed],
    }


def render_md(res):
    i = res["inputs"]
    out = ["# 探针分析结果", "", f"输入：实例日志 {i['logs']} 个（In {i['in']}、Out {i['out']}、不完整 {i['broken']}），导出比对 {i['compares']} 行", ""]
    if res["excluded"]:
        out += [f"**本次按 `--exclude` 排除了 {len(res['excluded'])} 段，结论不含这些段，见文末「已排除的段」；"
                "是否确属误记请人工确认。**", ""]  # fmt: skip
    if res["crashed"]:
        out += [f"**本次按 `--crashed` 把 {len(res['crashed'])} 个实例标注为日志因整机崩溃中断（没有收尾不计为插件崩溃），见 A8 的说明；"
                "是否确属整机崩溃请人工确认。**", ""]  # fmt: skip
    if res["excluded_fail"]:
        out += [f"**被排除的段里本来就不通过的项（{'、'.join(res['excluded_fail'])}）照样记不通过：排除只拿得掉数据不全的段。**", ""]
    if res["toggles"]:
        out += ["现场按键次数（`--toggles`）：" + "、".join(f"{c} {n} 次" for c, n in res["toggles"].items()), ""]
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
        out += ["", "## 已排除的段", "", "这些段整段不进任何判据（文件名不写进输出，按实例序号和 C 行序号列）：", ""]
        for x in res["excluded"]:
            out.append(f"- 实例 {x['instance']}（{x['role']}）的第 {x['c_index']} 个 C 行（选择子 `{x['selector']}`），{x['cell']}")
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
    for k in ("bypass", "smart", "misplaced", "a1_stall", "replaced"):
        for m in ob[k]:
            m["cell"] = scrub(m["cell"])
    for x in res["excluded"]:
        x["selector"], x["cell"] = scrub(x["selector"]), scrub(x["cell"])
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
        toggle_args = take("--toggles", many=True)
        crashed_args = take("--crashed", many=True)
    except SystemExit:
        err.write("用法错误：选项缺少取值\n")
        return 2
    try:
        exclude = parse_excludes(exclude_args)
    except ValueError:
        err.write("用法错误：--exclude 应写成 <文件名>:<C 行序号或 t_ns>（序号与 t_ns 重名时写 #N 或 t=N）\n")
        return 2
    try:
        toggles = parse_toggles(toggle_args)
    except ValueError:
        err.write("用法错误：--toggles 应写成 <case>:<现场按键次数>，case 取 " + " 或 ".join(A2_CASES) + "，同一个 case 只写一次\n")
        return 2
    try:
        crashed = parse_crashed(crashed_args)
    except ValueError:
        err.write("用法错误：--crashed 应写成 <文件名>（日志目录里的 CSV 文件名，可重复）\n")
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
        err.write("用法：analyze-probe.py <日志目录> [--json] [--exclude <文件名>:<C 行序号或 t_ns>]... [--toggles <case>:<次数>]... [--crashed <文件名>]... | --self-test | --mutation-check | --spec\n")
        return 2
    salt = (salt_arg or secrets.token_hex(16)).encode()
    try:
        res = analyze(args[0], salt, exclude, toggles, crashed)
    except FormatError as e:
        err.write(scrub(f"日志格式错误：{e}") + "\n")
        return 2
    except ExcludeError as e:
        err.write(scrub(f"--exclude / --crashed 参数错误：{e}") + "\n")
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
    yield "seq 回退（相邻两个 B 行互换）", "\n".join(lines[:b] + [lines[b + 1], lines[b]] + lines[b + 2 :])
    yield "seq 回退（两个 B 行隔着事件行互换）", "\n".join(lines[:b] + [lines[b + 2], lines[e], lines[b + 1], lines[b]] + lines[b + 3 :])
    yield "seq 回退（call_seq 递增）", edit(b + 2, 1, "0")
    yield "call_seq 不递增（seq 递增）", edit(b + 1, 2, lines[b].split(",")[2])
    yield "call_seq 回退（seq 递增）", edit(b + 2, 2, "0")
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
    """补口：A2 的切换点与现场次数确认、CB-05 的停用证据要和块流中断对上、只数段内事件、短命克隆的 A8、--exclude 的归属与影子判定。
    夹具的判定已在夹具矩阵里查过，这里查说明文字、命令行与退出码。"""

    def run_json(d, *extra):
        code, so, se = _cli([os.path.join(tmp, d), "--json", "--hash-salt", "s", *extra])
        empty = {"items": [], "excluded": [], "excluded_fail": [], "warnings": [], "extras": [], "stop": False, "toggles": {}, "crashed": [], "f1": {"ok": False, "missing": []},
                 "observations": {"cells": [], "smart": [], "a1_stall": [], "replaced": []}}  # fmt: skip
        return code, (json.loads(so) if so.strip() else empty), se  # 参数错时没有 JSON：给个空壳，让断言失败而不是自测崩溃

    def said(res, iid):
        return " ".join(next((it["notes"] for it in res["items"] if it["id"] == iid), []))

    def verdict(res, iid):
        return next((it["verdict"] for it in res["items"] if it["id"] == iid), None)

    def a2(d, want=None):
        """→ (退出码, A2 结论, A2 说明)：want 是 --toggles CB-02:<次数>，None 表示不给。"""
        extra = [] if want is None else ["--toggles", f"CB-02:{want}"]
        code, r, _ = run_json(d, *extra)
        return code, verdict(r, "A2"), said(r, "A2")

    # A2：切换点 = 显式事件 ∪ 处理线程变化点（2 s 内并成一次）；块长抖动不算；推断出来的切换必须有现场次数确认才能通过
    _, v, txt = a2("a2-incomplete-thread-unconfirmed")
    check("A2 推断：不给现场次数时，推断的切换撑不出通过", v == "INCOMPLETE" and "没有给现场按键次数" in txt and "--toggles CB-02" in txt, True)
    _, v, txt = a2("a2-incomplete-thread-unconfirmed", 10)
    check("A2 推断：现场次数对得上时通过，文字写明按处理形态切换推断 10 次与相符", v == "PASS" and "按处理形态切换推断 10" in txt and "与现场记录的 10 次相符" in txt, False)
    for want, expect in ((9, "PASS"), (11, "PASS"), (8, "INCOMPLETE"), (12, "INCOMPLETE"), (0, "INCOMPLETE")):
        _, v, txt = a2("a2-incomplete-thread-unconfirmed", want)
        check(f"A2 推断：数出 10 次、现场记 {want} 次应为 {expect}", v == expect and (expect == "PASS" or "对不上" in txt), expect != "PASS")
    _, v, txt = a2("a2-pass", 20)
    check("A2 显式：显式事件数出 10 次、现场记 20 次时数据不全，显式事件也要对得上现场", v == "INCOMPLETE" and "对不上" in txt, True)
    _, v, txt = a2("a2-pass", 10)
    check("A2 显式：现场次数相符时通过并写明相符", v == "PASS" and "相符" in txt, False)
    _, v, txt = a2("a2-pass")
    check("A2 显式：不给现场次数时显式事件照样能通过（宿主亲口报的切换）", v == "PASS" and "显式事件 10、按处理形态切换推断 0" in txt, False)
    _, v, txt = a2("a2-incomplete-thread-pool3", 13)
    check("A2 推断：次数对得上但出现 3 个处理线程，更像线程池调度，数据不全", v == "INCOMPLETE" and "多于线程池预取与专用线程实时两种" in txt, True)
    _, v, txt = a2("a2-incomplete-thread-pool3", 10)
    check("A2 推断：线程池轮转的次数与现场次数对不上，数据不全", v == "INCOMPLETE" and "对不上" in txt, True)
    for d in ("a2-fail-thread-inferred", "a2-fail-thread-split-window", "a2-fail-thread-double-resync", "a2-fail-thread-presync", "a2-fail-mixed-explicit-and-inferred"):
        for want in (None, 10, 12):
            _, v, txt = a2(d, want)
            check(f"A2 推断：{d} 现场次数 {want} 时仍是不通过（重同步不能因次数对得上而被放过）", v == "FAIL" and "按处理形态切换推断" in txt, True)
    _, v, txt = a2("a2-fail-thread-split-window", 12)
    check("A2 推断：一次切换后的两次线程变化并成一次，窗口不被截断", v == "FAIL" and "并入那一次" in txt and "重同步最多 2 次" in txt, True)
    _, v, txt = a2("a2-pass-explicit-and-thread", 10)
    check("A2 推断：显式事件与线程变化同时出现时只数一遍", v == "PASS" and "显式事件 10、按处理形态切换推断 0" in txt, False)
    _, v, txt = a2("a2-pass-explicit-skewed-thread", 10)
    check("A2 推断：显式事件旁 2 块外的线程变化仍算同一次切换", v == "PASS" and "显式事件 10、按处理形态切换推断 0" in txt, False)
    _, v, txt = a2("a2-incomplete-thread-constant", 10)
    check("A2 推断：线程不变（只有块长抖动）时写明原因", v == "INCOMPLETE" and "处理形态没有变化" in txt and "ASIO-Guard 关着" in txt, True)
    _, v, txt = a2("a2-incomplete-one-cell-no-switch", 10)
    check("A2 推断：某一格一次切换都没有时单独说明，不被另一格的切换数盖住", v == "INCOMPLETE" and "buf=64" in txt and "处理形态没有变化" in txt, True)
    _, v, txt = a2("a2-incomplete-thread-few", 3)
    check("A2 推断：次数不足时分开写显式与推断的次数", v == "INCOMPLETE" and "按处理形态切换推断 3" in txt and f"不足 {MIN_TOGGLES}" in txt, True)
    _, v, txt = a2("a2-incomplete-thread-flap", 1)
    check("A2 推断：线程每 3 块换一次、全挤在几百毫秒里（不是用户在按），哪个线程都占不满迟滞，处理形态没有变化", v == "INCOMPLETE" and "处理形态没有变化" in txt, True)
    _, v, txt = a2("a2-incomplete-thread-tail-thin", 10)
    check("A2 推断：最后一次切换后块数不足时数据不全，不被前面的切换盖住", v == "INCOMPLETE" and "窗口里块数不足" in txt, True)
    # 命令行：--toggles 的结果、用法错误与全路径退出码
    code, r, _ = run_json("a2-pass", "--toggles", "CB-02:10")
    check("--toggles：JSON 里回显现场次数", r["toggles"] == {"CB-02": 10}, False)
    code, so, _ = _cli([os.path.join(tmp, "a2-pass"), "--hash-salt", "s", "--toggles", "CB-02:10"])
    check("--toggles：文本输出回显现场次数", "`--toggles`" in so and "CB-02 10 次" in so, False)
    code, r, _ = run_json("a2-pass", "--toggles", "LU-03:10")
    check("--toggles：给了日志里没有的 case 不改判定", verdict(r, "A2") == "PASS" and code in (0, 3), False)
    for label, extra in (
        ("缺冒号", ["--toggles", "CB-02"]),
        ("次数不是数字", ["--toggles", "CB-02:abc"]),
        ("不是 A2 判的 case", ["--toggles", "CB-03:5"]),
        ("同一个 case 写两次", ["--toggles", "CB-02:5", "--toggles", "CB-02:6"]),
        ("选项缺取值", ["--toggles"]),
    ):  # fmt: skip
        code, so, se = _cli([os.path.join(tmp, "a2-pass"), *extra])
        check(f"--toggles 用法错误：{label}，退出码 2", code == 2 and not so, True)
    full = os.path.join(tmp, "f1-a2-inferred")
    check("F1：A2 全靠推断的切换又没给现场次数时，全部判据过不了，退出码 3", _cli([full])[0] == 3, True)
    check("F1：A2 全靠推断的切换，现场次数对得上时可以冻结，退出码 0", _cli([full, "--toggles", "CB-02:10"])[0] == 0, False)
    check("F1：现场次数对不上时退出码 3", _cli([full, "--toggles", "CB-02:20"])[0] == 3, True)
    check("F1：线程池轮转即使次数凑得上也不能冻结", _cli([os.path.join(tmp, "f1-a2-pool3"), "--toggles", "CB-02:13"])[0] == 3, True)
    check("F1：显式事件的次数对不上现场时不能冻结", _cli([os.path.join(tmp, "full-pass"), "--toggles", "CB-02:20"])[0] == 3, True)
    check("F1：显式事件的次数对得上现场时可以冻结", _cli([os.path.join(tmp, "full-pass"), "--toggles", "CB-02:10"])[0] == 0, False)

    # CB-05：停用事件要落在块流中断里；set_processing(a=0)、reset、release 各自成立，写明是哪一种；段之前的事件、起播的 reset、析构的 release 不算
    def stop_cell(d, variant="deactivate"):
        cells = [c for c in run_json(d)[1]["observations"]["cells"] if c["variant"] == variant]
        return cells[0] if cells else {"state": None, "note": ""}

    for d, variant, word in (("cb05-deactivate-reset-only", "deactivate", "reset 1"), ("cb05-deactivate-release-only", "deactivate", "release 1"),
                             ("cb05-deactivate-set-processing-off", "deactivate", "set_processing(0) 1"), ("cb05-disable-track-reset-only", "disable-track", "reset 1"),
                             ("cb05-deactivate-reset-before-last-block", "deactivate", "reset 1")):  # fmt: skip
        c = stop_cell(d, variant)
        check(f"CB-05 停用证据：{d} 落在块流中断里算数，写明事件与中断时长", c["state"] == "PASS" and word in c["note"] and "块流中断 5005 ms" in c["note"], False)
    c = stop_cell("cb05-deactivate-single-end")
    check("CB-05 停用证据：一端整段没有块，看另一端的块流在事件前后都在", c["state"] == "PASS" and "另一端的块流在事件前后都在" in c["note"], False)
    for d in ("cb05-deactivate-no-event", "cb05-deactivate-pre-release", "cb05-deactivate-pre-reset", "cb05-deactivate-pre-set-processing", "cb05-deactivate-set-processing-on"):
        c = stop_cell(d)
        check(f"CB-05 停用证据：{d} 本段内没有停用事件，数据不全", c["state"] == "INCOMPLETE" and "本段内缺停用的生命周期事件" in c["note"], True)
    for d in ("cb05-deactivate-reset-at-play-only", "cb05-deactivate-teardown-release-only", "cb05-deactivate-short-gap", "cb05-deactivate-event-late-in-gap",
              "cb05-deactivate-single-end-at-start", "cb05-deactivate-single-end-at-end"):  # fmt: skip
        c = stop_cell(d)
        check(f"CB-05 停用证据：{d} 有事件但不在不短于 1 s 的块流中断里，不算停用", c["state"] == "INCOMPLETE" and "都不在不短于 1000 ms 的块流中断里" in c["note"], True)
    for d in ("f1-cb05-reset-at-play-only", "f1-cb05-teardown-release-only"):
        res = analyze(os.path.join(tmp, d), b"s")
        miss = res["f1"]["missing"]
        check(f"F1：{d} 的停用证据不算数，F1 点名 deactivate 与 disable-track", (not res["f1"]["ok"]) and "CB-05 缺 deactivate 格的本格证据" in miss and "CB-05 缺 disable-track 格的本格证据" in miss, True)
        check(f"F1：{d} 其余判据都过了也不能冻结，退出码 3", _cli([os.path.join(tmp, d)])[0] == 3, True)
    ob = run_json("cb06-smart-pre-ping")[1]["observations"]
    check("smart：段前的 ping 不算，格观察与本格证据都只数 1 条", ob["smart"][0]["pings"] == 1 and "本段内只有 1 条 ping" in ob["cells"][0]["note"], True)

    # A8：没有 C 行又活不到 2 s 的实例只记说明；说明不得声称查过它们的跳闸
    _, r, _ = run_json("a8-pass-short-clones")
    txt = said(r, "A8")
    check("A8：短命克隆只记说明，写明实例与时长", "实例 2（I，300 ms）" in txt and "实例 4（O，120 ms）" in txt, False)
    check("A8：说明写明没有摘要行、跳闸与卡死无从判断，不声称跳闸仍判不通过", "没有摘要行" in txt and "跳闸与卡死无从判断" in txt and "跳闸仍判不通过" not in txt and "没收尾的仍判不通过" in txt, False)
    check("extras：照常数出短命克隆", any("实例 2（I，300 ms）" in x and "2 个" in x for x in r["extras"]), False)
    check("A8：克隆没收尾仍是不通过", verdict(run_json("a8-fail-short-clone-crash")[1], "A8") == "FAIL", True)
    _, r, _ = run_json("a8-pass-short-clone-1900ms")
    check("A8：1.9 s 的克隆在短命界线内，只记说明", verdict(r, "A8") == "PASS" and "1900 ms" in said(r, "A8"), False)
    _, r, _ = run_json("a8-incomplete-long-no-c-2200ms")
    check("A8：2.2 s 的无 C 行实例过了界线，摘要行不足就是数据不全", verdict(r, "A8") == "INCOMPLETE" and "摘要行不足" in said(r, "A8"), True)

    # --exclude：归属、影子判定、只输出选择子
    mid, pids = "a1-incomplete-misrecorded-mid", {"mid": 5020, "unbound": 5021, "trailing": 5022, "gap": 5023, "tail": 5025}

    def both(pid, *sels):
        return [x for sel in sels for x in ("--exclude", f"probe-I-{pid}-e1f2.csv:{sel}", "--exclude", f"probe-O-{pid}-e3f4.csv:{sel}")]

    for tag, d, sel in (("mid", mid, 2), ("unbound", "a1-incomplete-misrecorded-unbound", 2), ("trailing", "a1-incomplete-misrecorded-trailing", 3), ("tail", "a1-incomplete-misrecorded-tail", 2),
                        ("mid", mid, "#2"), ("mid", mid, "t=1999999900"), ("mid", mid, 1999999900)):  # fmt: skip
        _, r, _ = run_json(d, *both(pids[tag], sel))
        ok = verdict(r, "A1") == "PASS" and [x["cell"] for x in r["excluded"]] == ["CB-01/steady/buf=-/guard=na"] * 2
        check(f"--exclude：{d} 排除 {sel} 后 A1 回到通过", ok, True)
    _, r, _ = run_json("a1-incomplete-misrecorded-trailing", *both(pids["trailing"], 3))
    check("--exclude：丢弃归给被排除的段时，提示里说明归属", any("归给被排除的段" in w and "40 个归给被排除的段" in w for w in r["warnings"]), False)
    _, r, _ = run_json(mid, "--exclude", f"probe-I-{pids['mid']}-e1f2.csv:2")
    check("--exclude：只排除 In 一份时，Out 一份仍留着，这一格还是数据不全", verdict(r, "A1") == "INCOMPLETE" and "只有 Out 一端" in said(r, "A1"), True)
    _, r, _ = run_json("a1-incomplete-gap-beside-excluded", *both(pids["gap"], 2))
    check("--exclude：邻段自己的缺口仍按段内连续性算，排除别的段盖不住它", verdict(r, "A1") == "INCOMPLETE" and "序号缺口" in said(r, "A1"), True)
    for d, pid, sels, want, word in (
        ("a1-incomplete-headdrop-after-excluded", 5041, (2,), "INCOMPLETE", "序号缺口"),
        ("a1-incomplete-headdrop-in-after-excluded", 5042, (2,), "INCOMPLETE", "序号缺口"),
        ("a1-incomplete-taildrop-after-unrelated-excluded", 5043, (1,), "INCOMPLETE", "序号缺口"),
        ("a1-incomplete-taildrop-after-unbound-excluded", 5044, (2,), "INCOMPLETE", "序号缺口"),
        ("a1-incomplete-two-excluded-taildrop", 5045, (1, 3), "INCOMPLETE", "序号缺口"),
        ("a1-incomplete-unlocated-drop", 5024, (1,), "INCOMPLETE", "序号缺口"),
        ("a1-incomplete-misrecorded-nosummary", 5047, (2,), "INCOMPLETE", "序号缺口"),
        ("a1-incomplete-two-excluded-ok", 5046, (1, 3), "PASS", ""),
    ):  # fmt: skip
        _, r, _ = run_json(d, *both(pid, *sels))
        check(f"--exclude：{d} 排除 {sels} 后 A1 为 {want}（丢块只归它自己的段，不被别的被排除段吞掉）", verdict(r, "A1") == want and word in said(r, "A1"), want != "PASS")
    _, r, _ = run_json("a1-incomplete-headdrop-after-excluded", *both(5041, 2))
    check("--exclude：被排除段后面一格的头部丢块，提示里写明归给未排除的段", any("0 个归给被排除的段" in w and "1 个归给未排除的段" in w for w in r["warnings"]), True)
    _, r, _ = run_json("a1-incomplete-taildrop-after-unrelated-excluded", *both(5043, 1))
    check("--exclude：尾部丢块不会因为前面有被排除的段而被吞", any("0 个归给被排除的段" in w and "3 个归给未排除的段" in w for w in r["warnings"]), True)
    _, r, _ = run_json("a1-incomplete-misrecorded-nosummary", *both(5047, 2))
    check("--exclude：没有摘要行时缺失的块序号无从归属，提示里写明，各个未排除的段都记一次", any("无从归属" in w and "没有摘要行可用" in w for w in r["warnings"]), True)
    code, r, _ = run_json("a1-fail-misrecorded-drift", *both(5040, 2))
    check("--exclude：被排除的段本来就不通过时，A1 照样不通过并停工，退出码 1", verdict(r, "A1") == "FAIL" and r["stop"] and r["excluded_fail"] == ["A1"] and code == 1, True)
    check("--exclude：影子判定的说明写明拿不掉不通过", "拿不掉不通过的段" in said(r, "A1") and "m≠0" in said(r, "A1"), True)
    for d, pid, iid in (("a4-fail-misrecorded-bign", 5048, "A4"), ("a7-fail-misrecorded-hostns", 5049, "A7")):
        code, r, _ = run_json(d, *both(pid, 2))
        check(f"--exclude：被排除的段里 {iid} 本来就不通过时照样不通过，不止 A1 一项", verdict(r, iid) == "FAIL" and r["excluded_fail"] == [iid] and code == 1, True)
    code, so, _ = _cli([os.path.join(tmp, "a1-fail-misrecorded-drift"), "--hash-salt", "s", *both(5040, 2)])
    check("--exclude：文本输出写明被排除的段里本来就不通过的项", "照样记不通过" in so and "A1" in so and code == 1, True)
    _, r, _ = run_json(mid)
    check("--exclude：不给参数时 excluded 与 excluded_fail 为空", r["excluded"] == [] and r["excluded_fail"] == [], False)
    args_md = [os.path.join(tmp, mid), "--hash-salt", "s", *both(pids["mid"], 2)]
    code, so, _ = _cli(args_md)
    check("--exclude：文本输出按实例序号与 C 行序号列出被排除的段，带选择子，不写文件名", "## 已排除的段" in so and "选择子 `2`" in so and "第 2 个 C 行" in so and "probe-" not in so and "e1f2" not in so, False)
    _, r, _ = run_json(mid, *both(pids["mid"], 2))
    check("--exclude：JSON 的 excluded 只有选择子、实例序号、角色、格与 C 行序号", all(set(x) == {"selector", "instance", "role", "cell", "c_index"} for x in r["excluded"]) and len(r["excluded"]) == 2, False)
    # 文件名可能带用户自己加的字：任何输出都不回显
    ren = os.path.join(tmp, "renamed-for-exclude")
    os.makedirs(ren)
    src = os.path.join(tmp, mid)
    a_name, b_name = f"probe-I-{pids['mid']}-e1f2-take2-quuxfile.csv", f"probe-O-{pids['mid']}-e3f4-take2-quuxfile.csv"
    shutil.copyfile(os.path.join(src, f"probe-I-{pids['mid']}-e1f2.csv"), os.path.join(ren, a_name))
    shutil.copyfile(os.path.join(src, f"probe-O-{pids['mid']}-e3f4.csv"), os.path.join(ren, b_name))
    pair = ["--exclude", f"{a_name}:2", "--exclude", f"{b_name}:2"]
    for fmt in ([], ["--json"]):
        code, so, se = _cli([ren, "--hash-salt", "s", *pair, *fmt])
        check(f"--exclude：文件名里的自加字不进输出（{'json' if fmt else 'md'}）", code == 3 and "quuxfile" not in so + se and "take2" not in so + se, True)
    code, so, se = _cli([ren, "--hash-salt", "s", "--exclude", f"{a_name}x:2"])
    check("--exclude：文件名写错时报错也不回显", code == EXIT_EXCLUDE and "quuxfile" not in so + se, True)
    for label, extra, want in (
        ("文件不存在", ["--exclude", "probe-I-1-zzzz.csv:2"], EXIT_EXCLUDE),
        ("C 行序号超出", ["--exclude", f"probe-I-{pids['mid']}-e1f2.csv:9"], EXIT_EXCLUDE),
        ("t_ns 对不上", ["--exclude", f"probe-I-{pids['mid']}-e1f2.csv:t=12345"], EXIT_EXCLUDE),
        ("同一个 C 行写两次（序号与 #N）", ["--exclude", f"probe-I-{pids['mid']}-e1f2.csv:2", "--exclude", f"probe-I-{pids['mid']}-e1f2.csv:#2"], EXIT_EXCLUDE),
        ("同一个 C 行写两次（原样重复）", ["--exclude", f"probe-I-{pids['mid']}-e1f2.csv:2", "--exclude", f"probe-I-{pids['mid']}-e1f2.csv:2"], EXIT_EXCLUDE),
        ("缺冒号", ["--exclude", "nocolon"], 2),
        ("选择子不是数字", ["--exclude", f"probe-I-{pids['mid']}-e1f2.csv:abc"], 2),
        ("选项缺取值", ["--exclude"], 2),
    ):  # fmt: skip
        code, so, se = _cli([os.path.join(tmp, mid), *extra])
        check(f"--exclude 参数错：{label}，退出码 {want}", code == want and not so and "zzzz" not in se, True)
    code, so, se = _cli([os.path.join(tmp, mid), "--exclude", f"probe-I-{pids['mid']}-e1f2.csv:2", "--exclude", f"probe-I-{pids['mid']}-e1f2.csv:#2"])
    check("--exclude：重复指向同一个 C 行时说明是第几个参数，不替用户去重", "同一个 C 行" in se and "第 2 个参数与第 1 个参数" in se, True)
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
    check("--exclude：#N 按序号、t=N 按 t_ns、裸数字先按 t_ns", resolved([2, 5, 9], "x.csv:#2", "x.csv:t=2", "x.csv:9") == [1, 0, 2], False)
    check("--exclude：两个 C 行的 t_ns 相同时，按 t_ns 选报错、按序号选可以", resolved([7, 7], "x.csv:t=7") is None and resolved([7, 7], "x.csv:#2") == [1], True)
    check("--exclude：同一个 C 行用序号和 t_ns 各写一遍报错", resolved([2, 5], "x.csv:#2", "x.csv:t=5") is None, True)
    check("--exclude：带目录的文件名只取最后一级", [p[0] for p in parse_excludes(["a/b/x.csv:3", "a\\b\\y.csv:4"])] == ["x.csv", "y.csv"], False)
    check("--exclude：parse_excludes 保留选择子原文，输出用它而不是文件名", [p[3] for p in parse_excludes(["x.csv:3", "y.csv:#4", "z.csv:t=5"])] == ["3", "#4", "t=5"], False)
    _self_test_r1(tmp, check, run_json, said, verdict, a2)


def _self_test_r1(tmp, check, run_json, said, verdict, a2):
    """第 1 轮整改的补口：被排除段的影子判定与归属提示、A2 的窗口取法与处理形态、A1 的 Out 端起播停顿、
    A7 的 call_seq 对齐与 toggle 格的 hostNs 回退、A8 的整机崩溃标注、CB-05 禁用轨道的实例替换。
    夹具的判定已在夹具矩阵里查过，这里查说明文字、命令行与退出码。"""

    def both(pid, *sels):
        return [x for sel in sels for x in ("--exclude", f"probe-I-{pid}-e1f2.csv:{sel}", "--exclude", f"probe-O-{pid}-e3f4.csv:{sel}")]

    # 被排除段的影子判定：缺口归属要同时记到被排除的段上，主会话里不记一个都不会进判据
    code, r, _ = run_json("a1-incomplete-misrecorded-headdrop-drift", *both(5050, 2))
    check("--exclude 影子：被排除的一格头部丢块（缺口归给它）又有 drift，影子判定只能算数据不全，A1 不被改成不通过、不停工",
          verdict(r, "A1") == "PASS" and r["excluded_fail"] == [] and not r["stop"] and code == 3, True)  # fmt: skip
    code, r, _ = run_json("a1-incomplete-misrecorded-nosummary-drift", *both(5054, 2))
    check("--exclude 影子：一条摘要都没有时缺口无从归属，被排除的一格同样记缺口，影子判定只能算数据不全，A1 不被改成不通过、不停工",
          verdict(r, "A1") == "INCOMPLETE" and r["excluded_fail"] == [] and not r["stop"] and code == 3, True)  # fmt: skip
    code, r, _ = run_json("a1-fail-misrecorded-drift-main-fail", *both(5051, 2))
    check("--exclude 影子：未排除的格已经让 A1 不通过、被排除的格本来也不通过时，两件事都记：excluded_fail 含 A1，停工",
          verdict(r, "A1") == "FAIL" and r["excluded_fail"] == ["A1"] and r["stop"] and code == 1, True)  # fmt: skip
    check("--exclude 影子：主会话这一项已经不通过时不改写它的说明", "被排除的段若计入" not in said(r, "A1") and "停顿之后" not in said(r, "A1"), True)
    _, r, _ = run_json("a1-incomplete-misrecorded-unbound", *both(5021, 2))
    check("--exclude 提示：丢弃全部归给被排除的段时，改写成已全部归给被排除的段，不再说相关格按数据不全处理",
          any("已全部归给被排除的段" in w for w in r["warnings"]) and not any("相关格按数据不全处理" in w for w in r["warnings"]), True)  # fmt: skip
    _, r, _ = run_json("a1-incomplete-taildrop-after-unrelated-excluded", *both(5043, 1))
    check("--exclude 提示：丢弃有一部分归给未排除的段时，仍说相关格按数据不全处理", any("相关格按数据不全处理" in w for w in r["warnings"]), True)

    # A2 的窗口：显式事件与推断的切换同样从切换之前的最后一块算起；窗口按 mono_ns 排序后用二分取
    _, v, txt = a2("a2-fail-explicit-presync", 10)
    check("A2 窗口：显式切换事件之前最后一块上的重同步算进这一次切换", v == "FAIL" and "重同步最多 2 次" in txt, True)
    _, v, txt = a2("a2-pass-unsorted-timestamps", 10)
    check("A2 窗口：文件里按 seq 排的块与 mono_ns 不同序时，窗口照样按 mono_ns 取", v == "PASS" and "显式事件 10" in txt, False)
    # 数时间戳之间的比较次数：排序一遍加每个切换点的二分，总共约为块数的 2.7 倍；为每个切换点扫整段会多出约 10 倍
    cmps = [0]

    class Counted(int):
        def __lt__(self, other):
            cmps[0] += 1
            return int.__lt__(self, other)

        def __gt__(self, other):
            cmps[0] += 1
            return int.__gt__(self, other)

        def __le__(self, other):
            cmps[0] += 1
            return int.__le__(self, other)

        def __ge__(self, other):
            cmps[0] += 1
            return int.__ge__(self, other)

    logs, compares, broken = load_dir(os.path.join(tmp, "a2-pass"))
    nblocks = sum(len(lg.blocks) for lg in logs)
    for lg in logs:
        for b in lg.blocks:
            b["mono_ns"] = Counted(b["mono_ns"])
    cmps[0] = 0
    judge_a2(Session(logs, compares, broken))
    check("A2 窗口：10 个切换点的窗口用二分取，不为每个切换点扫整段（时间戳之间的比较次数不超过块数的 5 倍）", 0 < cmps[0] <= 5 * nblocks, False)

    # F2：按处理形态数切换
    _, v, txt = a2("a2-incomplete-regimes-unconfirmed", 12)
    check("A2 形态：线程池预取与专用线程实时来回 13 段，数出 12 次切换，其中 12 次伴随块长变化，与现场 12 次相符",
          v == "PASS" and "按处理形态切换推断 12" in txt and "其中 12 次伴随块长变化" in txt and "处理形态 2 种" in txt and "与现场记录的 12 次相符" in txt, False)  # fmt: skip
    _, v, txt = a2("a2-incomplete-regimes-unconfirmed")
    check("A2 形态：不给现场次数时，推断出的 12 次撑不出通过", v == "INCOMPLETE" and "没有给现场按键次数" in txt, True)
    _, v, txt = a2("a2-incomplete-regimes-unconfirmed", 10)
    check("A2 形态：现场记 10 次、数出 12 次时对不上", v == "INCOMPLETE" and "对不上" in txt, True)
    _, v, txt = a2("a2-pass-regimes-explicit")
    check("A2 形态：显式事件与形态变化落在同一处时只数一遍", v == "PASS" and "显式事件 12、按处理形态切换推断 0" in txt and "其中 12 次伴随块长变化" in txt, False)
    _, v, txt = a2("a2-incomplete-pool-rotation-only", 12)
    check("A2 形态：线程池内部逐块轮换线程不算切换，没有形态变化时写明原因", v == "INCOMPLETE" and "处理形态没有变化" in txt, True)
    _, v, txt = a2("a2-incomplete-regime-blip-short", 1)
    check("A2 形态：专用线程上只有 0.4 s 的另一线程（短于迟滞）不算形态变化", v == "INCOMPLETE" and "处理形态没有变化" in txt, True)
    _, v, txt = a2("a2-incomplete-regime-blip-long", 1)
    check("A2 形态：另一线程占满迟滞就是一种形态，去和回两次变化挤在 2 s 内并成一次", v == "INCOMPLETE" and "只有 1 次切换" in txt and "并入了那一次" in txt, True)
    _, v, txt = a2("a2-incomplete-regimes-three-kinds", 12)
    check("A2 形态：专用线程、两线程轮换、线程池三种形态，次数对得上也多于两种", v == "INCOMPLETE" and "出现了 3 种不同的处理形态" in txt, True)
    _, v, txt = a2("a2-incomplete-thread-unconfirmed", 10)
    check("A2 形态：两个专用线程之间 A→B 来回算切换，块长没变就写 0 次伴随块长变化", v == "PASS" and "处理形态 2 种" in txt and "其中 0 次伴随块长变化" in txt, False)

    # F1：A1 的 Out 端起播停顿
    _, r, _ = run_json("a1-pass-out-stall")
    st = r["observations"]["a1_stall"]
    check("A1 停顿：Out 端头 10 块时间线不走，off 之后恒定，通过；格观察报停顿 53 ms（10 块）", verdict(r, "A1") == "PASS" and len(st) == 1 and (st[0]["stall_ms"], st[0]["stall_blocks"]) == (53, 10), False)
    check("A1 停顿：说明写明上限与来源，In 端不放宽", "允许上限 120 ms（现场最长 56 ms 的约 2 倍）" in said(r, "A1") and "In 端不放宽" in said(r, "A1"), False)
    code, so, _ = _cli([os.path.join(tmp, "a1-pass-out-stall"), "--hash-salt", "s"])
    check("A1 停顿：文本输出的格观察里逐格列出最长停顿与上限", "A1 Out 端起播时间线停顿" in so and "最长 53 ms（10 块）" in so and "上限 120 ms" in so, False)
    _, r, _ = run_json("a1-pass-out-stall-jump-only")
    check("A1 停顿：只有一次部分跳变也算停顿，通过", verdict(r, "A1") == "PASS" and [x["stall_blocks"] for x in r["observations"]["a1_stall"]] == [1], False)
    _, r, _ = run_json("a1-pass-out-two-runs")
    check("A1 停顿：停播后再起播是第二段播放，各段停顿后落在同一个 off，通过", verdict(r, "A1") == "PASS" and [x["runs"] for x in r["observations"]["a1_stall"]] == [2], False)
    for d, word in (
        ("a1-fail-out-stall-too-long", "起播停顿 159 ms（30 块）超过上限 120 ms"),
        ("a1-fail-out-off-keeps-moving", "超过上限 120 ms"),
        ("a1-fail-out-off-shrinks", "停顿里 off 回退或时间线后退"),
        ("a1-fail-out-timeline-backward", "停顿里 off 回退或时间线后退"),
        ("a1-fail-out-two-runs-different-off", "停顿之后 off 取值 2 种"),
    ):  # fmt: skip
        _, r, _ = run_json(d)
        check(f"A1 停顿：{d} 不通过并写明原因", verdict(r, "A1") == "FAIL" and word in said(r, "A1") and r["stop"], True)
    _, r, _ = run_json("a1-fail-in-stall")
    check("A1 停顿：In 端同样的停顿形态不放宽，不通过", verdict(r, "A1") == "FAIL" and " I m≠0" in said(r, "A1") and "停顿之后" not in said(r, "A1"), True)

    # F3：A7 按 call_seq 对齐
    _, r, _ = run_json("a7-pass-out-opens-first")
    txt = said(r, "A7")
    check("A7 对齐：Out 先开窗多出 5 块，按 Out = In +1 对齐配对 40 对，通过，方向写 In 先于 Out", verdict(r, "A7") == "PASS" and "Out = In +1，In 先于 Out" in txt and "配对 40，两端不等 0" in txt, False)
    _, r, _ = run_json("a7-pass-out-first-reported")
    check("A7 对齐：Out 回调排在 In 之前时照实报「Out 先于 In（错放）」，hostNs 对得上仍通过", verdict(r, "A7") == "PASS" and "Out = In -1，Out 先于 In（错放）" in said(r, "A7"), False)
    _, r, _ = run_json("a7-incomplete-callseq")
    check("A7 对齐：两端 call_seq 差远超上限时数据不全，不硬配", verdict(r, "A7") == "INCOMPLETE" and "call_seq 对不上" in said(r, "A7") and "+1001" in said(r, "A7"), True)
    _, r, _ = run_json("obs-abnormal")
    check("A7 对齐：只有 30/40 对的 call_seq 差一致（众数占比不足）时数据不全", verdict(r, "A7") == "INCOMPLETE" and "只占 30/40" in said(r, "A7"), True)

    # F4：toggle 格切换点前后的 hostNs 回退（推断的切换点要 A2 确认过才给容忍：见 _self_test_r2；显式事件不受此限）
    for d, extra in (("a7-fail-toggle-hostns-regress-unconfirmed", ["--toggles", "CB-02:10"]), ("a7-pass-toggle-hostns-regress-explicit", [])):
        _, r, _ = run_json(d, *extra)
        txt = said(r, "A7")
        check(f"A7 回退：{d} 回退（最大 7100 ms）落在切换点前后 500 ms 内，容忍并计数，通过",
              verdict(r, "A7") == "PASS" and "非递增 0" in txt and "另有 2 次落在切换点前后 500 ms 内，已容忍，最大回退 7100 ms" in txt, False)  # fmt: skip
    _, r, _ = run_json("a7-fail-toggle-hostns-regress-away")
    check("A7 回退：回退离切换点 1.6 s，窗口外照常算非递增，不通过", verdict(r, "A7") == "FAIL" and "非递增 2" in said(r, "A7") and "已容忍" not in said(r, "A7"), True)
    _, r, _ = run_json("a7-fail-steady-hostns-regress-at-event")
    check("A7 回退：非 toggle 格里多余的监听切换事件不开容忍窗口，回退照常算非递增", verdict(r, "A7") == "FAIL" and "非递增 2" in said(r, "A7") and "已容忍" not in said(r, "A7"), True)

    # F5：--crashed
    iname, oname = "probe-I-4100-a1b2.csv", "probe-O-4100-c3d4.csv"
    _, r, _ = run_json("a8-fail")
    check("A8 标注：没有 --crashed 时，没有收尾的实例仍是不通过", verdict(r, "A8") == "FAIL" and r["crashed"] == [], True)
    _, r, _ = run_json("a8-fail", "--crashed", oname)
    txt = said(r, "A8")
    check("A8 标注：标注了的实例没有收尾，不计为插件崩溃，A8 通过并写 NOTE（写明实例与「请人工确认」）",
          verdict(r, "A8") == "PASS" and "1 个实例按人工标注（--crashed）" in txt and "实例 2（O）" in txt and "请人工确认" in txt, False)  # fmt: skip
    check("A8 标注：JSON 只回显实例序号与角色，不含文件名", r["crashed"] == [{"instance": 2, "role": "O"}] and "c3d4" not in json.dumps(r), False)
    code, so, se = _cli([os.path.join(tmp, "a8-fail"), "--hash-salt", "s", "--crashed", oname])
    check("A8 标注：文本输出在表格前醒目提示「是否确属整机崩溃请人工确认」，不回显文件名",
          "`--crashed`" in so and "请人工确认" in so and so.index("请人工确认") < so.index("| 项 |") and "c3d4" not in so + se and "probe-O" not in so + se, False)  # fmt: skip
    for d, pid, word in (("a8-fail-crashed-stall", 5100, "摘要间隔超限"), ("a8-fail-crashed-trips", 5101, "跳闸 2 次")):
        _, r, _ = run_json(d, "--crashed", f"probe-O-{pid}-c3d4.csv")
        check(f"A8 标注：{d} 标注了没有收尾，摘要间隔与跳闸仍照常判，不通过", verdict(r, "A8") == "FAIL" and word in said(r, "A8"), True)
    for label, extra in (
        ("标注了有收尾行的实例", ["--crashed", iname]),
        ("文件不存在", ["--crashed", "probe-O-1-zzzz.csv"]),
        ("同一个文件写两次", ["--crashed", oname, "--crashed", oname]),
    ):  # fmt: skip
        code, so, se = _cli([os.path.join(tmp, "a8-fail"), *extra])
        check(f"A8 标注参数错：{label}，退出码 {EXIT_EXCLUDE} 且不回显文件名", code == EXIT_EXCLUDE and not so and "zzzz" not in se and "a1b2" not in se and "c3d4" not in se, True)
    code, so, se = _cli([os.path.join(tmp, "a8-fail"), "--crashed"])
    check("A8 标注参数错：选项缺取值，退出码 2", code == 2 and not so, True)
    for label, val in (("文件名为空", ""), ("只有目录没有文件名", "some/dir/")):
        code, so, se = _cli([os.path.join(tmp, "a8-fail"), "--crashed", val])
        check(f"A8 标注参数错：{label}，退出码 2", code == 2 and not so and "some" not in se, True)
    ren = os.path.join(tmp, "renamed-for-crashed")
    os.makedirs(ren)
    shutil.copyfile(os.path.join(tmp, "a8-fail", iname), os.path.join(ren, iname))
    nm = "probe-O-4100-c3d4-take2-quuxfile.csv"
    shutil.copyfile(os.path.join(tmp, "a8-fail", oname), os.path.join(ren, nm))
    for fmt in ([], ["--json"]):
        code, so, se = _cli([ren, "--hash-salt", "s", "--crashed", nm, *fmt])
        check(f"A8 标注：文件名里的自加字不进输出（{'json' if fmt else 'md'}）", "quuxfile" not in so + se and "take2" not in so + se and code == 3, True)
    code, so, se = _cli([ren, "--hash-salt", "s", "--crashed", nm + "x"])
    check("A8 标注：文件名写错时报错也不回显", code == EXIT_EXCLUDE and "quuxfile" not in so + se, True)

    # F6：CB-05 禁用轨道，旧实例析构、同 pid 新实例 set_state 并配对
    def dt_cell(d, variant="disable-track"):
        code, r, _ = run_json(d)
        cells = [c for c in r["observations"]["cells"] if c["variant"] == variant]
        return (cells[0] if cells else {"state": None, "note": ""}), r

    c, r = dt_cell("cb05-disable-track-replaced")
    check("CB-05 替换：旧实例 destruct 后 7500 ms 同 pid 新实例 set_state、7800 ms 配对，算停用证据，写明耗时",
          c["state"] == "PASS" and "实例被替换（Disable Track 销毁实例）" in c["note"] and "destruct 后 7500 ms 同 pid 新实例 set_state、7800 ms 重新配对（已恢复配对）" in c["note"], False)  # fmt: skip
    rep = r["observations"]["replaced"]
    check("CB-05 替换：格观察里分端报替换耗时与是否恢复配对", sorted((x["role"], x["set_state_ms"], x["paired_ms"]) for x in rep) == [("I", 7500, 7800), ("O", 7500, 7800)], False)
    for d, word in (
        ("cb05-disable-track-replaced-unpaired", "但在 15 s 内没有重新配对"),
        ("cb05-disable-track-replaced-timeout", "15 s 内没有同 pid 同角色的新实例 set_state"),
        ("cb05-disable-track-replaced-in-only", "Out 端旧实例 destruct 后 15 s 内没有同 pid 同角色的新实例 set_state"),
        ("cb05-disable-track-replaced-other-pid", "15 s 内没有同 pid 同角色的新实例 set_state"),
        ("cb05-disable-track-replaced-born-earlier", "15 s 内没有同 pid 同角色的新实例 set_state"),
        ("cb05-disable-track-destruct-only", "15 s 内没有同 pid 同角色的新实例 set_state"),
    ):  # fmt: skip
        c, _ = dt_cell(d)
        check(f"CB-05 替换：{d} 不算停用证据，数据不全并写明新实例没有按时恢复",
              c["state"] == "INCOMPLETE" and "新实例没有按时恢复" in c["note"] and word in c["note"], True)  # fmt: skip
    c, r = dt_cell("cb05-deactivate-replaced-ignored", "deactivate")
    check("CB-05 替换：只对禁用轨道认实例替换，停用格里同样的析构加新实例不算证据", c["state"] == "INCOMPLETE" and "实例被替换" not in c["note"] and r["observations"]["replaced"] == [], True)
    check("F1：禁用轨道这一格的证据是实例替换时，其余判据都过，F1 满足，退出码 0", _cli([os.path.join(tmp, "f1-cb05-disable-track-replaced")])[0] == 0, False)
    _self_test_r2(tmp, check, run_json, said, verdict, a2)


def _self_test_r2(tmp, check, run_json, said, verdict, a2):
    """第 2 轮整改：A7 的容忍窗口只认经 A2 确认的推断切换点；A2 的切换时刻取新形态第一块；
    影子判定不把会话级判据（A3、A8）的失败记到被排除的段名下。夹具的判定已在夹具矩阵里查过，这里查说明文字与现场次数的配合。"""

    # 推断的切换点要 A2 确认过（现场次数对上、形态数合规）才给 A7 的容忍窗口
    d = "a7-fail-toggle-hostns-regress-unconfirmed"
    _, r, _ = run_json(d)
    txt = said(r, "A7")
    check("A7 确认：推断的切换点没给 --toggles 时不给容忍，回退算非递增，不通过；说明写明有几次本会落在没经确认的点前后、怎样才算确认",
          verdict(r, "A7") == "FAIL" and "非递增 2" in txt and "已容忍" not in txt and "非递增里有 2 次落在没经确认的推断切换点前后 500 ms 内，没有容忍" in txt
          and "10 个按处理形态推断的切换点没有经 A2 确认，不给容忍" in txt and "--toggles CB-02:<次数>" in txt, True)  # fmt: skip
    _, r, _ = run_json(d, "--toggles", "CB-02:10")
    txt = said(r, "A7")
    check("A7 确认：给了 --toggles CB-02:10、A2 数出 10 次对上，推断的切换点给容忍，通过；说明写明已由 A2 确认",
          verdict(r, "A2") == "PASS" and verdict(r, "A7") == "PASS" and "非递增 0" in txt and "另有 2 次落在切换点前后 500 ms 内，已容忍，最大回退 7100 ms" in txt
          and "切换点含 10 个按处理形态推断的，已由 A2 按现场次数（--toggles CB-02:10）确认" in txt, False)  # fmt: skip
    _, r, _ = run_json(d, "--toggles", "CB-02:10", "--toggles", "LU-03:99")
    check("A7 确认：别的 case 的现场次数不影响这个 case 的确认", verdict(r, "A7") == "PASS", False)
    _, r, _ = run_json(d, "--toggles", "CB-02:5")
    check("A7 确认：现场次数 5 与数出的 10 次对不上，A2 数据不全，A7 也不给容忍", verdict(r, "A2") == "INCOMPLETE" and verdict(r, "A7") == "FAIL" and "没有经 A2 确认" in said(r, "A7"), True)
    _, r, _ = run_json("a7-fail-toggle-hostns-regress-three-regimes", "--toggles", "CB-02:12")
    check("A7 确认：次数对得上、但出现 3 种处理形态，A2 不确认，A7 也不给容忍",
          verdict(r, "A2") == "INCOMPLETE" and "出现了 3 种不同的处理形态" in said(r, "A2") and verdict(r, "A7") == "FAIL" and "没有经 A2 确认" in said(r, "A7"), True)  # fmt: skip
    _, r, _ = run_json("a7-pass-toggle-hostns-regress-explicit", "--toggles", "CB-02:5")
    check("A7 确认：显式事件是宿主亲口报的，不受 A2 确认的约束：现场次数对不上、A2 数据不全时，A7 仍容忍显式切换点处的回退，说明里没有推断点的话",
          verdict(r, "A2") == "INCOMPLETE" and verdict(r, "A7") == "PASS" and "已容忍" in said(r, "A7") and "推断" not in said(r, "A7"), False)  # fmt: skip
    _, r, _ = run_json("a7-fail-toggle-hostns-regress-away", "--toggles", "CB-02:10")
    check("A7 确认：窗口外的回退即使推断的切换点已确认也照常算非递增", verdict(r, "A7") == "FAIL" and "非递增 2" in said(r, "A7") and "没经确认" not in said(r, "A7"), True)

    # A2 的切换时刻：新形态第一块，窗口从它之前的一块算起；夹在两个专用线程形态之间的杂散段不当切换点
    d = "a2-incomplete-stray-between-unconfirmed"
    _, v, txt = a2(d)
    check("A2 切换时刻：夹着杂散段的两个专用线程形态，不给现场次数时推断的 10 次撑不出通过", v == "INCOMPLETE" and "没有给现场按键次数" in txt, True)
    _, v, txt = a2(d, 10)
    check("A2 切换时刻：杂散段第一块上的重同步不算进这次切换（切换时刻取新形态第一块，窗口从杂散段末块算起），每次切换后最多 1 次，通过",
          v == "PASS" and "按处理形态切换推断 10" in txt and "处理形态 2 种" in txt, False)  # fmt: skip
    ps = _synth()
    logs, compares, broken = load_dir(os.path.join(tmp, d))
    seg = next(g for g in Session(logs, compares, broken).segments if g.role == "I")
    got = switch_points(seg)["inferred"]
    want = [(ps.T_START + s * ps.LONG, ps.T_START + (s - 1) * ps.LONG) for s in ps.switch_blocks(10)]
    check("A2 切换时刻：switch_points 给出的是 (新形态首块的 mono_ns, 它之前一块的 mono_ns)，不是杂散段的第一块", got == want, False)
    check("A2 切换时刻：夹着的杂散段不是形态，不数成两次切换", len(got) == 10 and switch_points(seg)["regimes"] == 2, False)

    # 影子判定只算由被排除的段本身导致的失败：A3（导出比对行）与 A8（实例级的收尾、摘要、跳闸）读的是整场的数据，不记到被排除的段名下
    d = "exclude-clean-while-others-fail"
    both = ["--exclude", "probe-I-5120-e1f2.csv:2", "--exclude", "probe-O-5120-e3f4.csv:2"]
    code, r, _ = run_json(d, *both)
    check("--exclude 影子：被排除的一格干净，别的实例让 A3 A5 A8 A9 各自不通过，这些失败不记到被排除的段名下：excluded_fail 为空",
          [verdict(r, i) for i in ("A3", "A5", "A8", "A9")] == ["FAIL"] * 4 and r["excluded_fail"] == [] and len(r["excluded"]) == 2, True)  # fmt: skip
    check("--exclude 影子：A8 的不通过说明是别的实例的跳闸，没有被改写成「被排除的段若计入」", "跳闸 2 次" in said(r, "A8") and "被排除的段若计入" not in said(r, "A8"), True)
    code, so, _ = _cli([os.path.join(tmp, d), "--hash-salt", "s", *both])
    check("--exclude 影子：文本输出不说「被排除的段里本来就不通过」", "照样记不通过" not in so and "已排除的段" in so, True)
    _self_test_r3(tmp, check, run_json, said, verdict)
    _self_test_r4(tmp, check, run_json, said, verdict)


def _self_test_r3(tmp, check, run_json, said, verdict):
    """第 3 轮整改：
    1. --exclude 的影子会话里，A7 对推断切换点的确认、A2 对现场次数的对账都是整个 case 的性质，由主会话算一次，影子沿用；
       不再拿只含被排除的段的计数去对整场的 --toggles（那样被排除的 toggle 格必然对不上，hostNs 回退被说成「被排除的段本来就不通过」）；
    2. --crashed 认整机崩溃留下的残缺末行（没有换行结尾，parse_log 丢掉并记字节数，其余照常解析），文件头都没写完的与中间损坏的仍不认。
    夹具的判定已在夹具矩阵里查过，这里查带 --exclude / --crashed 的组合、说明文字与退出码。"""
    ps = _synth()

    def tree(name, files):
        _write_tree(os.path.join(tmp, name), files)
        return name

    # ---- 1. 被排除的 toggle 格 ----
    d = "a7-exclude-toggle-beside-main"  # 同键两遍：pid 5150 留下（10 次切换、没有回退），pid 5151 误记（3 次切换，第 2 次切换处 hostNs 回退 7.3 s）
    ex = ["--exclude", "probe-I-5151-a1b2.csv:#1", "--exclude", "probe-O-5151-c3d4.csv:#1"]
    code, r, _ = run_json(d, *ex, "--toggles", "CB-02:10")
    check("--exclude 影子：被排除的 toggle 格有 hostNs 回退，主会话的 --toggles 对得上：A7 沿用主会话的确认，excluded_fail 为空，A2 A7 都通过",
          verdict(r, "A2") == "PASS" and verdict(r, "A7") == "PASS" and r["excluded_fail"] == [] and len(r["excluded"]) == 2 and "被排除的段若计入" not in said(r, "A7"), False)  # fmt: skip
    code, so, _ = _cli([os.path.join(tmp, d), "--hash-salt", "s", *ex, "--toggles", "CB-02:10"])
    check("--exclude 影子：文本输出不说被排除的 toggle 格「本来就不通过」，退出码不是 1", "照样记不通过" not in so and "已排除的段" in so and code != 1, False)
    for label, extra in (("现场次数与主会话数出的 10 次对不上", ["--toggles", "CB-02:5"]), ("没给现场次数", [])):
        code, r, _ = run_json(d, *ex, *extra)
        check(f"--exclude 影子：{label}，主会话不确认，被排除的段里推断点前后的回退没有容忍，照第 2 轮的口径记 A7 不通过",
              verdict(r, "A2") == "INCOMPLETE" and verdict(r, "A7") == "FAIL" and r["excluded_fail"] == ["A7"] and "被排除的段若计入" in said(r, "A7") and code == 1, True)  # fmt: skip
    # 沿用确认不等于放过真问题：被排除的 toggle 格里回退离切换点 1.6 s，窗口外照常算非递增
    away = tree("a7-exclude-toggle-away", {**ps.scen_a2_thread("pass", pid=5150), **ps.scen_a7_toggle_regress(5151, "away", switches=3, at=1)})
    _, r, _ = run_json(away, *ex, "--toggles", "CB-02:10")
    check("--exclude 影子：推断点已确认也不放过窗口外的回退：被排除的段里 A7 本来就不通过，记进 excluded_fail",
          verdict(r, "A7") == "FAIL" and r["excluded_fail"] == ["A7"] and "非递增 2" in said(r, "A7"), True)  # fmt: skip
    # A2 在影子里不再对账整场的现场次数：被排除的格 A2 本来就不通过（切换块上重同步 2 次）时，说明里只有这一件事，不夹带「只有 3 次切换」「对不上」
    dbl = tree("a2-exclude-toggle-double-resync", ps.toggle_excluded_beside_main(5150, double_resync=True))
    _, r, _ = run_json(dbl, *ex, "--toggles", "CB-02:10")
    check("--exclude 影子：被排除的 toggle 格 A2 本来就不通过，记进 excluded_fail；A7 不受牵连",
          verdict(r, "A2") == "FAIL" and r["excluded_fail"] == ["A2"] and verdict(r, "A7") == "PASS" and "每次切换后重同步最多 2 次" in said(r, "A2"), True)  # fmt: skip
    check("--exclude 影子：A2 不拿被排除的段自己的 3 次去对整场的现场次数，说明里没有「只有 3 次切换」「对不上」", "只有 3 次切换" not in said(r, "A2") and "对不上" not in said(r, "A2"), False)
    # 整个 case 的 toggle 格都被排除了：主会话没有可沿用的结论，--toggles 说的只可能是这些段里的按键，影子才用自己的计数对账
    whole = "a7-fail-toggle-hostns-regress-unconfirmed"
    both = ["--exclude", "probe-I-5090-a1b2.csv:#1", "--exclude", "probe-O-5090-c3d4.csv:#1"]
    _, r, _ = run_json(whole, *both, "--toggles", "CB-02:10")
    check("--exclude 影子：整个 case 的 toggle 格都被排除，--toggles 对上被排除的段自己数出的 10 次：推断点确认，A7 不被记不通过，excluded_fail 为空",
          verdict(r, "A7") == "INCOMPLETE" and r["excluded_fail"] == [] and len(r["excluded"]) == 2, False)  # fmt: skip
    for label, extra in (("现场次数对不上", ["--toggles", "CB-02:5"]), ("没给现场次数", [])):
        _, r, _ = run_json(whole, *both, *extra)
        check(f"--exclude 影子：整个 case 的 toggle 格都被排除、{label}：不确认，被排除的段里的回退没有容忍，记 A7 不通过", verdict(r, "A7") == "FAIL" and r["excluded_fail"] == ["A7"], True)
    # 留下的 toggle 格里一次切换都没数到（线程一直不变），主会话对这个 case 没有话要说：同样没有结论可沿用，影子用被排除的段自己的计数
    zero = tree("a7-exclude-toggle-main-without-switch", {**ps.scen_a2_thread("incomplete", pid=5150), **ps.scen_a7_toggle_regress(5151, "at-switch")})
    _, r, _ = run_json(zero, *ex, "--toggles", "CB-02:10")
    check("--exclude 影子：留下的 toggle 格一次切换都没数到，--toggles 对上被排除的段自己数出的 10 次：推断点确认，excluded_fail 为空，A7 不被记不通过",
          verdict(r, "A2") == "INCOMPLETE" and r["excluded_fail"] == [] and len(r["excluded"]) == 2 and verdict(r, "A7") == "PASS", False)  # fmt: skip

    # ---- 2. 整机崩溃留下的残缺末行 ----
    iname, oname = "probe-I-5200-a1b2.csv", "probe-O-5200-c3d4.csv"
    base = ps.files_of(*ps.pair("CB-11", "constrain", 256, "low", pid=5200))
    no_z = "\n".join(ln for ln in base[oname].split("\n") if not ln.startswith("Z,"))  # Out 端没有收尾行，末尾仍有换行
    # parse_log：没有换行结尾的末行丢掉，记下字节数；整份文件仍可解析，不进 broken
    check("末行残缺：完整的文件没有残行", parse_log(base[oname].encode(), "日志#1").partial_bytes == 0 and not parse_log(base[oname].encode(), "日志#1").truncated, False)
    for suffix, label in (("S,300", "写到一半的 S 行"), ("S,300" + "\0" * 10, "写到一半的 S 行后接 10 个零字节（崩溃后文件长度已更新、数据还是零）"), ("\0" * 64, "只有一段零字节")):
        lg = parse_log((no_z + suffix).encode("ascii"), "日志#1")
        check(f"末行残缺：{label}，末行丢掉并记下字节数 {len(suffix)}，其余照常解析", lg.truncated and lg.partial_bytes == len(suffix) and lg.closing is None and len(lg.blocks) == 40, False)
    # 夹具：三种残缺末行（S 行写到一半 / B 行写到一半 / 整行写完只缺换行）
    for shape, pid in (("summary", 5102), ("block", 5103), ("nonl", 5104)):
        d = f"a8-fail-partial-tail-{shape}"
        name = f"probe-O-{pid}-c3d4.csv"
        with open(os.path.join(tmp, d, name), "rb") as f:
            n = len(f.read().decode("ascii").rsplit("\n", 1)[1])
        _, r, _ = run_json(d)
        check(f"--crashed 残缺末行：{shape} 不带 --crashed 照旧：A8 日志没有收尾，不通过；文件不进 broken", verdict(r, "A8") == "FAIL" and "日志没有收尾" in said(r, "A8") and r["inputs"]["broken"] == 0 and r["crashed"] == [], True)  # fmt: skip
        code, r, _ = run_json(d, "--crashed", name)
        txt = said(r, "A8")
        check(f"--crashed 残缺末行：{shape} 标注后被接受，A8 通过并写 NOTE，写明丢弃了 {n} 字节残行",
              verdict(r, "A8") == "PASS" and r["crashed"] == [{"instance": 2, "role": "O"}] and f"末行没有换行结尾，已丢弃 {n} 字节残行" in txt and "请人工确认" in txt and code != 5, False)  # fmt: skip
        code, so, se = _cli([os.path.join(tmp, d), "--hash-salt", "s", "--crashed", name])
        check(f"--crashed 残缺末行：{shape} 文本输出醒目提示请人工确认，不回显文件名与残行内容", "请人工确认" in so and name not in so + se and "c3d4" not in so + se, False)
    nulls = tree("a8-partial-tail-zero-fill", {iname: base[iname], oname: no_z + "S,300" + "\0" * 10})
    _, r, _ = run_json(nulls, "--crashed", oname)
    check("--crashed 残缺末行：崩溃后补零的末尾（15 字节）同样被接受，写明丢弃 15 字节", verdict(r, "A8") == "PASS" and "已丢弃 15 字节残行" in said(r, "A8"), False)
    # 只允许「末行没有换行结尾」这一种缺陷：其余位置的损坏仍是格式错（退出码 2），--crashed 不放宽
    lines = no_z.split("\n")
    k = [n for n, ln in enumerate(lines) if ln.startswith("B,")][10]
    mid = tree("a8-partial-tail-middle-damaged", {iname: base[iname], oname: "\n".join(lines[:k] + [",".join(lines[k].split(",")[:-3])] + lines[k + 1 :]) + "S,300"})
    last = tree("a8-last-line-short-with-newline", {iname: base[iname], oname: no_z + "S,300\n"})
    for d, label in ((mid, "中间的一行字段残缺（末行也残缺）"), (last, "末行有换行结尾却字段残缺（不是写到一半，是内容损坏）")):
        for extra in ([], ["--crashed", oname]):
            code, so, se = _cli([os.path.join(tmp, d), "--hash-salt", "s", *extra])
            check(f"--crashed 不放宽：{label}，{'带' if extra else '不带'} --crashed 都是格式错，退出码 2，没有分析输出", code == 2 and not so and "S,300" not in se and "c3d4" not in se, True)
    # 文件头都没写完的进 broken（没有角色、没有 pid，什么都分析不了），--crashed 不认，A8 照样不通过
    hdr = tree("a8-header-cut", {iname: base[iname], oname: "rinne-probe-log,1\nH,O,vst3,cu"})
    _, r, _ = run_json(hdr)
    check("文件头写到一半：进 broken，A8 不通过", r["inputs"]["broken"] == 1 and verdict(r, "A8") == "FAIL" and "只有不完整的内容" in said(r, "A8"), True)
    for d, nm, label in ((hdr, oname, "文件头写到一半"), ("a8-fail-broken-file", "probe-I-9999-dead.csv", "0 字节")):
        code, so, se = _cli([os.path.join(tmp, d), "--hash-salt", "s", "--crashed", nm])
        check(f"--crashed 不认文件头都没写完的文件：{label}，退出码 {EXIT_EXCLUDE}，说明里讲清缘由且不回显文件名",
              code == EXIT_EXCLUDE and not so and "文件头写完之前就被截断了" in se and nm not in se, True)  # fmt: skip


def _self_test_r4(tmp, check, run_json, said, verdict):
    """第 4 轮整改：
    1. --exclude 的影子沿用主会话的切换次数对账之前，先用被排除的段自己的 toggle_counts 核处理形态数：形态数是逐段的性质，
       主会话（留下的格）形态不多于 2 种，不能替被排除的格也有 3 种形态背书；
    2. 缺失的块序号所有缺口都计入 lost_total（尾部丢块 = 探针报告的丢弃数 − 缺口总数）：早于第一个 C 行的孤块之间（与第一个块是孤块时它之前）
       的缺口不归任何格，只在提示里报个数，不会被当成尾部丢块记给最后一格；
    3. B 行的 seq / call_seq 按文件顺序严格递增是格式约束（parse_log 拒收，退出码 2），文件顺序就是 seq 顺序；交错的是别的行（C、E、S）。
    夹具的判定已在夹具矩阵里查过，这里查带 --exclude 的组合、说明文字与退出码。"""
    ps = _synth()

    def tree(name, files):
        _write_tree(os.path.join(tmp, name), files)
        return name

    def both(pid, *sels):
        return [x for sel in sels for x in ("--exclude", f"probe-I-{pid}-e1f2.csv:{sel}", "--exclude", f"probe-O-{pid}-e3f4.csv:{sel}")]

    # ---- 1. 影子沿用确认之前核被排除的段自己的形态数 ----
    ex = ["--exclude", "probe-I-5151-a1b2.csv:#1", "--exclude", "probe-O-5151-c3d4.csv:#1"]
    d3 = "a7-exclude-toggle-three-regimes"  # 留下的格 10 次切换、2 种形态；被排除的格 3 次切换、3 种形态，第 2 次切换处 hostNs 回退 7.3 s
    code, r, _ = run_json(d3, *ex, "--toggles", "CB-02:10")
    txt = said(r, "A7")
    check("--exclude 影子：被排除的 toggle 格有 3 种处理形态（更像线程池调度）、推断点处 hostNs 回退，主会话的确认不替它背书：A7 不通过，excluded_fail 为 [A7]",
          verdict(r, "A2") == "PASS" and verdict(r, "A7") == "FAIL" and r["excluded_fail"] == ["A7"] and code == 1 and len(r["excluded"]) == 2, True)  # fmt: skip
    check("--exclude 影子：说明写明被排除的段若计入不通过、回退没有容忍、怎样才算确认",
          "被排除的段若计入" in txt and "非递增 2" in txt and "没有经 A2 确认" in txt and f"处理形态不多于 {A2_MAX_REGIMES} 种" in txt, True)  # fmt: skip
    check("--exclude 影子：A2 在主会话里照旧通过（留下的格对得上现场次数），形态数的核查不改写它", "与现场记录的 10 次相符" in said(r, "A2") and "被排除的段若计入" not in said(r, "A2"), False)
    code, so, _ = _cli([os.path.join(tmp, d3), "--hash-salt", "s", *ex, "--toggles", "CB-02:10"])
    check("--exclude 影子：文本输出写明被排除的段里本来就不通过的项，退出码 1", "照样记不通过" in so and "A7" in so and code == 1, True)
    # 对照：被排除的格形态数不多于 2 种时照旧沿用主会话的确认（第 3 轮的夹具）
    code, r, _ = run_json("a7-exclude-toggle-beside-main", *ex, "--toggles", "CB-02:10")
    check("--exclude 影子对照：被排除的格只有 2 种处理形态，照旧沿用主会话的确认，excluded_fail 为空", verdict(r, "A7") == "PASS" and r["excluded_fail"] == [] and code != 1, False)
    # 形态数只影响推断点的容忍：被排除的格 3 种形态、但 hostNs 没有回退，没有东西要容忍，A7 在影子里通过；A2 的「形态多于 2 种」只是数据不全，排除合法
    quiet = tree("a7-exclude-toggle-three-regimes-no-regress", ps.toggle_excluded_beside_main(5150, regimes=3, where="none"))
    code, r, _ = run_json(quiet, *ex, "--toggles", "CB-02:10")
    check("--exclude 影子：3 种形态但 hostNs 没有回退：没有东西要容忍，A7 通过，excluded_fail 为空（形态多于 2 种只是数据不全，排除合法）",
          verdict(r, "A2") == "PASS" and verdict(r, "A7") == "PASS" and r["excluded_fail"] == [] and code != 1, False)  # fmt: skip
    # shadow_reviews 本身：同样的主会话结论，被排除的段形态数不同，沿用与否不同
    for name, want in (("a7-exclude-toggle-beside-main", "NOTE"), (d3, "INCOMPLETE")):
        logs, compares, broken = load_dir(os.path.join(tmp, name))
        s = Session(logs, compares, broken, resolve_excludes(logs, parse_excludes(["probe-I-5151-a1b2.csv:#1", "probe-O-5151-c3d4.csv:#1"])))
        reviews = toggle_reviews(s, {"CB-02": 10})
        sh = s.shadow()
        got = shadow_reviews(reviews, sh, {"CB-02": 10}).get("CB-02") or (None,)  # 判据被改坏时这些键可能不在：让断言失败，不让自测崩溃
        kinds = toggle_counts(sh).get("CB-02", {}).get("regimes")
        check(f"shadow_reviews：主会话（留下的格）的确认是 NOTE；{name} 被排除的格 {kinds} 种形态，影子里该 case 的结论应为 {want}",
              (reviews.get("CB-02") or (None,))[0] == "NOTE" and got[0] == want, want != "NOTE")  # fmt: skip

    # ---- 2. 所有缺口都计入已缺总数，孤块之间的缺口不归任何格 ----
    code, r, _ = run_json("a1-incomplete-orphan-gaps-before-excluded", *both(5160, 2))
    check("--exclude 孤块之间的缺口：开窗前的孤块之间缺了块，不归任何格，也不是尾部丢块：排除误记的一格后最后一格的 A1 通过",
          verdict(r, "A1") == "PASS" and r["excluded_fail"] == [] and len(r["excluded"]) == 2, True)  # fmt: skip
    check("--exclude 孤块之间的缺口：提示里单独报个数（两个实例各 3 个块序号、2 处），不说相关格按数据不全，也没有归给未排除的段",
          any("缺 6 个块序号、4 处" in w and "不归任何格" in w for w in r["warnings"]) and not any("相关格按数据不全处理" in w or "归给未排除的段" in w for w in r["warnings"]), True)  # fmt: skip
    _, r, _ = run_json("a1-incomplete-orphan-gaps-and-taildrop", *both(5161, 2))
    check("--exclude 孤块之间的缺口：最后一格自己另有 3 块尾部丢块：只有这 3 块归给它（A1 数据不全），孤块之间的 3 块不再被多算成尾部丢块",
          verdict(r, "A1") == "INCOMPLETE" and "序号缺口" in said(r, "A1")
          and sum("0 个归给被排除的段" in w and "3 个归给未排除的段" in w for w in r["warnings"]) == 2 and not any("6 个归给未排除的段" in w for w in r["warnings"]), True)  # fmt: skip
    _, r, _ = run_json("a1-incomplete-orphan-head-before-excluded", *both(5162, 2))
    check("--exclude 孤块之间的缺口：第一个块就是孤块、它之前缺的块更早，同样不归第一格：A1 通过，提示里报个数（两个实例各 2 个块序号、1 处）",
          verdict(r, "A1") == "PASS" and any("缺 4 个块序号、2 处" in w for w in r["warnings"]), True)  # fmt: skip
    # 没有 --exclude：探针报告的丢弃全在孤块之间，已经定位，各格不按数据不全算（旧口径是「定位不到缺口」而整个实例都记缺口）
    code, r, _ = run_json("a1-pass-orphan-gaps-only")
    check("孤块之间的缺口：不带 --exclude，丢弃全在孤块之间，已经定位：A1 通过，提示不说相关格按数据不全处理",
          verdict(r, "A1") == "PASS" and any("缺 6 个块序号、4 处" in w for w in r["warnings"]) and not any("相关格按数据不全处理" in w for w in r["warnings"]), True)  # fmt: skip
    check("孤块之间的缺口：孤块本身仍挡 F1", not r["f1"]["ok"] and any("不属于任何测试格" in m for m in r["f1"]["missing"]), True)
    # 探针报告的丢弃比孤块之间的缺口多：多出来的部分定位不到，整个实例仍按数据不全
    plus = {n: t.replace("S,3000000000,85,3", "S,3000000000,85,5").replace("Z,3100000000,85,3", "Z,3100000000,85,5")
            for n, t in ps.files_of(*ps.multi_cells([("CB-01", "steady", 64, "off", 40, "ok"), ("CB-01", "steady", 1024, "high", 40, "ok")], pid=5163, pre="ooxxooxo")).items()}  # fmt: skip
    _, r, _ = run_json(tree("a1-incomplete-orphan-gaps-plus-unlocated", plus))
    check("孤块之间的缺口：探针报告的丢弃多于孤块之间的缺口（多出的 2 个定位不到）：整个实例仍按数据不全",
          verdict(r, "A1") == "INCOMPLETE" and any("相关格按数据不全处理" in w for w in r["warnings"]), True)  # fmt: skip
    # Session 层面：缺口按谁的格记
    logs, compares, broken = load_dir(os.path.join(tmp, "a1-pass-orphan-gaps-only"))
    s = Session(logs, compares, broken)
    check("孤块之间的缺口：Session 记下孤块之间缺 6 个块序号、4 处，任何格都没有序号缺口",
          (s.orphan_lost, s.orphan_gaps, s.orphans) == (6, 4, 10) and all(g.gaps == 0 for g in s.segments), False)  # fmt: skip
    # 时间戳倒着走的文件：孤块紧跟在某格的块之后、中间缺了块，缺口记给前一块所在的格（不丢，也不记给别的格）
    li, lo = ps.multi_cells([("CB-01", "steady", 64, "off", 40, "ok"), ("CB-01", "steady", 256, "low", 40, "ok"), ("CB-01", "steady", 1024, "high", 40, "ok")], pid=5164)
    bs = [k for k, ln in enumerate(li.lines) if ln.startswith("B,")]
    f = li.lines[bs[10]].split(",")
    f[11] = str(ps.T_START - 50_000)  # mono_ns 早于第一个 C 行：孤块
    li.lines[bs[10]] = ",".join(f)
    del li.lines[bs[9]]  # 第 9 块丢掉：缺口在第一格的第 8 块与孤块之间
    back = tree("a1-incomplete-orphan-after-block", ps.files_of(li, lo))
    logs, compares, broken = load_dir(os.path.join(tmp, back))
    ins = [g for g in Session(logs, compares, broken).segments if g.role == "I"]
    check("孤块之间的缺口：孤块紧跟在第一格的块之后、中间缺了块（时间戳倒着走）：缺口记给前一块所在的第一格", [g.gaps for g in ins] == [1, 0, 0], True)

    # ---- 3. B 行 seq / call_seq 按文件顺序严格递增是格式约束，文件顺序就是 seq 顺序 ----
    base = ps.files_of(*ps.pair("CB-01", "steady", 256, "low", pid=5170))
    iname, oname = "probe-I-5170-a1b2.csv", "probe-O-5170-c3d4.csv"
    ilines = base[iname].split("\n")
    bs = [k for k, ln in enumerate(ilines) if ln.startswith("B,")]
    blocks = parse_log(base[iname].encode(), "日志#1").blocks
    check("B 行：合法文件的 seq 与 call_seq 按文件顺序严格递增", all(a["seq"] < b["seq"] and a["call_seq"] < b["call_seq"] for a, b in zip(blocks, blocks[1:])), False)
    cs = ilines[bs[4]].split(",")
    cs[2] = ilines[bs[3]].split(",")[2]  # call_seq 与前一块相同，seq 递增
    for tag, label, text in (
        ("swap", "seq 回退（相邻两个 B 行互换）", "\n".join(ilines[: bs[3]] + [ilines[bs[4]], ilines[bs[3]]] + ilines[bs[4] + 1 :])),
        ("cseq", "call_seq 不递增（seq 递增）", "\n".join(ilines[: bs[4]] + [",".join(cs)] + ilines[bs[4] + 1 :])),
    ):  # fmt: skip
        d = tree("b-order-" + tag, {"probe-I-5170-a1b2-quuxfile.csv": text, oname: base[oname]})
        code, so, se = _cli([os.path.join(tmp, d), "--hash-salt", "s"])
        check(f"B 行：{label}：格式错，退出码 2，没有分析输出；消息只带文件序号与行号，不回显文件名", code == 2 and not so and "日志#" in se and "严格递增" in se and "quux" not in se, True)
    # 交错的是别的行：事件行、摘要行放在文件里与时间戳不一致的位置，B 行本身仍按 seq 排；分析结果与原文件相同
    moved = list(ilines)
    ev = next(k for k, ln in enumerate(moved) if ln.startswith("E,"))
    sm = next(k for k, ln in enumerate(moved) if ln.startswith("S,"))
    e_line, s_line = moved[ev], moved[sm]
    moved = [ln for k, ln in enumerate(moved) if k not in (ev, sm)]
    at = [k for k, ln in enumerate(moved) if ln.startswith("B,")][20]
    moved[at:at] = [e_line, s_line]  # 早于第一个块的事件与摘要，写在第 20 个块的位置
    lg = parse_log("\n".join(moved).encode(), "日志#1")
    seqs = [b["seq"] for b in lg.blocks]
    check("B 行：事件行与摘要行夹在 B 行中间、与时间戳顺序不一致：照常解析，B 行的 seq 仍按文件顺序严格递增，事件行与摘要行都在",
          seqs == sorted(set(seqs)) and len(seqs) == 40 and len(lg.events) >= 1 and len(lg.summaries) >= 1, False)  # fmt: skip
    _, r1, _ = run_json(tree("b-order-interleaved", {iname: "\n".join(moved), oname: base[oname]}))
    _, r2, _ = run_json(tree("b-order-plain", base))
    check("B 行：行交错不改变分析结果（判据逐项相同，没有序号缺口）",
          [(i["id"], i["verdict"]) for i in r1["items"]] == [(i["id"], i["verdict"]) for i in r2["items"]] and verdict(r1, "A1") == "PASS", False)  # fmt: skip


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
            JUDGES[k] = (iid, lambda s, *a, _i=iid: {**stub(), "id": _i, "name": ITEM_NAMES[_i]})  # A2 还会收到 toggles
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
    for const, loose in (("RESIDUAL_LIMIT_DB", 0.0), ("MIN_TOGGLES", 0), ("HOSTNS_MISMATCH_RATIO", 1.0), ("MIN_SLICE_BLOCKS", 0), ("MIN_STEADY_BLOCKS", 0), ("SHORT_LIVED_NS", 0),
                         ("A2_TOGGLE_TOLERANCE", 99), ("A2_MAX_REGIMES", 99), ("DEACT_GAP_NS", 0),
                         ("A1_STALL_MAX_NS", 10**12), ("A2_REGIME_MIN_NS", 0), ("A2_POOL_MIN_THREADS", 1), ("A7_TOGGLE_ZONE_NS", 10**12),
                         ("A7_SHIFT_MAX", 10**9), ("A7_SHIFT_SHARE_MIN", 0.0), ("REPLACE_WINDOW_NS", 10**15)):  # fmt: skip
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
