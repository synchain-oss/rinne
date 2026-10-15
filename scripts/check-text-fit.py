# SPDX-License-Identifier: GPL-3.0-or-later
"""check-text-fit.py：离线估算界面文本在设计盒里占多宽、折几行。

读 web/shared/text-budget.json（每个词条键的行宽预算 maxWidthPx 与字体设置 fit）和合并后的三语词条
（`node scripts/check-i18n.mjs --dump-json`，或用 --strings 指向同形的 JSON 文件）。
对每个同时有预算和词条的键：把占位符换成最坏情况的样例值，按贪心折行，报告超出预算的条目：
单行预算看总宽是否超过 maxWidthPx，多行预算看行数是否超过 fit.lines。有超出则退出 1。

字宽表：拉丁字符的逐字前进宽度（单位 em）取自界面实际使用的子集字体（Rinne Sans 与 Space Grotesk），
Rinne Mono 是 0.6 em 的等宽，汉字与全角标点按 1.0 em（Noto Sans SC）。
只用 Python 标准库。

    python scripts/check-text-fit.py [--strings 文件] [--lang zh,en] [--lang fr]
                                     [--margin 0.04] [--verbose] [--self-test]
"""

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))

SANS_GROUPS = {
    0.236: ' ', 0.242: "'", 0.25: 'ijìíîï', 0.272: ',.l', 0.273: '’', 0.284: '!', 0.292: ':;',
    0.314: '|', 0.317: '[]', 0.324: 'f', 0.326: '·', 0.335: '()', 0.343: '{}', 0.351: 't',
    0.367: 'r', 0.383: '/\\', 0.399: '-', 0.4: 'IÌÍÎÏ', 0.419: '"', 0.45: '*', 0.464: 'z',
    0.468: '°', 0.474: '”', 0.475: '“', 0.477: '?', 0.487: 's', 0.492: 'v', 0.499: 'yýÿ',
    0.501: 'L', 0.503: 'cç', 0.507: 'x', 0.51: 'J', 0.513: '«»', 0.527: 'k', 0.528: 'g',
    0.534: 'aàáâä', 0.549: 'eèéêë', 0.559: 'F', 0.56: 'oòóôö', 0.565: '_', 0.568: 'hnuùúûü',
    0.572: 'T', 0.58: 'Zbdpq', 0.581: 'S', 0.583: 'EÈÉÊË', 0.586: '§', 0.588: '–', 0.593: 'YÝ',
    0.598: '$', 0.6: '+0123456789<=>^`~±×−≥≤', 0.606: 'P', 0.609: 'V', 0.613: 'X', 0.621: 'CÇ',
    0.634: 'K', 0.64: 'R', 0.641: 'AÀÁÂÄ', 0.653: 'B', 0.671: 'D', 0.678: 'UÙÚÛÜ', 0.694: '&',
    0.695: 'G', 0.707: 'HN', 0.708: 'OQÒÓÔÖ', 0.713: '#', 0.768: 'w', 0.78: '—', 0.803: '…',
    0.812: 'M', 0.82: '→←', 0.873: 'm', 0.891: '@W', 0.927: '%', 0.931: 'œ', 0.985: 'Œ',
}
GROTESK_GROUPS = {
    0.21: '·', 0.253: '|', 0.255: ' ', 0.259: 'IÌÍÎÏ', 0.261: 'ilìíîï', 0.263: 'j', 0.284: "',’",
    0.287: '!.:;', 0.292: '`', 0.352: '[]', 0.384: '/\\', 0.387: ')', 0.391: 'r°', 0.393: '(',
    0.437: 'f', 0.438: '-', 0.444: '1', 0.457: 't', 0.458: '{}', 0.48: '§', 0.492: '"“”',
    0.519: 'z', 0.525: 's', 0.535: '*F', 0.543: 'L', 0.546: 'v', 0.556: '7EÈÉÊË', 0.558: 'k',
    0.576: 'Z', 0.577: 'aàáâä', 0.579: '?', 0.581: 'eèéêë', 0.588: 'T', 0.589: 'cç', 0.59: '–',
    0.592: 'x', 0.594: '&', 0.596: '2', 0.6: '5', 0.603: 'P', 0.606: '8J', 0.607: '3', 0.608: '$S',
    0.613: 'oòóôö', 0.615: 'hnuyùúûüýÿ', 0.617: '69', 0.618: 'V', 0.62: '+<=>^_~±×−≥≤',
    0.621: 'YÝ', 0.622: 'K', 0.632: 'R', 0.633: '4AÀÁÂÄ', 0.634: '#', 0.639: 'bdgpq', 0.642: 'X',
    0.643: 'CÇ', 0.646: '0', 0.656: 'H', 0.661: 'G', 0.663: 'B', 0.665: 'D', 0.668: 'N',
    0.671: 'UÙÚÛÜ', 0.674: 'OQÒÓÔÖ', 0.711: '«»', 0.761: '%', 0.769: '…', 0.788: 'w', 0.8: '→←',
    0.837: 'Œ', 0.855: 'm', 0.878: 'M', 0.892: 'W', 0.894: '—', 0.993: 'œ', 1.017: '@',
}


def _expand(groups):
    table = {}
    for adv, chars in groups.items():
        for ch in chars:
            table[ch] = adv
    return table


TABLES = {"sans": _expand(SANS_GROUPS), "grotesk": _expand(GROTESK_GROUPS)}
# 子集字体里没有的字形（∞ Ø ‘ 等）会回退到系统字体，按偏宽估算。
FALLBACK_EM = {"sans": 0.62, "grotesk": 0.66, "mono": 0.6}
WEIGHT_FACTOR = {300: 0.99, 400: 1.0, 500: 1.02, 600: 1.04, 700: 1.06}
NBSP = "\u00a0"

# 占位符的最坏情况样例值：看的是设计盒里的像素宽度，不追求真实。
PLACEHOLDER_SAMPLES = {
    "g": "H", "y": "H", "x": "18.0", "s": "150", "n": "16384", "d": "4000.0", "u": "4000.0",
    "m": "341.3", "a": "0.10.12", "b": "0.10.12", "c": "−24.0", "v": "−24.0", "f": "20.0",
    "sr": "192", "bpm": "240", "t": "4000.0", "ch": "MONO", "i": "6", "min": "−24.0", "max": "4000",
    "sha": "a1b2c3d", "arch": "macOS arm64", "fmt": "VST3 | AU", "role": "Rinne Output",
    "reason": "ERROR_ACCESS_DENIED", "name": "我的超长预设名称用于测试截断效果甲乙",
    "peer": "主唱 Lead Vox 双轨叠录（副歌二）", "self": "主唱 Lead Vox 双轨叠录（副歌三）",
    "holder": "主唱 Lead Vox 双轨叠录（副歌二）", "note": "1/32 三连",
}

NO_LINE_START = set("，。、：；？！」』）》〉】,.:;?!)]}…%’”·")
NO_LINE_END = set("「『（《〈【([{‘“")


def is_cjk(ch):
    cp = ord(ch)
    return (
        0x2E80 <= cp <= 0x2FDF
        or 0x3000 <= cp <= 0x30FF
        or 0x3400 <= cp <= 0x4DBF
        or 0x4E00 <= cp <= 0x9FFF
        or 0xF900 <= cp <= 0xFAFF
        or 0xFE30 <= cp <= 0xFE4F
        or 0xFF00 <= cp <= 0xFFEF
    )


def char_advance_px(ch, fit):
    size = float(fit["size"])
    ls = float(fit.get("ls", 0)) * size
    font = fit["font"]
    if is_cjk(ch):
        return size * 1.0 + ls
    if ch == NBSP:
        ch = " "
    if font == "mono":
        em = 0.6
    else:
        em = TABLES[font].get(ch, FALLBACK_EM[font])
        em *= WEIGHT_FACTOR.get(int(fit.get("weight", 400)), 1.0)
    return em * size + ls


def text_width(text, fit):
    return sum(char_advance_px(ch, fit) for ch in text)


def substitute(text):
    def rep(m):
        key = m.group(1)
        return PLACEHOLDER_SAMPLES.get(key, "W" * 6)

    return re.sub(r"\{([a-zA-Z_]+)\}", rep, text)


def tokens(text):
    """换行机会：拉丁文在空格之后，汉字之间。"""
    out = []
    buf = ""
    for ch in text:
        if ch == " ":
            buf += ch
            out.append(buf)
            buf = ""
        elif is_cjk(ch):
            if buf and not (buf[-1] in NO_LINE_END):
                if ch in NO_LINE_START:
                    buf += ch
                    continue
                out.append(buf)
                buf = ""
            buf += ch
        else:
            if buf and is_cjk(buf[-1]) and buf[-1] not in NO_LINE_END and ch not in NO_LINE_START:
                out.append(buf)
                buf = ""
            buf += ch
    if buf:
        out.append(buf)
    return out


def wrap(text, fit, width):
    """贪心折行，返回 [(行文本, 行宽 px)]。超长的词按字符拆开。"""
    lines = []
    cur, cur_w = "", 0.0
    for tok in tokens(text):
        tw = text_width(tok, fit)
        trimmed_w = text_width(tok.rstrip(" "), fit)
        if cur and cur_w + trimmed_w > width:
            lines.append((cur.rstrip(" "), text_width(cur.rstrip(" "), fit)))
            cur, cur_w = "", 0.0
        if not cur and trimmed_w > width:
            for ch in tok:
                cw = char_advance_px(ch, fit)
                if cur and cur_w + cw > width:
                    lines.append((cur, cur_w))
                    cur, cur_w = "", 0.0
                cur += ch
                cur_w += cw
            continue
        cur += tok
        cur_w += tw
    if cur.strip():
        lines.append((cur.rstrip(" "), text_width(cur.rstrip(" "), fit)))
    return lines


def check_entry(key, entry, lang, margin):
    text = entry.get(lang, "")
    if not text:
        return None
    fit = entry.get("fit")
    width = entry.get("maxWidthPx")
    if not fit or not width:
        return None
    sample = substitute(text)
    budget = float(width) / (1.0 + margin)
    max_lines = int(fit.get("lines", 1))
    if max_lines <= 1:
        w = text_width(sample, fit)
        ok = w <= budget
        return {"key": key, "lang": lang, "ok": ok, "lines": 1, "maxLines": 1,
                "widthPx": round(w, 1), "budgetPx": width, "sample": sample}
    lines = wrap(sample, fit, budget)
    ok = len(lines) <= max_lines
    return {"key": key, "lang": lang, "ok": ok, "lines": len(lines), "maxLines": max_lines,
            "widthPx": round(max(w for _, w in lines), 1) if lines else 0, "budgetPx": width,
            "sample": sample}


def load_strings(strings_path):
    """合并后的三语词条：给了 --strings 就读那个 JSON；否则让 node 现算。"""
    if strings_path:
        with open(strings_path, encoding="utf-8") as fh:
            return json.load(fh)
    import subprocess

    node = os.environ.get("RINNE_NODE") or "node"
    out = subprocess.run([node, os.path.join(HERE, "check-i18n.mjs"), "--dump-json"], capture_output=True, check=True)
    return json.loads(out.stdout.decode("utf-8"))


def run(strings_path, budget_path, langs, margin, verbose):
    strings = load_strings(strings_path)
    with open(budget_path, encoding="utf-8") as fh:
        budget = json.load(fh)
    data = {k: dict(strings[k], **budget[k]) for k in budget if k in strings}
    results = []
    for key, entry in data.items():
        for lang in langs:
            r = check_entry(key, entry, lang, margin)
            if r:
                results.append(r)
    bad = [r for r in results if not r["ok"]]
    measured = len(results)
    print("check-text-fit：%d 项测量（%s），余量 %.0f%%，预算表 %d 条、其中已有词条 %d 条" % (measured, ",".join(langs), margin * 100, len(budget), len(data)))
    for r in bad:
        eff = r["budgetPx"] / (1.0 + margin)
        if r["maxLines"] > 1:
            print("  超出  %-34s %s  %d/%d 行 @ %dpx（有效 %.0f）" % (r["key"], r["lang"], r["lines"], r["maxLines"], r["budgetPx"], eff))
        else:
            print("  超出  %-34s %s  %.1fpx > %dpx（有效 %.0f）" % (r["key"], r["lang"], r["widthPx"], r["budgetPx"], eff))
        if verbose:
            print("        " + r["sample"])
    if verbose:
        tight = sorted((r for r in results if r["ok"] and r["maxLines"] == 1),
                       key=lambda r: r["widthPx"] / r["budgetPx"], reverse=True)[:10]
        for r in tight:
            print("  临界  %-34s %s  %.1f/%dpx" % (r["key"], r["lang"], r["widthPx"], r["budgetPx"]))
    print("check-text-fit：%s（%d 项超出预算）" % ("失败" if bad else "通过", len(bad)))
    return 1 if bad else 0


def self_test():
    fails = []

    def expect(cond, msg):
        if not cond:
            fails.append(msg)

    mono10 = {"font": "mono", "size": 10, "weight": 400, "ls": 0, "lines": 1}
    expect(abs(text_width("MMMM", mono10) - 24.0) < 1e-6, "mono 10px: 4 glyphs = 24px")
    mono_ls = dict(mono10, ls=0.1)
    expect(abs(text_width("AB", mono_ls) - 14.0) < 1e-6, "mono letter-spacing adds 1px per glyph")
    cjk12 = {"font": "sans", "size": 12, "weight": 400, "ls": 0, "lines": 1}
    expect(abs(text_width("回环反馈插件", cjk12) - 72.0) < 1e-6, "CJK = 1em per glyph")
    expect(abs(text_width("「」，", cjk12) - 36.0) < 1e-6, "full-width punctuation = 1em")
    w_i = text_width("iiii", cjk12)
    w_m = text_width("mmmm", cjk12)
    expect(w_m > 3 * w_i, "proportional sans: m much wider than i")
    bold = dict(cjk12, weight=600)
    expect(text_width("abc", bold) > text_width("abc", cjk12), "600 wider than 400 for Latin")
    expect(text_width("中", bold) == text_width("中", cjk12), "weight does not change CJK advance")
    expect(substitute("组 {g} · {n}") == "组 H · 16384", "placeholder substitution")
    two = {"font": "mono", "size": 10, "weight": 400, "ls": 0, "lines": 2}
    lines = wrap("AAAA BBBB CCCC", two, 54)  # each word 24px, space 6px
    expect(len(lines) == 2, "greedy Latin wrap breaks at spaces (got %d)" % len(lines))
    zh = wrap("一二三四五六七八九十", dict(cjk12, lines=3), 48)
    expect(len(zh) == 3 and zh[0][0] == "一二三四", "CJK wraps per character")
    kin = wrap("一二三，四五", dict(cjk12, lines=3), 36)
    expect(all(not l.startswith("，") for l, _ in kin), "kinsoku: no line starts with full-width comma")
    fixture = {
        "t.fits": {"zh": "已连接", "en": "CONNECTED", "fr": "", "maxWidthPx": 196,
                   "fit": {"font": "mono", "size": 10, "weight": 400, "ls": 0.1, "lines": 1}},
        "t.over": {"zh": "这是一条明显超过预算宽度的状态灯文案用来触发报警", "en": "THIS LABEL IS FAR TOO LONG FOR A PILL",
                   "fr": "", "maxWidthPx": 120, "fit": {"font": "mono", "size": 10, "weight": 400, "ls": 0.1, "lines": 1}},
        "t.multi": {"zh": "短句。", "en": "word " * 60, "fr": "", "maxWidthPx": 200,
                    "fit": {"font": "sans", "size": 11.5, "weight": 500, "ls": 0, "lines": 2}},
        "t.nofit": {"zh": "任意长度" * 50, "en": "no budget", "fr": "", "maxWidthPx": None},
        "t.emptyfr": {"zh": "短", "en": "short", "fr": "", "maxWidthPx": 50,
                      "fit": {"font": "sans", "size": 10, "weight": 400, "ls": 0, "lines": 1}},
    }
    res = {}
    for k, e in fixture.items():
        for lang in ("zh", "en", "fr"):
            r = check_entry(k, e, lang, 0.04)
            if r:
                res[(k, lang)] = r["ok"]
    expect(res.get(("t.fits", "zh")) is True and res.get(("t.fits", "en")) is True, "short pill fits")
    expect(res.get(("t.over", "zh")) is False and res.get(("t.over", "en")) is False, "long pill flagged")
    expect(res.get(("t.multi", "zh")) is True and res.get(("t.multi", "en")) is False, "2-line budget enforced")
    expect(("t.nofit", "zh") not in res, "entries without budget are skipped")
    expect(("t.emptyfr", "fr") not in res, "empty language value is skipped")
    if fails:
        for f in fails:
            print("自测失败：" + f)
        return 1
    print("自测通过（%d 项）" % 18)
    return 0


def main(argv):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--strings", help="合并后词条的 JSON 文件（缺省让 node 现算）")
    ap.add_argument("--budget", default=os.path.join(ROOT, "web", "shared", "text-budget.json"))
    ap.add_argument("--lang", action="append", help="逗号分隔，可重复（缺省 zh,en,fr）")
    ap.add_argument("--margin", type=float, default=0.04, help="安全余量（缺省 0.04，即 4%%）")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    langs = []
    for item in args.lang or ["zh,en,fr"]:
        langs.extend(x.strip() for x in item.split(",") if x.strip())
    for lang in langs:
        if lang not in ("zh", "en", "fr"):
            print("未知语言：" + lang)
            return 2
    return run(args.strings, args.budget, langs, args.margin, args.verbose)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
