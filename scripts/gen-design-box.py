# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/web.json
"""gen-design-box.py：从 web/shared/design-box.js 生成 src/core/util/DesignBox.h。

设计盒常量的唯一真源是 web/shared/design-box.js；C++ 侧的头文件由本脚本生成，
任何位置都不得二次硬编码这些数字。同一份 design-box.js 永远生成逐字节相同的头文件。

design-box.js 被约束为「单个对象字面量加纯数字」，所以这里不执行 JS，
去掉行注释后把字面量改写成 JSON 再解析。

用法：
    python scripts/gen-design-box.py            生成 src/core/util/DesignBox.h
    python scripts/gen-design-box.py --check    与已提交的头文件对拍，不一致退出 1
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "web" / "shared" / "design-box.js"
TARGET = ROOT / "src" / "core" / "util" / "DesignBox.h"

ROLES = ("input", "output")


def parse_design(text):
    """从 design-box.js 全文解析出 DESIGN 字面量，返回嵌套的 dict。形状不符就抛 ValueError。"""
    body = re.sub(r"//[^\n]*", "", text)
    m = re.search(r"export\s+const\s+DESIGN\s*=\s*(\{.*\})\s*;", body, re.S)
    if not m:
        raise ValueError("没有找到 export const DESIGN = { ... };")
    literal = m.group(1)
    if re.search(r"[\"'`()=+*/]|=>|\bfunction\b", literal):
        raise ValueError("DESIGN 必须是纯数字字面量，不能含字符串、运算或函数")
    literal = re.sub(r"([A-Za-z_]\w*)\s*:", r'"\1":', literal)
    literal = re.sub(r",\s*([}\]])", r"\1", literal)
    try:
        return json.loads(literal)
    except json.JSONDecodeError as e:
        raise ValueError("DESIGN 无法解析：%s" % e)


def validate(design):
    for role in ROLES:
        box = design.get(role)
        if not isinstance(box, dict) or set(box) != {"w", "h", "presets"}:
            raise ValueError("DESIGN.%s 必须恰有 w、h、presets" % role)
        for key in ("w", "h"):
            if not isinstance(box[key], int) or box[key] <= 0:
                raise ValueError("DESIGN.%s.%s 必须是正整数" % (role, key))
        presets = box["presets"]
        if not presets or any(not isinstance(v, (int, float)) or v <= 0 for v in presets):
            raise ValueError("DESIGN.%s.presets 必须是非空的正数数组" % role)
        if any(a >= b for a, b in zip(presets, presets[1:])):
            raise ValueError("DESIGN.%s.presets 必须严格递增" % role)
        if 1 not in presets:
            raise ValueError("DESIGN.%s.presets 必须含 1（1x 档）" % role)
    frame = design.get("hostFrame")
    if not isinstance(frame, dict) or set(frame) != {"w", "h"}:
        raise ValueError("DESIGN.hostFrame 必须恰有 w、h")
    for key in ("w", "h"):
        if not isinstance(frame[key], int) or frame[key] < 0:
            raise ValueError("DESIGN.hostFrame.%s 必须是非负整数" % key)
    if set(design) != set(ROLES) | {"hostFrame"}:
        raise ValueError("DESIGN 只能有 input、output、hostFrame 三项")
    return design


def number(v):
    """数组元素按 JS 里的写法回写：整数不带小数点。"""
    return repr(v) if isinstance(v, float) and not v.is_integer() else str(int(v))


def render(design):
    lines = [
        # 分开拼：整行写在源码里会被 REUSE 当成本文件的许可声明，而行尾的引号和逗号会让它解析失败
        "// SPDX-License-" "Identifier: GPL-3.0-or-later",
        "// 本文件由 scripts/gen-design-box.py 从 web/shared/design-box.js 生成，请勿手工编辑。",
        "// 改值只改 design-box.js，再重跑：python scripts/gen-design-box.py",
        "#pragma once",
        "",
        "#include <array>",
        "",
        "namespace rinne::design",
        "{",
    ]
    for role in ROLES:
        box = design[role]
        name = role.capitalize()
        values = ", ".join(number(v) for v in box["presets"])
        lines += [
            "// %s 设计盒（1x）：%d×%d，%d 档缩放。" % (name, box["w"], box["h"], len(box["presets"])),
            "inline constexpr int k%sDesignW = %d;" % (name, box["w"]),
            "inline constexpr int k%sDesignH = %d;" % (name, box["h"]),
            "inline constexpr int k%sPresetCount = %d;" % (name, len(box["presets"])),
            "inline constexpr std::array<double, %d> k%sPresets = {%s};" % (len(box["presets"]), name, values),
            "",
        ]
    frame = design["hostFrame"]
    lines += [
        "// 宿主给插件窗加的框（标题栏加工具条）的估计值，用来判断某个缩放档位放不放得下。",
        "inline constexpr int kHostFrameW = %d;" % frame["w"],
        "inline constexpr int kHostFrameH = %d;" % frame["h"],
        "} // namespace rinne::design",
        "",
    ]
    return "\n".join(lines)


def main(argv):
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")
    if not SOURCE.exists():
        print("gen-design-box：找不到 %s" % SOURCE.relative_to(ROOT), file=sys.stderr)
        return 1
    try:
        generated = render(validate(parse_design(SOURCE.read_text(encoding="utf-8"))))
    except ValueError as e:
        print("gen-design-box：%s" % e, file=sys.stderr)
        return 1
    if "--check" in argv:
        current = TARGET.read_text(encoding="utf-8") if TARGET.exists() else ""
        if current == generated:
            print("gen-design-box --check：%s 与 design-box.js 一致" % TARGET.relative_to(ROOT).as_posix())
            return 0
        print("gen-design-box --check：%s 与 design-box.js 不一致，请重跑生成器" % TARGET.relative_to(ROOT).as_posix(), file=sys.stderr)
        return 1
    TARGET.parent.mkdir(parents=True, exist_ok=True)
    TARGET.write_text(generated, encoding="utf-8", newline="\n")
    print("gen-design-box：已生成 %s" % TARGET.relative_to(ROOT).as_posix())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
