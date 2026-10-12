// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ci.json
//
// SPDX 头门禁：能写注释的源文件，前 5 行必须有 GPL-3.0-or-later 的 SPDX 许可标识。
// REUSE.toml 的兜底块会让 reuse lint 在缺头时照样通过，所以逐文件的头只能靠这里把关。
// 不查：逐字收录的第三方目录、golden 数据、设计稿归档（加头会破坏逐字节一致）。
//   node scripts/check-spdx.mjs [--self-test]

import path from "node:path";
import { ROOT, assertNodeVersion, caseResult, isMain, j, readText, trackedFiles } from "./lib/check-kit.mjs";

export const SOURCE =
    /\.(?:c|cc|cpp|h|hpp|mm|js|mjs|cjs|css|py|ps1|psm1|sh|cmake)$|(?:^|\/)CMakeLists\.txt$|^scripts\/hooks\/[^/.]+$/;
export const EXEMPT = /^(?:third_party|web\/js\/juce|tests\/golden|docs\/design)\//;
// 分开拼：完整的标识行写在源码里会被 reuse 当成本文件的许可声明
const HEADER = j("SPDX-License-", "Identifier: GPL-3.0-or-later");

/** files：仓内相对路径；read(rel) 返回文本或 null。没有任何候选文件时返回 null（多半是清单写坏了，判红）。 */
export function missingHeaders(files, read) {
    const cand = files.filter((f) => SOURCE.test(f) && !EXEMPT.test(f));
    if (!cand.length) return null;
    return cand.filter((f) => {
        const text = read(f);
        return text !== null && !text.split(/\r?\n/, 5).some((l) => l.includes(HEADER));
    });
}

export function checkTree(root = ROOT) {
    return missingHeaders(trackedFiles(root), (rel) => readText(path.join(root, rel)));
}

export function selfTest() {
    const files = {
        "src/a.cpp": `// ${HEADER}\nint a;\n`,
        "scripts/x.ps1": `#Requires -Version 7\n# ${HEADER}\n`,
        "scripts/hooks/pre-push": `#!/bin/sh\n# ${HEADER}\n`,
        "third_party/x/y.c": "int y;\n",
        "web/js/juce/index.js": "export {};\n",
        "README.md": "# x\n",
    };
    const read = (f) => files[f] ?? null;
    const run = (extra) => missingHeaders(Object.keys({ ...files, ...extra }), (f) => extra[f] ?? read(f));
    const cases = [
        ["有头的源文件与豁免目录", false, run({})],
        ["缺头的 C++", true, run({ "src/b.h": "#pragma once\n" })],
        ["头在第 6 行之后", true, run({ "src/c.mjs": `${"\n".repeat(5)}// ${HEADER}\n` })],
        ["缺头的 hook 外壳", true, run({ "scripts/hooks/commit-msg": "#!/bin/sh\n" })],
        ["别的许可标识", true, run({ "src/d.js": `// ${j("SPDX-License-", "Identifier: MIT")}\n` })],
        ["没有候选文件", true, missingHeaders(["README.md"], read)],
    ].map(([label, bad, missing]) => ({ label, bad, passed: bad ? missing?.length !== 0 : missing?.length === 0 }));
    return caseResult(cases);
}

function main(argv) {
    assertNodeVersion();
    if (argv.includes("--self-test")) {
        const r = selfTest();
        for (const l of r.lines) console.error(l);
        console.log(`check-spdx --self-test：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`);
        return r.ok ? 0 : 1;
    }
    const missing = checkTree();
    if (missing === null) {
        console.error("check-spdx 失败：没有找到任何需要 SPDX 头的源文件");
        return 1;
    }
    if (!missing.length) {
        console.log("check-spdx 通过");
        return 0;
    }
    console.error(`check-spdx 失败：${missing.length} 个文件前 5 行缺「${HEADER}」`);
    for (const f of missing) console.error(`  ${f}`);
    return 1;
}

if (isMain(import.meta.url)) process.exitCode = main(process.argv.slice(2));
