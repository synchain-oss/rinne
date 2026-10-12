// SPDX-License-Identifier: GPL-3.0-or-later
//
// 跟踪路径不得只差大小写（docs-truth 的一部分）。Windows 与 mac 的文件系统不区分大小写：Src/ 与 src/、
// Scripts/hooks/x 与 scripts/hooks/x 在那里是同一个目录或文件，检出时一份会悄悄盖掉另一份，按路径定档与认定基础设施
// 的规则也会被绕开。逐个比较全部跟踪路径与它们的每一级目录前缀，统一成 NFC 再小写后相同、写法却不同即判红；
// 上一级目录已经冲突时只报上一级。

import { caseResult, trackedFiles } from "../lib/check-kit.mjs";

/** paths：仓内相对路径（正斜杠）。返回冲突说明，例如 "Src 与 src"。 */
export function caseCollisions(paths) {
    const spellings = new Map();
    for (const p of paths) {
        const parts = p.split("/");
        for (let i = 1; i <= parts.length; i++) {
            const prefix = parts.slice(0, i).join("/");
            const key = prefix.normalize("NFC").toLowerCase();
            if (!spellings.has(key)) spellings.set(key, new Set());
            spellings.get(key).add(prefix);
        }
    }
    const clash = new Set([...spellings].filter(([, s]) => s.size > 1).map(([k]) => k));
    const parent = (k) => k.slice(0, Math.max(0, k.lastIndexOf("/")));
    return [...clash].filter((k) => !clash.has(parent(k))).map((k) => [...spellings.get(k)].sort().join(" 与 "));
}

export function selfTest() {
    const cases = [
        [
            "互不冲突的路径",
            false,
            ["src/core/a.cpp", "tests/core/a.cpp", "README.md", "docs/README.md", "docs/adr/a.md"],
        ],
        ["TESTS/ 与 tests/", true, ["tests/core/a.cpp", "TESTS/core/b.cpp"]],
        ["Src/ 与 src/ 下的不同文件", true, ["src/core/a.cpp", "Src/core/evil.cpp"]],
        [
            "Scripts/hooks/ 与 scripts/hooks/ 下同名的文件",
            true,
            ["scripts/hooks/pre-push.mjs", "Scripts/hooks/pre-push.mjs"],
        ],
        ["只差大小写的两个文件", true, ["README.md", "readme.md"]],
        ["深层目录只差大小写", true, ["docs/adr/a.md", "docs/ADR/b.md"]],
    ].map(([label, bad, paths]) => {
        const hits = caseCollisions(paths);
        return { label, bad, passed: bad ? hits.length > 0 : hits.length === 0 };
    });
    // 上一级冲突时只报上一级，不把下面每一层都列一遍
    const one = caseCollisions(["src/core/a.cpp", "Src/core/b.cpp"]);
    cases.push({ label: "只报最上面冲突的那一级", bad: false, passed: one.length === 1 && one[0] === "Src 与 src" });
    return caseResult(cases);
}

export default {
    name: "path-case",
    group: "docs",
    inputs: [],
    run(ctx) {
        const hits = caseCollisions(trackedFiles(ctx.root));
        return { ok: hits.length === 0, lines: hits.map((h) => `跟踪路径只差大小写：${h}`) };
    },
    selfTest,
};
