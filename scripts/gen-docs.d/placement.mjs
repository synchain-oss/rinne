// SPDX-License-Identifier: GPL-3.0-or-later
//
// 放置示意（单源）：真源是 docs/media/placement.txt，一张 ASCII 画。渲染时包一层不带 info string 的代码围栏，
// 中英两版的块内容逐字相同（画里只写英文，中文说明写在图下的普通段落里）。
//
// 等宽判据放在本模块的 validate 里（structure 检查不动）：
//   - 框体行（以 + 或 | 开头）的左竖线在第 0 列；
//   - 所有框体行的框右边界在同一列 C；
//   - 顶行与底行长度 = C + 1；
//   - 画里只有可打印 ASCII：宽字符混进来会让按字符数判的对齐在视觉上错位却仍通过；
//   - 框体之外的自由行（标题行、脚注行）与右边界之外的标注列不参与等宽判定。
// 原图右侧有一列自由文本标注，整行长度 66…85 不等，所以「每行等宽」按整行算不成立；本模块按上面的口径判，
// 也不写死 C：说明文字里写过 49 列，原图实测 48 列，写死会误判。
//
// C 从上下边框行（整行只有 + 与 -）取：这种行的右端 + 位置唯一，不会和右侧标注里的字符混淆。
// 没有边框行时退到「所有框体行都有的最右框字符列」。
//
// 真源缺失时 validate 返回空数组（跳过），文件存在但框体不齐时报告是第几行的框右边界在第几列。

import { readRepoText } from "../lib/docs-kit.mjs";
import { caseResult } from "../lib/check-kit.mjs";

export const SOURCE_FILE = "docs/media/placement.txt";

/** 判据名 → 说明。 */
export const RULES = {
    "frame-present": "画里要有框体行",
    "frame-left": "框体行的左竖线在第 0 列",
    "frame-right": "所有框体行的框右边界在同一列",
    "frame-edge": "顶行与底行的长度等于框右边界列号加一",
    "ascii-only": "画里只有可打印 ASCII",
};

/** 换行统一成 LF：CRLF 检出的 \r 不是画的一部分，不能让 ascii-only 把每一行都判成不可打印，也不能漏进生成的围栏。 */
const lf = (text) => text.replace(/\r\n?/g, "\n");

/** 框体行：去掉行首空白后以 + 或 | 开头。 */
const FRAME = /^[+|]/;
/** 上下边框行：整行只有 + 与 -，且以 + 收尾（这种行的右端 + 位置唯一）。 */
const BORDER = /^\+[-+]*\+$/;

/**
 * 一份画里每个框体行的边界。列号一律按整行的列号算。
 * left 是行首空白之后的第一个框字符所在的列；cols 是所有框字符（+ 与 |）的列号，升序。
 */
export function frameBounds(text) {
    const out = [];
    text.split("\n").forEach((raw, i) => {
        const trimmed = raw.replace(/\s+$/, "");
        const body = trimmed.replace(/^[ \t]+/, "");
        if (!FRAME.test(body)) return;
        const left = trimmed.length - body.length;
        const cols = [];
        for (let k = 0; k < trimmed.length; k++) if (trimmed[k] === "|" || trimmed[k] === "+") cols.push(k);
        out.push({ line: i + 1, left, cols, text: trimmed, border: BORDER.test(body) });
    });
    return out;
}

/**
 * 框右边界所在的列 C。优先取边框行的右端 +（唯一），没有边框行时取所有框体行都有的最右框字符列。
 * 找不到时返回 null。
 */
export function rightColumn(frames) {
    const borders = frames.filter((f) => f.border);
    if (borders.length) return Math.max(...borders.map((f) => f.text.length - 1));
    const common = frames[0].cols.filter((c) => frames.every((f) => f.cols.includes(c)));
    return common.length ? Math.max(...common) : null;
}

/**
 * 一行自己的框右边界：从 C 往左找最近的框字符列（框内还有竖线与折角，右边界是最右的那一个）。
 * 找不到时返回 -1。
 */
function ownBoundary(f, right) {
    let best = -1;
    for (const c of f.cols) if (c <= right && c > best) best = c;
    return best;
}

/**
 * 等宽判据。返回问题字符串数组（空数组 = 齐）。每条问题以 [判据名] 开头。
 * 口径：框体行左竖线在第 0 列；所有框体行的框右边界在同一列 C；顶行与底行长度 = C + 1；全是可打印 ASCII。
 */
export function placementProblems(raw) {
    const text = lf(raw);
    const out = [];
    text.split("\n").forEach((line, i) => {
        const bad = [...line].find((ch) => ch < " " || ch > "~");
        if (bad !== undefined)
            out.push(
                `[ascii-only] 放置图第 ${i + 1} 行有不是可打印 ASCII 的字符（U+${bad.codePointAt(0).toString(16).toUpperCase().padStart(4, "0")}）`,
            );
    });
    const frames = frameBounds(text);
    if (!frames.length) return [...out, "[frame-present] 放置图里没有框体行（以 + 或 | 开头的行）"];
    for (const f of frames)
        if (f.left !== 0) out.push(`[frame-left] 放置图第 ${f.line} 行的框左边界在第 ${f.left} 列，应在第 0 列`);
    const right = rightColumn(frames);
    if (right === null) return [...out, "[frame-right] 放置图里找不到所有框体行共有的框右边界"];
    for (const f of frames) {
        const own = ownBoundary(f, right);
        if (own !== right)
            out.push(`[frame-right] 放置图第 ${f.line} 行的框右边界在第 ${own} 列，与其他行的第 ${right} 列不一致`);
    }
    // 顶行与底行是框体的上下边界，长度必须正好是 C + 1（列 0 到列 C）
    for (const f of frames.filter((x) => x.border))
        if (f.text.length !== right + 1)
            out.push(
                `[frame-edge] 放置图第 ${f.line} 行的顶/底边长度是 ${f.text.length}，应为 ${right + 1}（框右边界在第 ${right} 列）`,
            );
    return out;
}

/** 渲染出的块正文：包一层不带 info string 的代码围栏。 */
export function renderPlacement(raw) {
    const body = lf(raw).replace(/\n+$/, "");
    return `\`\`\`\n${body}\n\`\`\``;
}

/** 问题串里的判据名。 */
export const ruleOf = (problem) => /^\[([^\]]+)\]/.exec(problem)?.[1] ?? "";

/** 一份齐的画：框体宽 48 列，右侧标注列长短不一（这正是不能按整行等宽判的原因）。 */
const GOOD = [
    "One mixer channel - inserts (Cubase shown)",
    "+----------------------------------------------+",
    "| 1   Rinne Input   <--- fed back after D --+  |   return end: mixes the loop back in",
    "| 2   any plugin                            |  |   inside the loop",
    "| ..  ...                                   |  |",
    "| n   Rinne Output  ------------------------+  |   send end: last pre-fader slot",
    "|----------- pre/post-fader divider -----------|",
    "|     post-fader inserts                       |   outside the loop",
    "+----------------------------------------------+",
    "    fader / pan / sends                            outside the loop",
].join("\n");

/**
 * 自测用例：每个用例 { label, bad, passed, expect }。期望为空表示正例（一条问题都不许有），
 * 否则是坏样例（期望的判据都要被点名）。末尾的覆盖用例核对每条判据都被某个通过的坏样例点名：
 * 删掉任一判据，点名它的坏样例就变红。
 */
export function selfTestCases() {
    const cases = [];
    const add = (label, text, expect) => {
        const rules = placementProblems(text).map(ruleOf).filter(Boolean);
        const bad = expect.length > 0;
        cases.push({ label, bad, passed: bad ? expect.every((r) => rules.includes(r)) : rules.length === 0, expect });
    };
    add("齐的画没有问题", GOOD, []);
    add("标注列长短不一的自由行不算不齐", GOOD.replace("outside the loop\n+", "outside the loop, still fine\n+"), []);
    add(
        "右边框少一列",
        GOOD.replace(
            "| 2   any plugin                            |  |",
            "| 2   any plugin                            | |",
        ),
        ["frame-right"],
    );
    add(
        "右边框多一列",
        GOOD.replace(
            "| ..  ...                                   |  |",
            "| ..  ...                                   |   |",
        ),
        ["frame-right"],
    );
    add("有一行缩进", GOOD.replace("| ..  ...", "  | ..  ..."), ["frame-left"]);
    add(
        "顶行比框体短",
        GOOD.replace(
            "+----------------------------------------------+",
            "+--------------------------------------------+",
        ),
        ["frame-edge"],
    );
    add("没有框体行", "One mixer channel - inserts (Cubase shown)\n", ["frame-present"]);
    add("画里混进了宽字符", GOOD.replace("any plugin", "any plugin →"), ["ascii-only"]);
    add("画里混进了制表符", GOOD.replace("| ..  ...", "|\t..  ..."), ["ascii-only"]);
    const crlf = GOOD.replace(/\n/g, "\r\n");
    add("CRLF 换行的画与 LF 的画一样没有问题", crlf, []);
    add("CRLF 换行的画里混进宽字符照样判红", crlf.replace("any plugin", "any plugin →"), ["ascii-only"]);
    const fenced = renderPlacement(GOOD);
    cases.push({
        label: "渲染出的块是不带 info string 的围栏，围栏里逐字是真源",
        bad: false,
        passed: fenced.startsWith("```\n") && fenced.endsWith("\n```") && fenced.includes(GOOD),
        expect: [],
    });
    cases.push({
        label: "CRLF 的画渲染出的块没有回车符，与 LF 的画逐字相同",
        bad: false,
        passed: renderPlacement(crlf) === fenced && !renderPlacement(crlf).includes("\r"),
        expect: [],
    });
    const unnamed = Object.keys(RULES).filter((r) => !cases.some((c) => c.passed && c.expect.includes(r)));
    cases.push({
        label: `每条判据都有坏样例点名${unnamed.length ? `（没有：${unnamed.join("、")}）` : ""}`,
        bad: false,
        passed: unnamed.length === 0,
        expect: [],
    });
    return cases;
}

export const selfTest = () => caseResult(selfTestCases());

export default {
    name: "placement",
    sources: [SOURCE_FILE],
    surfaces: [
        { file: "README.md", id: "en", required: false },
        { file: "README.zh-CN.md", id: "zh", required: false },
    ],
    validate(ctx) {
        const text = readRepoText(ctx.root, SOURCE_FILE);
        if (text === null) return []; // 真源缺失：跳过，不报问题
        return placementProblems(text);
    },
    render(ctx) {
        const text = readRepoText(ctx.root, SOURCE_FILE);
        if (text === null) throw new Error(`缺 ${SOURCE_FILE}`);
        return renderPlacement(text);
    },
    selfTest,
};
