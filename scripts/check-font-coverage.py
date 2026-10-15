# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/web.json
"""字体子集覆盖门禁：重算界面必需的字符集，逐字对着 web/fonts/*.woff2 断言。

为什么要有这道门：词条改了而子集没重跑，新字上屏就是方块，人眼审 PR diff 永远看不出
「这个新汉字字体里没有」。所以由脚本拿 fontTools 读各个 woff2 的 cmap，逐字核对。

判定口径与 tokens.css 的三条字体栈一致：三条栈都是「拉丁族、Noto Sans SC、系统字体」，
且 Noto 子集含它有字形的全部字符。所以只要某个必需字符四款字体里一款都没有，运行期就必然落到系统字体或方块，判红。
某字只是拉丁三款没有、Noto 有，是设计内的逐字回退，只记 [INFO]。

必需字符集由 fetch_fonts.required_chars() 给出（web/ 里实际出现的非 ASCII 字符，加设计交接包词条
用到的字符），与生成侧是同一份代码。更宽的兜底字符（常用汉字、拉丁扩展等）只做尽力而为，缺了记 [INFO]。

用法：
    python scripts/check-font-coverage.py            缺字即非零退出
    python scripts/check-font-coverage.py --verbose  另打印每款字体的逐字亏空明细
    python scripts/check-font-coverage.py --self-test
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

try:
    from fontTools.ttLib import TTFont
except ImportError:
    # 缺依赖必须判红，不是跳过：缺依赖判绿是假绿
    sys.exit("!! 缺 fontTools：pip install fonttools brotli（woff2 解压需要 brotli，两者缺一不可）")

import fetch_fonts as ff

# 文件名的真源在 fetch_fonts.py（产出这些文件的一侧），这里只派生，不手抄第二份
LATIN_FONTS = list(ff.LATIN_OUTPUTS)
CJK_FONT = ff.CJK_OUTPUT
ALL_FONTS = LATIN_FONTS + [CJK_FONT]

# 四款上游家族本身就没有、重跑 fetch_fonts.py 也补不上的字符。现在是空的；
# 需要登记时必须写明字符和理由。这张表会自己保鲜：条目其实有了字形，或已不再被任何文案使用，都判红。
KNOWN_ABSENT_UPSTREAM = {}


def font_charset(path):
    with TTFont(path, lazy=True) as f:
        return set(f.getBestCmap().keys())


def fmt(chars):
    return " ".join("%s(U+%04X)" % (c, ord(c)) for c in sorted(chars))


def self_test():
    """必需字符里混入一个四款字体都没有的字符，必须判红；原件必须干净。"""
    fonts_dir = os.path.join(ROOT, "web", "fonts")
    have = set().union(*(font_charset(os.path.join(fonts_dir, f)) for f in ALL_FONTS))
    required, _ = ff.required_chars()
    absent = chr(0xA66E)  # 斯拉夫字母 Multiocular O：四款字体都没有
    fails = []
    if any(ord(c) not in have for c in required):
        fails.append("仓内字体应当覆盖全部必需字符")
    if ord(absent) in have:
        fails.append("自测用的缺字样例其实有字形了，换一个")
    elif not {c for c in required | {absent} if ord(c) not in have}:
        fails.append("混入缺字后应当判红")
    if fails:
        for f in fails:
            print("[FAIL] 自测：" + f)
        return 1
    print("[PASS] check-font-coverage 自测通过（坏样例 1、正例 1）")
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    verbose = "--verbose" in argv
    for a in argv:
        if a != "--verbose":
            sys.exit("!! 无法识别的参数 %s\n用法：python scripts/check-font-coverage.py [--verbose|--self-test]" % a)

    fonts_dir = os.path.join(ROOT, "web", "fonts")
    missing_files = [f for f in ALL_FONTS if not os.path.isfile(os.path.join(fonts_dir, f))]
    if missing_files:
        sys.exit("!! web/fonts/ 缺文件：%s" % ", ".join(missing_files))

    required, _detail = ff.required_chars()
    latin, everything, _ = ff.build_charsets()
    wanted = set(everything)

    have = {f: font_charset(os.path.join(fonts_dir, f)) for f in ALL_FONTS}
    union = set().union(*have.values())

    tofu = {c for c in required if ord(c) not in union}
    print("必需字符 %d；子集目标字符 %d；四款字体合计有字形 %d" % (len(required), len(wanted), len(union)))
    for f in ALL_FONTS:
        print("   %-20s 字形 %5d" % (f, len(have[f])))

    stale = []
    for c in sorted(KNOWN_ABSENT_UPSTREAM):
        if ord(c) in union:
            stale.append("%s(U+%04X) 现在有字形了，请从 KNOWN_ABSENT_UPSTREAM 删除" % (c, ord(c)))
        elif c not in required:
            stale.append("%s(U+%04X) 已无任何文案使用，请从 KNOWN_ABSENT_UPSTREAM 删除" % (c, ord(c)))
    if stale:
        print("[FAIL] 上游缺字白名单已过期：")
        for s in stale:
            print("       " + s)
        return 1

    allowed = {c for c in tofu if c in KNOWN_ABSENT_UPSTREAM}
    tofu -= allowed

    latin_gap = {c for c in latin if all(ord(c) not in have[f] for f in LATIN_FONTS)} - tofu
    if latin_gap:
        print("[INFO] 拉丁三款无字形、由 Noto 逐字回退（设计内）：%s" % fmt(latin_gap))
    best_effort = {c for c in wanted - required if ord(c) not in union}
    if best_effort:
        print("[INFO] 兜底字符里四款上游都没有的 %d 个（尽力而为，不判红）：%s" % (len(best_effort), fmt(best_effort)))
    if verbose:
        for f in ALL_FONTS:
            target = set(latin) if f in LATIN_FONTS else wanted
            gap = {c for c in target if ord(c) not in have[f]}
            print("[明细] %s 亏空 %d：%s" % (f, len(gap), fmt(gap)))

    if tofu:
        print("[FAIL] %d 个必需字符四款字体都没有字形，运行期必然上屏方块或回退系统字体：" % len(tofu))
        print("       %s" % fmt(tofu))
        print("       修复：python scripts/fetch_fonts.py（需联网），再同步 web/fonts/README.md 的字符数与字节数。")
        return 1
    print("[PASS] 必需的 %d 个字符全部有字形" % len(required))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
