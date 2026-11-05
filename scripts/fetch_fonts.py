# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/web.json
"""fetch_fonts.py：生成 web/fonts/ 下的离线子集 WOFF2。

只有本脚本联网；运行期的 web/ 一律使用本地 @font-face，全仓禁止在线字体服务的域名引用。

四款字体、五个文件，源文件都取自 google/fonts 仓的同一个钉死的提交，并校验 sha256：
  - Space Grotesk：可变字体，固定到 wght=600（标题与大数字只用这一档），输出 SpaceGrotesk.woff2；
  - Figtree：可变字体，分别固定到 wght=500 与 wght=600，改名为 Rinne Sans，输出 RinneSans.woff2（正文）与
    RinneSansSemiBold.woff2（强调）；
  - DM Mono：静态 Medium，改名为 Rinne Mono，输出 RinneMono.woff2；
  - Noto Sans SC：可变字体，字重轴收窄到 300 到 700，输出 NotoSansSC.woff2。
Figtree 与 DM Mono 都没有保留字体名，改名不是许可要求，是为了让 CSS 族名 Rinne Sans / Rinne Mono 不随上游字体变：
name 表的族名记录与文件名都写 Rinne Sans / Rinne Mono，别的 name 记录里残留的上游族名一并清掉；
版权与许可记录（nameID 0、7、13、14）原样保留。

字符集宽覆盖，让页面卡不必因为缺字回头改字体：
  1. 扫描 web/ 下 js、html、css 去掉注释后的全部非 ASCII 字符（上屏文本的真实来源）；
  2. web/fonts/charset-handoff.txt：设计交接包里三语词条用到的全部字符；
  3. GB 2312 一级汉字（3755 字），覆盖通用规范汉字一级 3500 字的绝大部分常用字；
  4. ASCII 可打印字符、Latin-1 补充、拉丁扩展 A；
  5. 兜底字符：常用标点与单位符号、箭头、数学符号、中日韩标点、全角形式。
拉丁四个文件拿到全部非汉字字符，Noto Sans SC 拿到全集，作为三条字体栈共同的最后一道内嵌回退。

用法：
    python scripts/fetch_fonts.py [输出目录] [--source-dir 目录]
    python scripts/fetch_fonts.py --print-charset     只扫描并打印字符集，不联网、不写文件
    python scripts/fetch_fonts.py --help
--source-dir 指向已下载好的源文件目录（文件名同下面的 SOURCES），仍然校验 sha256。
需要 fontTools 与 brotli；--print-charset 与 --help 不需要。
"""

import hashlib
import io
import os
import re
import sys
import unicodedata
import urllib.request

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
WEB = os.path.join(ROOT, "web")
HANDOFF_CHARSET = os.path.join(WEB, "fonts", "charset-handoff.txt")

# ---- 上游源文件 -------------------------------------------------------------
UPSTREAM_COMMIT = "bd8f81ddb5c74d5c8897b36ad88b440266245103"
UPSTREAM_BASE = "https://raw.githubusercontent.com/google/fonts/%s/ofl/" % UPSTREAM_COMMIT
# 本地文件名 -> (仓内路径, sha256)
SOURCES = {
    "Figtree.ttf": ("figtree/Figtree%5Bwght%5D.ttf", "26ad3db9b31ff7dde67a91ff515d022d2f495cd506590699cf264f0bfe6fb714"),
    "DMMono-Medium.ttf": ("dmmono/DMMono-Medium.ttf", "fd327daf461db87b44a87def475d251bf03b997f7c07d9680592d75dbbfaad0b"),
    "SpaceGrotesk.ttf": ("spacegrotesk/SpaceGrotesk%5Bwght%5D.ttf", "acad6de1fc93436f5c0f1f4137751ef04f1aea3063e7036535970ffcfbd79f72"),
    "NotoSansSC.ttf": ("notosanssc/NotoSansSC%5Bwght%5D.ttf", "a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da"),
}

# ---- 输出表（输出文件名的真源；两个检查脚本从这里派生）--------------------------
# (输出文件名, 源文件, 轴设置, 命名 (族名, 样式名, PostScript 名, 上游族名或 None, 字重) 或 None, 字符集 "latin" 或 "all")
# 轴设置：数字表示固定到该值，(低, 高) 表示收窄范围，None 表示静态字体。
# 命名为 None 的字体保持上游的 name 表不动。上游族名不为 None 表示改了名：族名记录换成登记的名字，
# 别的 name 记录里残留的上游族名清掉（见 rename_font）；为 None 表示沿用上游族名，只规范名字记录。
# 样式名与字重按实际内嵌的字重写（Medium 500、SemiBold 600），不写成 Regular。
OUTPUTS = [
    ("SpaceGrotesk.woff2", "SpaceGrotesk.ttf", {"wght": 600}, ("Space Grotesk", "SemiBold", "SpaceGrotesk-SemiBold", None, 600), "latin"),
    ("RinneSans.woff2", "Figtree.ttf", {"wght": 500}, ("Rinne Sans", "Medium", "RinneSans-Medium", "Figtree", 500), "latin"),
    ("RinneSansSemiBold.woff2", "Figtree.ttf", {"wght": 600}, ("Rinne Sans", "SemiBold", "RinneSans-SemiBold", "Figtree", 600), "latin"),
    ("RinneMono.woff2", "DMMono-Medium.ttf", None, ("Rinne Mono", "Medium", "RinneMono-Medium", "DM Mono", 500), "latin"),
    ("NotoSansSC.woff2", "NotoSansSC.ttf", {"wght": (300, 700)}, ("Noto Sans SC", "Light", "NotoSansSC-Light", None, 300), "all"),
]
LATIN_OUTPUTS = [o[0] for o in OUTPUTS if o[4] == "latin"]
CJK_OUTPUT = [o[0] for o in OUTPUTS if o[4] == "all"][0]

# OFL-1.1 要求每份拷贝都带版权声明与许可证：上游子集的 name 表里可能缺 13，改名时一并补齐
OFL_LICENSE_DESC = (
    "This Font Software is licensed under the SIL Open Font License, Version 1.1. "
    "This license is available with a FAQ at https://scripts.sil.org/OFL"
)
# 改名时重写的 name 记录：族名、唯一 ID、全名、PostScript 名、排版族名与子族名
NAME_IDS_FAMILY = (1, 3, 4, 6, 16, 17)
# 署名记录：0 版权、7 商标、13 许可证声明、14 许可证网址。永不改写，随分发逐字保留；
# 它们本身就会写着上游名字（版权行里的项目网址、商标声明、"with Reserved Font Name"），所以不参与残留名字复核。
NAME_IDS_ATTRIBUTION = (0, 7, 13, 14)

# ---- 兜底字符集 -------------------------------------------------------------
ASCII_PRINTABLE = "".join(chr(c) for c in range(0x20, 0x7F))
LATIN1 = "".join(chr(c) for c in range(0x00A0, 0x0100))
LATIN_EXT_A = "".join(chr(c) for c in range(0x0100, 0x0180))
# 常用标点与单位符号、箭头、数学符号：括号里是它们的用途
SYMBOLS = (
    "\u2010\u2011\u2012\u2013\u2014\u2015"  # 连字符与破折号
    "\u2018\u2019\u201a\u201c\u201d\u201e"  # 弯引号
    "\u2020\u2021\u2022\u2026\u2030\u2032\u2033"  # 刀号、项目符、省略号、千分号、角分秒
    "\u202f\u2009\u200a"  # 窄不换行空格与细空格
    "\u20ac\u2122\u2116"  # 欧元、商标、编号
    "\u2190\u2191\u2192\u2193\u2195"  # 箭头
    "\u2212\u00d7\u00f7\u00b1\u2260\u2264\u2265\u2248\u221e\u00b0"  # 数学符号与度
)
# 中日韩标点与全角形式
CJK_PUNCT = "".join(chr(c) for c in range(0x3000, 0x3040)) + "".join(chr(c) for c in range(0xFF01, 0xFF5F)) + "\u00b7\u2027\u30fb"
FALLBACK = ASCII_PRINTABLE + LATIN1 + LATIN_EXT_A + SYMBOLS + CJK_PUNCT


def usable(c):
    """可以进字符集的字符：排除控制符、代理项、私用区和未分配码位。"""
    return unicodedata.category(c) not in ("Cc", "Cs", "Co", "Cn")


def is_cjk(ch):
    """只能由 Noto Sans SC 提供字形的字符：部首、康熙部首、中日韩标点、假名、统一表意文字、兼容表意文字、
    竖排标点、全角形式、扩展 B 以上。区间取宽：宁可多算进汉字子集，也不要漏字导致运行期方块。"""
    o = ord(ch)
    return (
        0x2E80 <= o <= 0x9FFF
        or 0xF900 <= o <= 0xFAFF
        or 0xFE30 <= o <= 0xFE4F
        or 0xFF00 <= o <= 0xFFEF
        or 0x20000 <= o <= 0x3FFFF
    )


def gb2312_level1():
    """GB 2312 一级汉字（区 16 到 55，3755 字）。"""
    out = []
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            if hi == 0xD7 and lo > 0xF9:
                continue
            try:
                out.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return "".join(out)


# ---- 扫描 web/ ---------------------------------------------------------------
REGEX_PREV = set("(,=:[!&|?{};+-*%~^<>\n")
REGEX_WORDS = {"return", "typeof", "instanceof", "in", "of", "new", "delete", "void", "case", "do", "else", "yield", "await"}


def strip_js_comments(src):
    """去掉 JS 注释，保留字符串、模板串和正则字面量。"""
    out = []
    i, n = 0, len(src)
    prev, word = "\n", ""
    while i < n:
        c, d = src[i], src[i + 1] if i + 1 < n else ""
        if c == "/" and d == "/":
            e = src.find("\n", i)
            i = n if e < 0 else e
        elif c == "/" and d == "*":
            e = src.find("*/", i + 2)
            i = n if e < 0 else e + 2
        elif c in "\"'`":
            k = i + 1
            while k < n and src[k] != c and not (c != "`" and src[k] == "\n"):
                k += 2 if src[k] == "\\" else 1
            out.append(src[i : k + 1])
            i, prev, word = k + 1, c, ""
        elif c == "/" and (prev in REGEX_PREV or word in REGEX_WORDS):
            k, in_class = i + 1, False
            while k < n and src[k] != "\n":
                if src[k] == "\\":
                    k += 2
                    continue
                if src[k] == "[":
                    in_class = True
                elif src[k] == "]":
                    in_class = False
                elif src[k] == "/" and not in_class:
                    break
                k += 1
            out.append(src[i : k + 1])
            i, prev, word = k + 1, "/", ""
        else:
            out.append(c)
            if c == "\n":
                prev, word = "\n", ""
            elif not c.isspace():
                prev = c
                word = word + c if (c.isalnum() or c == "_") else ""
            i += 1
    return "".join(out)


def unescape_js(s):
    """还原 \\uXXXX、\\u{...}、\\xXX 转义：转义形式的字符也要进字符集。"""
    s = re.sub(r"\\u\{([0-9a-fA-F]+)\}", lambda m: chr(int(m.group(1), 16)), s)
    s = re.sub(r"\\u([0-9a-fA-F]{4})", lambda m: chr(int(m.group(1), 16)), s)
    return re.sub(r"\\x([0-9a-fA-F]{2})", lambda m: chr(int(m.group(1), 16)), s)


def strip_html(src):
    src = re.sub(r"<!--.*?-->", " ", src, flags=re.S)

    def script(m):
        return m.group(1) + strip_js_comments(m.group(2)) + m.group(3)

    src = re.sub(r"(<script\b[^>]*>)(.*?)(</script>)", script, src, flags=re.S | re.I)
    return re.sub(r"(<style\b[^>]*>)(.*?)(</style>)", lambda m: m.group(1) + re.sub(r"/\*.*?\*/", " ", m.group(2), flags=re.S) + m.group(3), src, flags=re.S | re.I)


def scan_web():
    """返回 (字符集合, 明细)。扫 web/ 下 js、mjs、html、css 和 web-preview/ 下 html 去掉注释后的非 ASCII 字符。"""
    chars, detail = set(), []
    roots = [(WEB, (".js", ".mjs", ".html", ".css")), (os.path.join(ROOT, "web-preview"), (".html",))]
    for base, exts in roots:
        for dirpath, dirs, files in os.walk(base):
            rel = os.path.relpath(dirpath, ROOT).replace("\\", "/")
            dirs[:] = sorted(d for d in dirs if "%s/%s" % (rel, d) not in ("web/js", "web/fonts"))
            for fn in sorted(files):
                if not fn.endswith(exts):
                    continue
                p = os.path.join(dirpath, fn)
                with open(p, "r", encoding="utf-8") as f:
                    text = f.read()
                if fn.endswith((".js", ".mjs")):
                    text = strip_js_comments(text)
                elif fn.endswith(".html"):
                    text = strip_html(text)
                else:
                    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
                found = {c for c in unescape_js(text) if ord(c) > 0x7E and usable(c)}
                chars |= found
                detail.append((os.path.relpath(p, ROOT).replace("\\", "/"), len(found)))
    return chars, detail


def read_handoff():
    if not os.path.isfile(HANDOFF_CHARSET):
        return set()
    with open(HANDOFF_CHARSET, "r", encoding="utf-8") as f:
        return {c for c in f.read() if usable(c) and not c.isspace()}


def required_chars():
    """界面一定要用到的字符：web/ 里实际出现的非 ASCII 字符，加设计交接包词条用到的字符。
    返回 (集合, 来源明细)。缺任何一个都是上屏方块，由 check-font-coverage.py 判红。"""
    scanned, detail = scan_web()
    return scanned | read_handoff(), detail


def build_charsets():
    """返回 (拉丁子集字符串, 全集字符串, 明细)。两串都按码位排序，保证重跑结果稳定。"""
    required, detail = required_chars()
    base = {c for c in (set(FALLBACK) | set(gb2312_level1()) | required) if usable(c)}
    latin = "".join(sorted(c for c in base if not is_cjk(c)))
    everything = "".join(sorted(base))
    return latin, everything, detail


# ---- 取源文件 ---------------------------------------------------------------
def sha256(b):
    return hashlib.sha256(b).hexdigest()


def load_source(name, source_dir):
    path, want = SOURCES[name]
    if source_dir:
        with open(os.path.join(source_dir, name), "rb") as f:
            data = f.read()
    else:
        print("   下载 %s" % name)
        req = urllib.request.Request(UPSTREAM_BASE + path, headers={"User-Agent": "rinne-fetch-fonts"})
        with urllib.request.urlopen(req, timeout=300) as r:
            data = r.read()
    got = sha256(data)
    if got != want:
        sys.exit("!! %s 的 sha256 是 %s，期望 %s；上游文件与钉死的提交不一致，拒绝使用" % (name, got, want))
    return data


# ---- 子集化、固定轴与改名 ------------------------------------------------------
def build_font(data, chars, axes, rename):
    from fontTools import subset
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer

    opts = subset.Options()
    opts.layout_features = ["*"]
    opts.name_IDs = ["*"]
    opts.name_legacy = True
    opts.name_languages = ["*"]
    opts.notdef_outline = True  # 缺字画方框而不是空白：方块是看得见的错误，空白不是
    opts.glyph_names = False
    font = subset.load_font(io.BytesIO(data), opts)
    sub = subset.Subsetter(options=opts)
    sub.populate(text=chars)
    sub.subset(font)
    if axes:
        limits = {k: (v if isinstance(v, tuple) else v) for k, v in axes.items()}
        font = instancer.instantiateVariableFont(font, limits, inplace=False)
    if rename:
        rename_font(font, *rename)
    alias_nnbsp(font)
    font.flavor = "woff2"
    buf = io.BytesIO()
    font.save(buf)
    return buf.getvalue()


def unique_id(version_string, ps_name):
    """nameID 3 的惯例是 "<版本>;<厂商>;<PostScript 名>"。版本取自 nameID 5（形如 "Version 3.201"），
    厂商段改成本仓；取不到版本就只留后两段。"""
    m = re.search(r"Version\s+([0-9][0-9.]*)", version_string or "")
    return ";".join(p for p in (m.group(1) if m else "", "Synchain", ps_name) if p)


def mentions(value, upstream):
    """name 记录是否提到上游族名：忽略大小写与空白，"DMMono-Medium" 也算提到 "DM Mono"。"""
    return re.sub(r"\s+", "", upstream).casefold() in re.sub(r"\s+", "", value).casefold()


def rename_font(font, family, style, ps_name, upstream, weight):
    """按输出表重写 name 表与字重。upstream 不为 None 表示改了名：去掉别的 name 记录里残留的上游族名并复核，
    让装进系统或被开发者工具读出来的名字也是登记的族名。署名记录（NAME_IDS_ATTRIBUTION）原样保留，并补齐缺失的 13。"""
    name = font["name"]
    keep_before = {
        (r.nameID, r.platformID, r.platEncID, r.langID): r.toUnicode() for r in name.names if r.nameID in NAME_IDS_ATTRIBUTION
    }
    ribbi = style in ("Regular", "Bold", "Italic", "Bold Italic")
    full = "%s %s" % (family, style)
    for slot in sorted({(r.platformID, r.platEncID, r.langID) for r in name.names}):
        values = {
            1: family if ribbi else full,
            2: style if ribbi else "Regular",
            3: unique_id(name.getName(5, *slot).toUnicode() if name.getName(5, *slot) else "", ps_name),
            4: full,
            6: ps_name,
            16: family,
            17: style,
        }
        for nid, value in values.items():
            if nid in (16, 17) and ribbi and name.getName(nid, *slot) is None:
                continue
            name.setName(value, nid, *slot)
        if name.getName(13, *slot) is None:
            name.setName(OFL_LICENSE_DESC, 13, *slot)
    font["OS/2"].usWeightClass = weight
    if not upstream:
        return
    # 可变字体遗留的变体名前缀、命名实例的 PostScript 名等记录会带着上游名字，固定轴后它们没有用处，直接删除
    name.names = [
        r
        for r in name.names
        if r.nameID in NAME_IDS_ATTRIBUTION or r.nameID in NAME_IDS_FAMILY or not mentions(r.toUnicode(), upstream)
    ]
    # 复核：署名以外的每一条 name 记录都不得残留上游族名（失败即不产出）
    stray = sorted({r.toUnicode() for r in name.names if r.nameID not in NAME_IDS_ATTRIBUTION and mentions(r.toUnicode(), upstream)})
    if stray:
        raise SystemExit("!! name 表仍含上游族名 %r：%s" % (upstream, stray))
    keep_after = {
        (r.nameID, r.platformID, r.platEncID, r.langID): r.toUnicode() for r in name.names if r.nameID in NAME_IDS_ATTRIBUTION
    }
    if any(keep_after.get(k) != v for k, v in keep_before.items()):
        raise SystemExit("!! 署名记录（nameID %s）被改动" % (NAME_IDS_ATTRIBUTION,))


def alias_nnbsp(font):
    """法语排版用窄不换行空格 U+202F。字体里没有这个字形时，让它指向细空格（没有就指向不换行空格）的字形，
    这样宽度在整条字体栈里是确定的，而不是落到系统字体。"""
    cmap_tables = [t for t in font["cmap"].tables if t.isUnicode()]
    if not cmap_tables or 0x202F in font.getBestCmap():
        return
    best = font.getBestCmap()
    source = best.get(0x2009) or best.get(0x200A) or best.get(0x00A0)
    if source is None:
        return
    for t in cmap_tables:
        t.cmap[0x202F] = source


USAGE = """用法：python scripts/fetch_fonts.py [输出目录] [--source-dir 目录] [--print-charset] [--help]
  默认输出 <仓库根>/web/fonts，覆盖 OUTPUTS 里的全部 woff2。
  --source-dir 目录   使用已下载的源文件（文件名见 SOURCES），仍然校验 sha256。
  --print-charset     只扫描并打印字符集统计，不联网、不写文件。
  --help, -h          打印本说明并退出。"""


def main(argv):
    args, source_dir, print_only = [], None, False
    it = iter(argv)
    for a in it:
        if a in ("--help", "-h"):
            print(USAGE)
            return 0
        if a == "--print-charset":
            print_only = True
        elif a == "--source-dir":
            source_dir = next(it, None)
            if not source_dir:
                sys.exit("!! --source-dir 需要一个目录\n" + USAGE)
        elif a.startswith("-"):
            sys.exit("!! 无法识别的参数 %s\n%s" % (a, USAGE))
        else:
            args.append(a)
    if len(args) > 1:
        sys.exit("!! 只接受 0 或 1 个位置参数（输出目录）\n" + USAGE)
    out = os.path.abspath(args[0] if args else os.path.join(WEB, "fonts"))

    latin, everything, detail = build_charsets()
    cjk = [c for c in everything if is_cjk(c)]
    print("扫描 web/ 的非 ASCII 字符来源：")
    for rel, n in detail:
        if n:
            print("   %-40s %5d" % (rel, n))
    print("拉丁子集 %d 字符；全集 %d 字符（其中汉字与全角 %d）" % (len(latin), len(everything), len(cjk)))
    if print_only:
        return 0

    os.makedirs(out, exist_ok=True)
    cache = {}
    for fname, src, axes, rename, which in OUTPUTS:
        if src not in cache:
            cache[src] = load_source(src, source_dir)
        data = build_font(cache[src], latin if which == "latin" else everything, axes, rename)
        with open(os.path.join(out, fname), "wb") as f:
            f.write(data)
        print("OK %-20s %8d 字节%s" % (fname, len(data), "（已改名）" if rename and rename[3] else ""))
    print("->", out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
