# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/web.json
"""字体名称门禁：改名是否改干净、OFL-1.1 的保留字体名与署名义务。

Rinne Sans 与 Rinne Mono 由 Figtree 与 DM Mono 子集化后改名。两款都没有保留字体名，改名是为了让 CSS 族名不随上游变，
所以要改就改干净：name 表里除署名记录外不得再出现上游族名，否则装进系统或被开发者工具读出来的名字前后不一。
Noto Sans SC 的保留字体名是 "Source"（写在它的版权行里），分发名不得使用它（OFL-1.1 第 3 条）。第 2 条要求每份拷贝
都带版权声明与许可证，所以每个字体文件的 nameID 0（版权）、13（许可证声明）、14（许可证网址）都必须在，并且版权行
要与 REUSE.toml 里该文件的登记逐字一致。

判据：
  1. fetch_fonts.OUTPUTS 的每个 woff2 都存在、能打开；
  2. 有登记名的字体（全部都有）：nameID 1、4、6、16、17 与 fetch_fonts.OUTPUTS 登记的名字一致（非 RIBBI 样式的 nameID 1 为「族名 样式」）；
     改了名的（Rinne Sans 两个文件与 Rinne Mono）另要求署名之外的所有 name 记录（含 nameID 3）都不提上游族名
     （忽略大小写与空白，"DMMono" 也算）；
  3. 所有字体：nameID 0、13、14 存在且非空；族名记录（1、4、6、16）不含该家族的保留字体名；
  4. REUSE.toml 里每个字体文件都有一个块，版权行与 nameID 0 去掉 "Copyright"、"(c)"、"©" 前缀后逐字相同。
  5. check-text-fit.py 里手抄的 U+202F 前进宽度与字体里的真实值一致（容差 0.005 em；Rinne Sans 两个字重都核）。
用法：python scripts/check-font-names.py [--self-test]
"""

import os
import re
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
    sys.exit("!! 缺 fontTools：pip install fonttools brotli（缺依赖必须判红，不能判绿）")

import fetch_fonts as ff

# 各文件的保留字体名：Source 来自 Noto Sans SC 的版权行；Space Grotesk、Figtree、DM Mono 没有
RESERVED = {
    "RinneSans.woff2": None,
    "RinneSansSemiBold.woff2": None,
    "RinneMono.woff2": None,
    "NotoSansSC.woff2": "Source",
    "SpaceGrotesk.woff2": None,
}


def registry_problems(reserved, outputs):
    """RESERVED 与 fetch_fonts.OUTPUTS 的文件必须一一对应：多产出一个文件而这里没登记，它就逃过了全部判据。"""
    want = {o[0] for o in outputs}
    return ["%s：fetch_fonts.OUTPUTS 产出它，RESERVED 没登记" % f for f in sorted(want - set(reserved))] + [
        "%s：RESERVED 登记了，fetch_fonts.OUTPUTS 不产出" % f for f in sorted(set(reserved) - want)
    ]

FAMILY_IDS = (1, 4, 6, 16)


def records(font):
    return [(r.nameID, r.toUnicode()) for r in font["name"].names]


def squash(text):
    """忽略大小写与空白的比较形式："DMMono-Medium" 里也能找到 "DM Mono"。"""
    return re.sub(r"\s+", "", text).casefold()


def strip_prefix(line):
    return re.sub(r"^(?:Copyright\s*)?(?:\(c\)|©)?\s*", "", line.strip(), flags=re.I)


def reuse_copyrights():
    """从 REUSE.toml 读出 {路径: 版权行}。只认 [[annotations]] 块里的 path 与 SPDX-FileCopyrightText。"""
    with open(os.path.join(ROOT, "REUSE.toml"), "r", encoding="utf-8") as f:
        text = f.read()
    out = {}
    for block in text.split("[[annotations]]")[1:]:
        c = re.search(r'^SPDX-FileCopyrightText\s*=\s*"((?:[^"\\]|\\.)*)"', block, re.M)
        p = re.search(r"^path\s*=\s*(\[[^\]]*\]|\"[^\"]*\")", block, re.M)
        if not (c and p):
            continue
        for path in re.findall(r'"([^"]+)"', p.group(1)):
            out[path] = c.group(1).replace('\\"', '"')
    return out


def load_text_fit():
    import importlib.util

    spec = importlib.util.spec_from_file_location("check_text_fit", os.path.join(HERE, "check-text-fit.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def nnbsp_problems(fonts_dir, expected, tolerance=0.005):
    """check-text-fit.py 里手抄的 U+202F 宽度（em）必须与字体里的真实前进宽度一致。
    expected 形如 {"sans": 0.244, "grotesk": 0.185}；等宽字体固定 0.6 em。check-text-fit.py 对 Rinne Sans 只用一个值，
    所以正文与强调两个文件都要对得上。"""
    out = []
    files = [
        ("sans", "RinneSans.woff2"),
        ("sans", "RinneSansSemiBold.woff2"),
        ("grotesk", "SpaceGrotesk.woff2"),
        ("mono", "RinneMono.woff2"),
    ]
    for kind, fname in files:
        want = 0.6 if kind == "mono" else expected[kind]
        path = os.path.join(fonts_dir, fname)
        if not os.path.isfile(path):
            continue
        with TTFont(path) as font:  # 用完就关：Windows 上还开着的字体文件会让临时目录删不掉
            glyph = font.getBestCmap().get(0x202F)
            if glyph is None:
                out.append("%s：没有 U+202F 的字形" % fname)
                continue
            got = font["hmtx"][glyph][0] / font["head"].unitsPerEm
        if abs(got - want) > tolerance:
            out.append("%s：U+202F 的前进宽度是 %.3f em，check-text-fit.py 里写的是 %.3f em" % (fname, got, want))
    return out


def find_problems(fonts_dir, reuse, nnbsp_expected=None, reserved=None):
    """检查 fonts_dir 里的全部字体文件，返回问题字符串数组。reuse 是 {路径: 版权行}；reserved 缺省为 RESERVED（自测才换）。"""
    reserved = RESERVED if reserved is None else reserved
    problems = registry_problems(reserved, ff.OUTPUTS)
    names = {o[0]: o[3] for o in ff.OUTPUTS}
    for fname in reserved:
        path = os.path.join(fonts_dir, fname)
        if not os.path.isfile(path):
            problems.append("%s：文件不存在" % fname)
            continue
        with TTFont(path) as font:
            recs = records(font)
        by_id = {}
        for nid, val in recs:
            by_id.setdefault(nid, set()).add(val)
        rfn = reserved[fname]
        naming = names[fname]

        for nid, what in ((0, "版权"), (13, "许可证声明"), (14, "许可证网址")):
            if not any(v.strip() for v in by_id.get(nid, ())):
                problems.append("%s：缺 nameID %d（%s），OFL-1.1 第 2 条要求随分发物保留" % (fname, nid, what))
        if rfn:
            for nid in FAMILY_IDS:
                for v in by_id.get(nid, ()):
                    if rfn.casefold() in v.casefold():
                        problems.append("%s：nameID %d 含保留字体名 %r：%r" % (fname, nid, rfn, v))
        if naming:
            family, style, ps_name, upstream, _weight = naming
            ribbi = style in ("Regular", "Bold", "Italic", "Bold Italic")
            want = {1: family if ribbi else "%s %s" % (family, style), 4: "%s %s" % (family, style), 6: ps_name, 16: family, 17: style}
            for nid, expect in want.items():
                got = by_id.get(nid, set())
                # Regular 样式的字体可以没有排版族名与子族名（16、17），有就必须是登记的名字
                if nid in (16, 17) and not got and style == "Regular":
                    continue
                if got != {expect}:
                    problems.append("%s：nameID %d 应为 %r，实际 %r" % (fname, nid, expect, sorted(got)))
            for nid, val in recs if upstream else ():
                # 判定自己写一份，不借生成侧的 mentions：两边共用一个谓词，它坏了两边会一起看不见
                if nid not in ff.NAME_IDS_ATTRIBUTION and squash(upstream) in squash(val):
                    problems.append("%s：nameID %d 残留上游族名 %r：%r" % (fname, nid, upstream, val))

        key = "web/fonts/" + fname
        line = next(iter(by_id.get(0, {""})))
        if key not in reuse:
            problems.append("%s：REUSE.toml 里没有这个字体文件的块" % fname)
        elif strip_prefix(line) != reuse[key].strip():
            problems.append("%s：REUSE.toml 版权行 %r 与 nameID 0 %r 不一致" % (fname, reuse[key], strip_prefix(line)))
    problems.extend(nnbsp_problems(fonts_dir, nnbsp_expected or load_text_fit().NNBSP_EM))
    return problems


def self_test():
    """每条判据都做一份被改坏的拷贝，必须变红；原件必须干净。"""
    import shutil
    import tempfile

    fonts_dir = os.path.join(ROOT, "web", "fonts")
    reuse = reuse_copyrights()
    fails = []
    if find_problems(fonts_dir, reuse):
        fails.append("仓内字体应当干净")

    def mutated(fname, edit):
        tmp = tempfile.mkdtemp(prefix="rinne-fonts-")
        try:
            for f in RESERVED:
                shutil.copy(os.path.join(fonts_dir, f), tmp)
            font = TTFont(os.path.join(tmp, fname))
            try:
                edit(font)
                font.flavor = "woff2"
                font.save(os.path.join(tmp, fname))
            finally:
                font.close()  # Windows 上字体文件还开着时删不掉目录，ignore_errors 会把失败吞掉、留下残留
            return find_problems(tmp, reuse)
        finally:
            # rinne-delete-ok: tmp 是本函数 mkdtemp 新建的目录，变量没有被改过
            shutil.rmtree(tmp, onerror=lambda _f, p, e: print("临时目录没清掉：%s：%s" % (p, e[1]), file=sys.stderr))

    def set_name(nid, value):
        def edit(font):
            name = font["name"]
            found = False
            for r in name.names:
                if r.nameID == nid:
                    r.string = value
                    found = True
            if not found:
                name.setName(value, nid, 3, 1, 0x409)
        return edit

    def drop_name(nid):
        def edit(font):
            font["name"].names = [r for r in font["name"].names if r.nameID != nid]
        return edit

    def widen_nnbsp(font):
        glyph = font.getBestCmap()[0x202F]
        font["hmtx"][glyph] = (font["head"].unitsPerEm // 2, font["hmtx"][glyph][1])

    cases = [
        ("族名被改回上游名", "RinneSans.woff2", set_name(1, "Figtree Medium")),
        ("PostScript 名漂回上游名", "RinneMono.woff2", set_name(6, "DMMono-Medium")),
        ("某条非署名记录残留上游族名", "RinneSans.woff2", set_name(25, "Figtree")),
        ("Noto 的族名含保留字体名 Source", "NotoSansSC.woff2", set_name(1, "Source Sans")),
        ("强调字重的全名 nameID 4 漂回上游名", "RinneSansSemiBold.woff2", set_name(4, "Figtree SemiBold")),
        ("排版族名 nameID 16 漂回上游名", "RinneMono.woff2", set_name(16, "DM Mono")),
        ("正文字重的子族名 nameID 17 写成 Regular", "RinneSans.woff2", set_name(17, "Regular")),
        ("强调字重的子族名 nameID 17 写成 Medium", "RinneSansSemiBold.woff2", set_name(17, "Medium")),
        ("唯一标识 nameID 3 残留不带空格的上游名", "RinneMono.woff2", set_name(3, "1.000;CF;DMMono-Medium")),
        ("Noto 的 PostScript 名漂回 Thin", "NotoSansSC.woff2", set_name(6, "NotoSansSC-Thin")),
        ("缺许可证声明", "SpaceGrotesk.woff2", drop_name(13)),
        ("强调字重缺版权行", "RinneSansSemiBold.woff2", drop_name(0)),
        ("版权行与 REUSE 登记不一致", "NotoSansSC.woff2", set_name(0, "Copyright 1999 Nobody")),
        ("强调字重的 U+202F 宽度与 check-text-fit.py 不符", "RinneSansSemiBold.woff2", widen_nnbsp),
    ]
    # 署名记录（nameID 0、7、13、14）不参与残留名字复核：它们按 OFL-1.1 第 2 条逐字保留，本来就会写着上游名字。这里以 nameID 7 的商标声明作正例，改动它不应变红
    if mutated("RinneSans.woff2", set_name(7, "Figtree is a trademark of Erik Kennedy.")):
        fails.append("nameID 7 里的商标声明不应触发残留上游族名判据")
    for label, fname, edit in cases:
        if not mutated(fname, edit):
            fails.append("%s：应当变红却没有" % label)
    partial = {k: v for k, v in reuse.items() if k != "web/fonts/RinneSansSemiBold.woff2"}
    if not find_problems(fonts_dir, partial):
        fails.append("REUSE.toml 少登记一个字体文件：应当变红却没有")
    if not find_problems(fonts_dir, reuse, reserved={k: v for k, v in RESERVED.items() if k != "RinneSansSemiBold.woff2"}):
        fails.append("RESERVED 漏登记 fetch_fonts.OUTPUTS 的一个文件：应当变红却没有")
    if not find_problems(fonts_dir, reuse, {"sans": 0.2, "grotesk": 0.185}):
        fails.append("经 find_problems 入口：U+202F 宽度与字体不符时应当变红却没有")
    if not nnbsp_problems(fonts_dir, {"sans": 0.2, "grotesk": 0.185}):
        fails.append("U+202F 宽度与字体不符时应当变红却没有")
    if nnbsp_problems(fonts_dir, load_text_fit().NNBSP_EM):
        fails.append("check-text-fit.py 里的 U+202F 宽度应当与字体一致")
    if fails:
        for f in fails:
            print("[FAIL] 自测：" + f)
        return 1
    print("[PASS] check-font-names 自测通过（坏样例 %d、正例 2）" % (len(cases) + 2))
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    problems = find_problems(os.path.join(ROOT, "web", "fonts"), reuse_copyrights())
    if problems:
        print("[FAIL] 字体名称门禁 %d 项：" % len(problems))
        for p in problems:
            print("   " + p)
        return 1
    print("[PASS] %d 个字体文件的名称、署名与 REUSE 登记全部符合要求" % len(RESERVED))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
