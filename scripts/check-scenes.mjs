// SPDX-License-Identifier: GPL-3.0-or-later
//
// 场景集合对拍：预览的场景表（web-preview/mock/scenarios.js 的 SCENES）与设计稿的机读清单（#rinne-scenes）。
//   1. 场景表自身一致（恒执行）：ID 唯一、前缀与窗口对应（in- 对 input，out- 对 output，cat- 对 catalog）、语言合法、
//      mustShow 里的元件编号都在锚点表里且与窗口一致、整窗场景带外壳元件；
//   2. 与设计稿对拍（设计稿入库后生效）：ID 集合相等；每个场景的窗口与语言相同；设计稿的 elements 覆盖场景表的 mustShow；
//      设计盒尺寸与 web/shared/design-box.js 相同。
// 设计稿的位置：docs/design/ 下的 *.dc.html（读其中的 #rinne-scenes），或同目录的 manifest.json。
// docs/design/ 还不存在（设计稿尚未回收入库）时第 2 部分跳过，不判红；--design <文件> 可以指向仓外的设计稿做一次性对拍。
//   node scripts/check-scenes.mjs [--design <文件>] [--self-test]

import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { ROOT, assertNodeVersion, caseResult, isMain } from "./lib/check-kit.mjs";

const LANGS = ["zh", "en", "fr"];
const PREFIX = { in: "input", out: "output", cat: "catalog" };

/** 场景表自身的一致性。scenes 是 SCENES 数组，anchors 是元件编号表（对象）。 */
export function internalProblems(scenes, anchors) {
    const out = [];
    const seen = new Set();
    for (const s of scenes) {
        const id = String(s.id);
        if (seen.has(id)) out.push(`场景 ID 重复：${id}`);
        seen.add(id);
        const prefix = /^(in|out|cat)-[a-z0-9]+(?:-[a-z0-9]+)*$/.exec(id)?.[1];
        if (!prefix) out.push(`场景 ID 形状不对：${id}（应为 in- / out- / cat- 开头的小写连字符串）`);
        else if (PREFIX[prefix] !== s.role) out.push(`场景 ${id}：前缀对应 ${PREFIX[prefix]}，role 却是 ${s.role}`);
        if (!LANGS.includes(s.lang)) out.push(`场景 ${id}：语言 ${s.lang} 不合法`);
        if (!Array.isArray(s.mustShow)) {
            out.push(`场景 ${id}：mustShow 不是数组`);
            continue;
        }
        for (const code of s.mustShow) {
            if (!Object.prototype.hasOwnProperty.call(anchors, code))
                out.push(`场景 ${id}：mustShow 里的 ${code} 不在锚点表里`);
            else if (s.role === "input" && !code.startsWith("I-")) out.push(`场景 ${id}：Input 窗口里出现了 ${code}`);
            else if (s.role === "output" && !code.startsWith("O-")) out.push(`场景 ${id}：Output 窗口里出现了 ${code}`);
        }
        const shell = s.role === "input" ? "I-01" : s.role === "output" ? "O-01" : null;
        if (shell && !s.mustShow.includes(shell)) out.push(`场景 ${id}：整窗场景缺外壳 ${shell}`);
    }
    return out;
}

/** 与设计稿机读清单对拍。manifest 形如 { designBox, scenes: [{ id, window, lang, elements }] }。 */
export function compareManifest(scenes, manifest, design) {
    const out = [];
    const mine = new Map(scenes.map((s) => [s.id, s]));
    const theirs = new Map();
    for (const m of manifest.scenes || []) {
        if (theirs.has(m.id)) out.push(`设计稿清单里场景 ${m.id} 重复`);
        theirs.set(m.id, m);
    }
    for (const id of mine.keys()) if (!theirs.has(id)) out.push(`场景表有 ${id}，设计稿清单没有`);
    for (const id of theirs.keys()) if (!mine.has(id)) out.push(`设计稿清单有 ${id}，场景表没有`);
    for (const [id, s] of mine) {
        const m = theirs.get(id);
        if (!m) continue;
        if (m.window !== s.role) out.push(`场景 ${id}：设计稿窗口 ${m.window}，场景表 ${s.role}`);
        if (m.lang !== s.lang) out.push(`场景 ${id}：设计稿语言 ${m.lang}，场景表 ${s.lang}`);
        const have = new Set(m.elements || []);
        const lacking = s.mustShow.filter((c) => !have.has(c));
        if (lacking.length) out.push(`场景 ${id}：设计稿 elements 缺 ${lacking.join("、")}`);
    }
    for (const role of ["input", "output"]) {
        const got = manifest.designBox?.[role];
        const want = [design[role].w, design[role].h];
        if (JSON.stringify(got) !== JSON.stringify(want)) {
            out.push(`设计盒 ${role}：设计稿 ${JSON.stringify(got)}，design-box.js ${JSON.stringify(want)}`);
        }
    }
    return out;
}

/** 从设计稿 html 里取出 #rinne-scenes；取不到返回 null。 */
export function parseManifestHtml(html) {
    const m = /<script[^>]*\bid="rinne-scenes"[^>]*>([\s\S]*?)<\/script>/.exec(html);
    if (!m) return null;
    try {
        return JSON.parse(m[1]);
    } catch {
        return null;
    }
}

/** 设计稿位置：--design 给的文件，或 docs/design/ 下的 *.dc.html / manifest.json；没有返回 null。 */
export function locateDesign(root, explicit = null) {
    if (explicit) return path.resolve(explicit);
    const dir = path.join(root, "docs", "design");
    if (!fs.existsSync(dir)) return null;
    const files = fs.readdirSync(dir).sort();
    const hit = files.find((f) => f.endsWith(".dc.html")) || files.find((f) => f === "manifest.json");
    return hit ? path.join(dir, hit) : null;
}

/** 读设计稿清单；返回 { manifest } 或 { problem }。 */
export function readManifest(file) {
    const text = fs.readFileSync(file, "utf8");
    const manifest = file.endsWith(".json") ? JSON.parse(text) : parseManifestHtml(text);
    return manifest ? { manifest } : { problem: `${path.basename(file)}：读不到机读清单 #rinne-scenes` };
}

export async function checkTree(root = ROOT, explicitDesign = null) {
    const url = (p) => pathToFileURL(path.join(root, p)).href;
    const { SCENES } = await import(url("web-preview/mock/scenarios.js"));
    const { ELEMENT_ANCHORS } = await import(url("web-preview/mock/anchors.js"));
    const { DESIGN } = await import(url("web/shared/design-box.js"));
    const problems = internalProblems(SCENES, ELEMENT_ANCHORS);
    const file = locateDesign(root, explicitDesign);
    if (!file) return { problems, skip: "设计稿还没有入库（docs/design/ 不存在），只做场景表自身的一致性检查" };
    const r = readManifest(file);
    if (r.problem) problems.push(r.problem);
    else problems.push(...compareManifest(SCENES, r.manifest, DESIGN));
    return { problems, skip: null };
}

export function selfTest() {
    const anchors = { "I-01": "input.shell", "I-06": "input.group", "O-01": "output.shell", "O-05": "output.group" };
    const design = { input: { w: 800, h: 580 }, output: { w: 460, h: 364 } };
    const scenes = [
        { id: "in-a", role: "input", lang: "zh", mustShow: ["I-01", "I-06"] },
        { id: "out-a", role: "output", lang: "en", mustShow: ["O-01", "O-05"] },
        { id: "cat-pair", role: "catalog", lang: "zh", mustShow: ["I-06", "O-05"] },
    ];
    const manifest = () => ({
        designBox: { input: [800, 580], output: [460, 364] },
        scenes: scenes.map((s) => ({ id: s.id, window: s.role, lang: s.lang, elements: [...s.mustShow, "I-01"] })),
    });
    const cases = [];
    const add = (label, bad, problems) =>
        cases.push({ label, bad, passed: bad ? problems.length > 0 : problems.length === 0 });
    const withScenes = (fn) => {
        const copy = scenes.map((s) => ({ ...s, mustShow: [...s.mustShow] }));
        fn(copy);
        return internalProblems(copy, anchors);
    };
    const withManifest = (fn) => {
        const m = manifest();
        fn(m);
        return compareManifest(scenes, m, design);
    };

    add("一致的场景表", false, internalProblems(scenes, anchors));
    add("一致的设计稿清单", false, compareManifest(scenes, manifest(), design));
    add(
        "设计稿多出的 elements 不算差异",
        false,
        withManifest((m) => m.scenes[0].elements.push("I-17")),
    );
    add(
        "场景 ID 重复",
        true,
        withScenes((s) => s.push({ ...s[0] })),
    );
    add(
        "前缀与窗口对不上",
        true,
        withScenes((s) => (s[0].role = "output")),
    );
    add(
        "语言不合法",
        true,
        withScenes((s) => (s[0].lang = "de")),
    );
    add(
        "mustShow 里有锚点表没有的编号",
        true,
        withScenes((s) => s[0].mustShow.push("I-99")),
    );
    add(
        "Input 窗口里出现 Output 元件",
        true,
        withScenes((s) => s[0].mustShow.push("O-05")),
    );
    add(
        "整窗场景缺外壳",
        true,
        withScenes((s) => (s[1].mustShow = ["O-05"])),
    );
    add(
        "设计稿少一个场景",
        true,
        withManifest((m) => m.scenes.pop()),
    );
    add(
        "设计稿多一个场景",
        true,
        withManifest((m) => m.scenes.push({ id: "in-extra", window: "input", lang: "zh", elements: [] })),
    );
    add(
        "设计稿的窗口不同",
        true,
        withManifest((m) => (m.scenes[0].window = "output")),
    );
    add(
        "设计稿的语言不同",
        true,
        withManifest((m) => (m.scenes[1].lang = "fr")),
    );
    add(
        "设计稿 elements 缺 mustShow 里的元件",
        true,
        withManifest((m) => (m.scenes[0].elements = ["I-01"])),
    );
    add(
        "设计盒尺寸不同",
        true,
        withManifest((m) => (m.designBox.input = [800, 600])),
    );
    add(
        "清单里场景重复",
        true,
        withManifest((m) => m.scenes.push({ ...m.scenes[0] })),
    );
    add("读不到清单", true, parseManifestHtml("<html></html>") === null ? ["null"] : []);
    add(
        "从 html 读出清单",
        false,
        parseManifestHtml('<script type="application/json" id="rinne-scenes">{"scenes":[]}</script>') ? [] : ["读不出"],
    );
    return caseResult(cases);
}

if (isMain(import.meta.url)) {
    assertNodeVersion();
    if (process.argv.includes("--self-test")) {
        const r = selfTest();
        for (const l of r.lines) console.error(l);
        console.log(`check-scenes 自测：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`);
        process.exitCode = r.ok ? 0 : 1;
    } else {
        const i = process.argv.indexOf("--design");
        const { problems, skip } = await checkTree(ROOT, i >= 0 ? process.argv[i + 1] : null);
        for (const p of problems) console.error(`  ${p}`);
        if (skip) console.log(`check-scenes：${skip}`);
        console.log(`check-scenes：${problems.length ? `${problems.length} 处差异` : "通过"}`);
        process.exitCode = problems.length ? 1 : 0;
    }
}
