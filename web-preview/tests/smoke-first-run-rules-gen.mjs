// SPDX-License-Identifier: GPL-3.0-or-later
//
// 硬约束的两个界面落地面（Node，无浏览器）：gen-docs 的 hard-rules 块整份写出 web/shared/hard-rules.gen.js，
// 并写 web/i18n/guide.js 里的生成段。在临时目录里搭一份真源加落地面的夹具，逐项改坏，看 gen-docs 的检查有没有变红：
//   ① 夹具回写后全绿，仓内两个落地面与回写结果逐字节相同；
//   ② hard-rules.gen.js：每一行的首、中、尾字节各改一次，外加按固定步长取的字节，以及文件头尾增删一个字节，全都判红；
//   ③ guide.js：生成段（含两端标记）里同样取点改字节，全都判红；生成段之外的手写词条改了不归这个块管（对照组）；
//   ④ 规则版本：rulesVersion 不是正整数、缺失，或中文改了而 rulesVersionZhSha256 没跟上，都判红；
//      把哈希换成当前中文的合并哈希后转绿（只改措辞的路径）；fr 已审（reviewed）时，回写先要被拒绝（审校记录已过期），
//      夹具里的 reviewedZhSha256 也换成合并哈希（模拟重新审过）后才转绿；
//   ⑤ 形状：hard-rules.gen.js 首行之前、END 之后多出内容判红；guide.js 丢了标记判红；
//   ⑥ 上屏数据：RULES_VERSION 等于 hard-rules.i18n.json 的 rulesVersion；guide.js 摊开的 rules.rN.lead / body 与
//      hard-rules.gen.js 的 RULES 三语逐条相同。
//
// 用法：node web-preview/tests/smoke-first-run-rules-gen.mjs [仓库根]

import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { createRun, repoRoot } from "./lib/harness.mjs";

const ROOT = repoRoot();
const run = createRun("硬约束界面落地面");
const load = (rel) => import(pathToFileURL(path.join(ROOT, rel)).href);

const { checkAll, loadModules, writeAll } = await load("scripts/gen-docs.mjs");
const { I18N_FILE, INSTALL_FILE, SOURCE_FILE, loadSource, zhHashes } = await load("scripts/lib/docs-kit.mjs");
const { cleanupTempDir, makeTempDir } = await load("scripts/lib/safe-rm.mjs");
const block = await load("scripts/gen-docs.d/hard-rules.mjs");
const { WEB_FILE, GUIDE_FILE } = block;

// 用户指南（SOURCE_FILE）里还带着参数表的生成块，回写时 params-table 模块要读它的真源：golden 与 Input 词条表，一并放进夹具
const { GOLDEN_FILE: PARAMS_GOLDEN_FILE, I18N_FILE: PARAMS_I18N_FILE } = await load(
    "scripts/gen-docs.d/params-table.mjs",
);

const MODS = await loadModules();
const FILES = [SOURCE_FILE, I18N_FILE, INSTALL_FILE, WEB_FILE, GUIDE_FILE, PARAMS_GOLDEN_FILE, PARAMS_I18N_FILE];
const repoBytes = (rel) => fs.readFileSync(path.join(ROOT, rel));

/** 临时目录里放一份仓内的真源与落地面，回写一遍；fn 拿到 { dir, put, get, check }。 */
async function withFixture(fn) {
    const dir = makeTempDir("rinne-rules-gen-");
    try {
        const abs = (rel) => path.join(dir, rel);
        const put = (rel, data) => {
            fs.mkdirSync(path.dirname(abs(rel)), { recursive: true });
            fs.writeFileSync(abs(rel), data);
        };
        const get = (rel) => fs.readFileSync(abs(rel));
        for (const rel of FILES) put(rel, repoBytes(rel));
        await writeAll(dir, MODS);
        return await fn({ dir, put, get, check: () => checkAll(dir, MODS) });
    } finally {
        cleanupTempDir(dir);
    }
}

/** 第 i 个字节换成另一个值（低位取反；换出回车时再换一次，免得被读取时的换行统一吞掉）。 */
function flipByte(buf, i) {
    const out = Buffer.from(buf);
    out[i] ^= 0x01;
    if (out[i] === 0x0d) out[i] ^= 0x03;
    return out;
}

/** 要改的字节位置：每一行的首、中、尾，外加 [from, to) 里按步长取的点；去重排序。 */
function probePositions(buf, from, to, stride) {
    const set = new Set();
    let start = from;
    for (let i = from; i <= to; i++) {
        if (i === to || buf[i] === 0x0a) {
            const end = i; // 换行符本身也算这一行的尾字节
            for (const p of [start, Math.floor((start + end) / 2), Math.min(end, to - 1)])
                if (p >= from && p < to) set.add(p);
            start = i + 1;
        }
    }
    for (let i = from; i < to; i += stride) set.add(i);
    return [...set].sort((a, b) => a - b);
}

const mentions = (problems, rel) => problems.some((p) => p.includes(rel));

// ---------------------------------------------------------------- ① 回写后全绿

await withFixture(async ({ get, check }) => {
    run.eq(await check(), [], "回写后的夹具应全绿");
    for (const rel of [WEB_FILE, GUIDE_FILE])
        run.check(get(rel).equals(repoBytes(rel)), `仓内的 ${rel} 应与回写结果逐字节相同（改真源后没跑 --write？）`);
});

// ---------------------------------------------------------------- ② hard-rules.gen.js 任一字节

await withFixture(async ({ put, check }) => {
    const orig = repoBytes(WEB_FILE);
    const missed = [];
    const positions = probePositions(orig, 0, orig.length, 13);
    for (const i of positions) {
        put(WEB_FILE, flipByte(orig, i));
        if (!mentions(await check(), WEB_FILE)) missed.push(i);
    }
    const edits = [
        ["文件头多一个字节", Buffer.concat([Buffer.from(" "), orig])],
        ["文件尾多一个字节", Buffer.concat([orig, Buffer.from(" ")])],
        ["文件尾多一个换行", Buffer.concat([orig, Buffer.from("\n")])],
        ["删掉最后一个字节", orig.subarray(0, orig.length - 1)],
        ["删掉第一个字节", orig.subarray(1)],
    ];
    for (const [what, data] of edits) {
        put(WEB_FILE, data);
        run.check(mentions(await check(), WEB_FILE), `${WEB_FILE}：${what}应判红`);
    }
    put(WEB_FILE, orig);
    run.eq(await check(), [], "改回原样后转绿");
    run.check(positions.length > 400, `取点太少（${positions.length}）：取点函数没覆盖整份文件`);
    run.eq(missed.slice(0, 10), [], `${WEB_FILE} 里这些字节被改后检查仍是绿的（共 ${positions.length} 处取点）`);
});

// ---------------------------------------------------------------- ③ guide.js 生成段任一字节

await withFixture(async ({ put, check }) => {
    const orig = repoBytes(GUIDE_FILE);
    const text = orig.toString("utf8");
    const begin = Buffer.byteLength(text.slice(0, text.indexOf("<!-- BEGIN gen:hard-rules:guide -->")));
    const endMark = "<!-- END gen:hard-rules:guide -->";
    const end = Buffer.byteLength(text.slice(0, text.indexOf(endMark) + endMark.length));
    run.check(begin > 0 && end > begin, `${GUIDE_FILE} 应有成对的生成段标记`);
    const missed = [];
    const positions = probePositions(orig, begin, end, 13);
    for (const i of positions) {
        put(GUIDE_FILE, flipByte(orig, i));
        if (!mentions(await check(), GUIDE_FILE)) missed.push(i);
    }
    run.check(positions.length > 400, `取点太少（${positions.length}）`);
    run.eq(
        missed.slice(0, 10),
        [],
        `${GUIDE_FILE} 生成段里这些字节被改后检查仍是绿的（共 ${positions.length} 处取点）`,
    );

    // 对照组：手写词条不归这个块管，改了也不该被它报（否则上面的红可能只是「整份文件一改就红」的空转）
    const hand = '"rules.btn.skip": "跳过导览"';
    run.check(text.includes(hand), "对照组夹具找不到手写词条");
    put(GUIDE_FILE, text.replace(hand, '"rules.btn.skip": "跳过导"'));
    run.eq(await check(), [], "生成段之外的手写词条改了，hard-rules 块不报");
});

// ---------------------------------------------------------------- ④ 规则版本

const editJson = (buf, fn) => {
    const j = JSON.parse(buf.toString("utf8"));
    fn(j);
    return JSON.stringify(j, null, 2) + "\n";
};
const hasVersionProblem = (p) => p.some((x) => x.includes("rulesVersion"));

for (const [what, mutate] of [
    ["rulesVersion 为 0", (j) => (j.rulesVersion = 0)],
    ["rulesVersion 为小数", (j) => (j.rulesVersion = 1.5)],
    ["rulesVersion 是字符串", (j) => (j.rulesVersion = "1")],
    ["缺 rulesVersion", (j) => delete j.rulesVersion],
    ["rulesVersionZhSha256 不对", (j) => (j.rulesVersionZhSha256 = "0".repeat(64))],
]) {
    await withFixture(async ({ put, get, check }) => {
        put(I18N_FILE, editJson(get(I18N_FILE), mutate));
        run.check(hasVersionProblem(await check()), `${what}应判红`);
    });
}

await withFixture(async ({ dir, put, get, check }) => {
    // 只改中文措辞，同时把译文的 zhSha256 跟上：剩下的红只该来自规则版本
    const src = get(SOURCE_FILE).toString("utf8");
    run.check(src.includes("从 0 开始"), "夹具找不到第 2 条规则的引导词");
    put(SOURCE_FILE, src.replace("从 0 开始", "从零开始"));
    const h = zhHashes(loadSource(dir));
    put(
        I18N_FILE,
        editJson(get(I18N_FILE), (j) => j.rules.forEach((r, i) => (r.zhSha256 = h.rules[i]))),
    );
    const p = await check();
    run.check(hasVersionProblem(p), "中文改了、rulesVersionZhSha256 没跟上应判红");
    run.check(!p.some((x) => x.includes("译文过期")), "夹具应只剩版本的问题（译文哈希已跟上）");
    put(
        I18N_FILE,
        editJson(get(I18N_FILE), (j) => (j.rulesVersionZhSha256 = h.combined)),
    );
    if (JSON.parse(get(I18N_FILE).toString("utf8")).frReview?.status === "reviewed") {
        // fr 已审：中文一改，审校记录就过期，回写必须拦住；换成当前合并哈希（模拟重新审过）后才放行
        let err = null;
        try {
            await writeAll(dir, MODS);
        } catch (e) {
            err = e;
        }
        run.check(
            err !== null && String(err.message).includes("审校记录已过期"),
            "fr 已审、中文改了：回写应拒绝并报审校记录已过期",
        );
        put(
            I18N_FILE,
            editJson(get(I18N_FILE), (j) => (j.frReview.reviewedZhSha256 = h.combined)),
        );
    }
    await writeAll(dir, MODS);
    run.eq(await check(), [], "确认只是措辞、把哈希换成当前合并哈希并回写后转绿");
});

// ---------------------------------------------------------------- ⑤ 形状

{
    const good = repoBytes(WEB_FILE).toString("utf8");
    run.eq(block.webShapeProblems(good), [], "仓内的 hard-rules.gen.js 形状合格");
    run.eq(block.webShapeProblems(null), [], "没有 hard-rules.gen.js 时不报形状（缺文件由落地面状态管）");
    run.check(block.webShapeProblems("\n" + good).length > 0, "首行之前多一个换行应判红");
    run.check(block.webShapeProblems(good + "export const X = 1;\n").length > 0, "END 之后多出代码应判红");
    run.check(block.webShapeProblems(good.replace(block.WEB_TAIL, "")).length > 0, "丢了结尾标记应判红");
}

await withFixture(async ({ put, get, check }) => {
    const text = get(GUIDE_FILE).toString("utf8");
    const stripped = text.replace(/^.*gen:hard-rules:guide.*$/gm, "// 标记被删");
    put(GUIDE_FILE, stripped);
    run.check(mentions(await check(), GUIDE_FILE), "guide.js 丢了生成段标记应判红");
});

// ---------------------------------------------------------------- ⑥ 上屏数据

{
    const web = await load(WEB_FILE);
    const guide = (await load(GUIDE_FILE)).default;
    const i18n = JSON.parse(repoBytes(I18N_FILE).toString("utf8"));
    run.eq(web.RULES_VERSION, i18n.rulesVersion, "RULES_VERSION 等于 hard-rules.i18n.json 的 rulesVersion");
    run.eq(
        web.RULES.map((r) => r.id),
        i18n.rules.map((r) => r.id),
        "RULES 的 id 与顺序同 hard-rules.i18n.json",
    );
    for (const lang of ["zh", "en", "fr"])
        web.RULES.forEach((r, i) => {
            run.eq(guide[lang][`rules.r${i + 1}.lead`], r[lang].lead, `${lang} 第 ${i + 1} 条引导词`);
            run.eq(guide[lang][`rules.r${i + 1}.body`], r[lang].body, `${lang} 第 ${i + 1} 条正文`);
        });
    run.check(
        !Object.keys(guide.zh).some((k) => k === `rules.r${web.RULES.length + 1}.lead`),
        "guide.js 不该多出第 N+1 条规则",
    );
}

run.finish();
