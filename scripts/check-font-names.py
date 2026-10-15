# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/web.json
"""字体名称门禁：OFL-1.1 的保留字体名与署名义务。

IBM Plex 的保留字体名是 "Plex"。子集化算 Modified Version，按 OFL-1.1 第 3 条，分发物不得再用这个名字，
所以 Rinne Sans 与 Rinne Mono 的 name 表里除署名记录外不得出现 "Plex"。第 2 条要求每份拷贝都带版权声明与许可证，
所以四个字体的 nameID 0（版权）、13（许可证声明）、14（许可证网址）都必须在，并且版权行要与 REUSE.toml 里
该文件的登记逐字一致。

判据：
  1. 四个 woff2 都存在、能打开；
  2. 有登记名的字体（四款都有）：nameID 1、4、6、16、17 与 fetch_fonts.OUTPUTS 登记的名字一致（非 RIBBI 样式的 nameID 1 为「族名 样式」）；
     改名的两款另要求署名之外的所有 name 记录（含 nameID 3）都不含保留字体名（大小写不敏感）；
  3. 所有字体：nameID 0、13、14 存在且非空；族名记录（1、4、6、16）不含该家族的保留字体名；
  4. REUSE.toml 里每个字体文件都有一个块，版权行与 nameID 0 去掉 "Copyright"、"(c)"、"©" 前缀后逐字相同。
  5. check-text-fit.py 里手抄的 U+202F 前进宽度与字体里的真实值一致（容差 0.005 em）。
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

# 各家族的保留字体名：Plex 来自 IBM Plex，Source 来自 Noto Sans SC 的版权行；Space Grotesk 没有
RESERVED = {"RinneSans.woff2": "Plex", "RinneMono.woff2": "Plex", "NotoSansSC.woff2": "Source", "SpaceGrotesk.woff2": None}
# 与 fetch_fonts 的输出表对拍，防止两边悄悄漂移
for _fname, _src, _axes, _naming, _which in ff.OUTPUTS:
    if _naming and _naming[3]:
        assert RESERVED[_fname] == _naming[3], "%s 的保留字体名与 fetch_fonts.OUTPUTS 不一致" % _fname

FAMILY_IDS = (1, 4, 6, 16)


def records(font):
    return [(r.nameID, r.toUnicode()) for r in font["name"].names]


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
    expected 形如 {"sans": 0.118, "grotesk": 0.185}；等宽字体固定 0.6 em。"""
    out = []
    files = {"sans": "RinneSans.woff2", "grotesk": "SpaceGrotesk.woff2", "mono": "RinneMono.woff2"}
    for kind, fname in files.items():
        want = 0.6 if kind == "mono" else expected[kind]
        path = os.path.join(fonts_dir, fname)
        if not os.path.isfile(path):
            continue
        font = TTFont(path)
        glyph = font.getBestCmap().get(0x202F)
        if glyph is None:
            out.append("%s：没有 U+202F 的字形" % fname)
            continue
        got = font["hmtx"][glyph][0] / font["head"].unitsPerEm
        if abs(got - want) > tolerance:
            out.append("%s：U+202F 的前进宽度是 %.3f em，check-text-fit.py 里写的是 %.3f em" % (fname, got, want))
    return out


def find_problems(fonts_dir, reuse):
    """检查 fonts_dir 里的四个字体，返回问题字符串数组。reuse 是 {路径: 版权行}。"""
    problems = []
    names = {o[0]: o[3] for o in ff.OUTPUTS}
    for fname in RESERVED:
        path = os.path.join(fonts_dir, fname)
        if not os.path.isfile(path):
            problems.append("%s：文件不存在" % fname)
            continue
        font = TTFont(path)
        recs = records(font)
        by_id = {}
        for nid, val in recs:
            by_id.setdefault(nid, set()).add(val)
        reserved = RESERVED[fname]
        naming = names[fname]

        for nid, what in ((0, "版权"), (13, "许可证声明"), (14, "许可证网址")):
            if not any(v.strip() for v in by_id.get(nid, ())):
                problems.append("%s：缺 nameID %d（%s），OFL-1.1 第 2 条要求随分发物保留" % (fname, nid, what))
        if reserved:
            for nid in FAMILY_IDS:
                for v in by_id.get(nid, ()):
                    if reserved.casefold() in v.casefold():
                        problems.append("%s：nameID %d 含保留字体名 %r：%r" % (fname, nid, reserved, v))
        if naming:
            family, style, ps_name, rfn, _weight = naming
            ribbi = style in ("Regular", "Bold", "Italic", "Bold Italic")
            want = {1: family if ribbi else "%s %s" % (family, style), 4: "%s %s" % (family, style), 6: ps_name, 16: family, 17: style}
            for nid, expect in want.items():
                got = by_id.get(nid, set())
                # Regular 样式的字体可以没有排版族名与子族名（16、17），有就必须是登记的名字
                if nid in (16, 17) and not got and style == "Regular":
                    continue
                if got != {expect}:
                    problems.append("%s：nameID %d 应为 %r，实际 %r" % (fname, nid, expect, sorted(got)))
            for nid, val in recs if rfn else ():
                if nid not in ff.NAME_IDS_ATTRIBUTION and rfn.casefold() in val.casefold():
                    problems.append("%s：nameID %d 残留保留字体名 %r：%r" % (fname, nid, rfn, val))

        key = "web/fonts/" + fname
        line = next(iter(by_id.get(0, {""})))
        if key not in reuse:
            problems.append("%s：REUSE.toml 里没有这个字体文件的块" % fname)
        elif strip_prefix(line) != reuse[key].strip():
            problems.append("%s：REUSE.toml 版权行 %r 与 nameID 0 %r 不一致" % (fname, reuse[key], strip_prefix(line)))
    problems.extend(nnbsp_problems(fonts_dir, load_text_fit().NNBSP_EM))
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
            edit(font)
            font.flavor = "woff2"
            font.save(os.path.join(tmp, fname))
            return find_problems(tmp, reuse)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

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

    cases = [
        ("族名被改回上游名", "RinneSans.woff2", set_name(1, "IBM Plex Sans")),
        ("PostScript 名含保留字体名", "RinneMono.woff2", set_name(6, "IBMPlexMono-Regular")),
        ("某条非署名记录残留保留字体名", "RinneSans.woff2", set_name(5, "Plex Version 3.201")),
        ("Noto 的族名含保留字体名 Source", "NotoSansSC.woff2", set_name(1, "Source Sans")),
        ("全名 nameID 4 含保留字体名", "RinneSans.woff2", set_name(4, "Plex Sans Regular")),
        ("排版族名 nameID 16 含保留字体名", "RinneMono.woff2", set_name(16, "IBM Plex Mono")),
        ("子族名 nameID 17 含保留字体名", "RinneSans.woff2", set_name(17, "Plex")),
        ("唯一标识 nameID 3 含保留字体名", "RinneMono.woff2", set_name(3, "2.3;IBM;PlexMono")),
        ("Noto 的 PostScript 名漂回 Thin", "NotoSansSC.woff2", set_name(6, "NotoSansSC-Thin")),
        ("缺许可证声明", "SpaceGrotesk.woff2", drop_name(13)),
        ("版权行与 REUSE 登记不一致", "NotoSansSC.woff2", set_name(0, "Copyright 1999 Nobody")),
    ]
    # 署名记录（nameID 0、7、13、14）不参与保留字体名复核：OFL 的保留名限制只针对呈现给用户的主字体名。这里以 nameID 7 的商标声明作正例，改动它不应变红
    if mutated("RinneSans.woff2", set_name(7, "IBM Plex(r) is a trademark of IBM Corp.")):
        fails.append("nameID 7 里的商标声明不应触发保留字体名判据")
    for label, fname, edit in cases:
        if not mutated(fname, edit):
            fails.append("%s：应当变红却没有" % label)
    if not nnbsp_problems(fonts_dir, {"sans": 0.2, "grotesk": 0.185}):
        fails.append("U+202F 宽度与字体不符时应当变红却没有")
    if nnbsp_problems(fonts_dir, load_text_fit().NNBSP_EM):
        fails.append("check-text-fit.py 里的 U+202F 宽度应当与字体一致")
    if fails:
        for f in fails:
            print("[FAIL] 自测：" + f)
        return 1
    print("[PASS] check-font-names 自测通过（坏样例 %d、正例 2）" % len(cases))
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
    print("[PASS] 四个字体的名称、署名与 REUSE 登记全部符合 OFL-1.1")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
