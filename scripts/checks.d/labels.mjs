// SPDX-License-Identifier: GPL-3.0-or-later
//
// 标签检查（docs-truth 的一部分）。.github/labels.json 是标签的唯一真源，scripts/sync-labels.mjs 按它同步到 GitHub：
//   - labels.json 本身：名字不重复（GitHub 按大小写不敏感比较）、≤ 50 字符，颜色是 6 位十六进制，说明 ≤ 100 字符；
//   - 引用到的标签都已登记：workflow 与本地 action（labels.*.name 比较、gh 的 --label / --add-label / --remove-label、
//     labels: 键）、dependabot.yml 与 issue 模板的 labels:。没登记的标签会被 dependabot 或 gh 自己建出来，真源就失效了。
//     含 ${{ }} 或 $ 的引用是运行时才定的值，不查。

import fs from "node:fs";
import path from "node:path";
import { caseResult } from "../lib/check-kit.mjs";
import { workflowFiles } from "./workflows.mjs";

/** labels.json 的格式问题。 */
export function validateLabels(json) {
    const errors = [];
    const list = json?.labels;
    if (!Array.isArray(list) || !list.length) return ["labels.json 要有非空的 labels 数组"];
    const seen = new Set();
    for (const [i, l] of list.entries()) {
        const at = `labels[${i}]`;
        if (typeof l?.name !== "string" || !l.name.trim()) {
            errors.push(`${at} 缺 name`);
            continue;
        }
        const key = l.name.toLowerCase();
        if (seen.has(key)) errors.push(`${at} 名字重复（不分大小写）：${l.name}`);
        seen.add(key);
        if (l.name.length > 50 || l.name !== l.name.trim())
            errors.push(`${at} 名字超过 50 字符或首尾有空白：${l.name}`);
        if (!/^[0-9a-f]{6}$/.test(l.color || "")) errors.push(`${at} 颜色要写成 6 位小写十六进制：${l.name}`);
        if (typeof l.description !== "string" || l.description.length > 100) {
            errors.push(`${at} 说明要有，且不超过 100 字符：${l.name}`);
        }
    }
    return errors;
}

const clean = (v) =>
    v
        .trim()
        .replace(/^(["'])(.*)\1$/, "$2")
        .trim();
const dynamic = (v) => v.includes("$");

/** 一个文件里引用的标签：[{ label, line }]。 */
export function labelRefs(text) {
    const out = [];
    const add = (raw, line) => {
        for (const part of raw.split(",")) {
            const v = clean(part);
            if (v && !dynamic(v)) out.push({ label: v, line });
        }
    };
    const lines = text.split(/\r?\n/);
    lines.forEach((l, i) => {
        const no = i + 1;
        for (const m of l.matchAll(/labels\.\*\.name\s*,\s*'([^']+)'/g)) add(m[1], no);
        for (const m of l.matchAll(/label\.name\s*[!=]=\s*'([^']+)'/g)) add(m[1], no);
        for (const m of l.matchAll(/--(?:add-|remove-)?label(?:=|\s+)("[^"]*"|'[^']*'|[^\s"'\\]+)/g)) add(m[1], no);
        const key = /^(\s*)(?:-\s+)?labels:\s*(.*)$/.exec(l);
        if (!key) return;
        const value = key[2].replace(/\s+#.*$/, "").trim();
        if (value.startsWith("[")) add(value.replace(/^\[|\]$/g, ""), no);
        else if (value && !/^[|>]/.test(value)) add(value, no);
        else {
            // 块式列表：缩进更深的 `- 名字` 行
            for (let k = i + 1; k < lines.length; k++) {
                const next = lines[k];
                if (!next.trim() || next.trim().startsWith("#")) continue;
                if (next.match(/^ */)[0].length <= key[1].length && !/^\s*-\s/.test(next)) break;
                const item = /^(\s*)-\s+(.+?)(?:\s+#.*)?$/.exec(next);
                if (!item || item[1].length < key[1].length) break;
                add(item[2], k + 1);
            }
        }
    });
    return out;
}

/** 要核对的文件：workflow 与本地 action、dependabot、issue 模板（仓内相对路径）。 */
export function referencingFiles(root) {
    const out = [...workflowFiles(root)];
    if (fs.existsSync(path.join(root, ".github", "dependabot.yml"))) out.push(".github/dependabot.yml");
    const tpl = path.join(root, ".github", "ISSUE_TEMPLATE");
    if (fs.existsSync(tpl)) {
        for (const f of fs.readdirSync(tpl).sort()) if (/\.ya?ml$/.test(f)) out.push(`.github/ISSUE_TEMPLATE/${f}`);
    }
    return out;
}

/** 引用了却没登记的标签。files：[{ rel, text }]。 */
export function unknownRefs(files, labels) {
    const known = new Set(labels.map((l) => l.name));
    return files.flatMap(({ rel, text }) =>
        labelRefs(text)
            .filter((r) => !known.has(r.label))
            .map((r) => `${rel}:${r.line} 引用的标签「${r.label}」不在 .github/labels.json 里`),
    );
}

/**
 * 同步计划（纯函数，sync-labels.mjs 用）。名字按大小写不敏感匹配：大小写不同就改名。
 * 返回 { create: [label], update: [{ from, to }], extra: [label] }。
 */
export function planLabels(desired, current) {
    const byKey = new Map(current.map((l) => [l.name.toLowerCase(), l]));
    const create = [];
    const update = [];
    for (const d of desired) {
        const c = byKey.get(d.name.toLowerCase());
        if (!c) create.push(d);
        else if (c.name !== d.name || c.color.toLowerCase() !== d.color || (c.description || "") !== d.description) {
            update.push({ from: c, to: d });
        }
    }
    const wanted = new Set(desired.map((d) => d.name.toLowerCase()));
    const extra = current.filter((c) => !wanted.has(c.name.toLowerCase()));
    return { create, update, extra };
}

const LABELS = [
    { name: "bug", color: "d73a4a", description: "缺陷" },
    { name: "dependencies", color: "0366d6", description: "依赖更新" },
];

export default {
    name: "labels",
    group: "docs",
    inputs: [],
    run(ctx) {
        const file = path.join(ctx.root, ".github", "labels.json");
        if (!fs.existsSync(file)) return { ok: false, lines: ["缺 .github/labels.json"] };
        let json;
        try {
            json = JSON.parse(fs.readFileSync(file, "utf8"));
        } catch (e) {
            return { ok: false, lines: [`.github/labels.json 不是合法 JSON：${e.message}`] };
        }
        const errors = validateLabels(json);
        if (errors.length) return { ok: false, lines: errors };
        const files = referencingFiles(ctx.root).map((rel) => ({
            rel,
            text: fs.readFileSync(path.join(ctx.root, rel), "utf8"),
        }));
        const unknown = unknownRefs(files, json.labels);
        if (unknown.length) return { ok: false, lines: unknown };
        const refs = files.reduce((n, f) => n + labelRefs(f.text).length, 0);
        return { ok: true, lines: [`${json.labels.length} 个标签；${files.length} 个文件里 ${refs} 处引用都已登记`] };
    },
    selfTest() {
        const cases = [];
        const t = (label, bad, errors) => cases.push({ label, bad, passed: bad ? errors.length > 0 : !errors.length });
        const v = (labels) => validateLabels({ labels });
        t("合格的 labels.json", false, v(LABELS));
        t("名字只差大小写也算重复", true, v([...LABELS, { name: "Bug", color: "000000", description: "" }]));
        t("颜色带 #", true, v([{ name: "x", color: "#d73a4a", description: "" }]));
        t("说明超过 100 字符", true, v([{ name: "x", color: "d73a4a", description: "长".repeat(101) }]));
        t("缺 labels 数组", true, validateLabels({}));
        const refs = (text) => unknownRefs([{ rel: "f.yml", text }], LABELS);
        const wf = (line) => `jobs:\n  a:\n    steps:\n      - run: ${line}\n`;
        t(
            "workflow 按已登记的标签判断",
            false,
            refs("    if: contains(github.event.pull_request.labels.*.name, 'bug')"),
        );
        t(
            "workflow 按没登记的标签判断",
            true,
            refs("    if: contains(github.event.pull_request.labels.*.name, 'ci-skip')"),
        );
        t("label.name 比较没登记的标签", true, refs("    if: github.event.label.name == 'release'"));
        t("gh --add-label 没登记的标签", true, refs(wf('gh pr edit "$PR" --add-label "needs-review"')));
        t("gh --label 逗号列表里有一个没登记", true, refs(wf("gh issue create --label bug,triage")));
        t("gh --label 已登记", false, refs(wf("gh issue create --label bug")));
        t("运行时才定的标签不查", false, refs(wf('gh pr edit "$PR" --add-label "$LABEL"')));
        const dependabot = (items) => `updates:\n  - package-ecosystem: github-actions\n    labels:\n${items}`;
        t("dependabot 块式列表已登记", false, refs(dependabot("      - dependencies\n    schedule:\n")));
        t("dependabot 块式列表有没登记的", true, refs(dependabot("      - dependencies\n      - ci # 注释\n")));
        t("issue 模板行内列表有没登记的", true, refs('name: x\nlabels: ["bug", "triage"]\nbody: []\n'));
        t("issue 模板行内列表已登记", false, refs("name: x\nlabels: [bug]\n"));
        const p = planLabels(
            [
                { name: "bug", color: "d73a4a", description: "缺陷" },
                { name: "external", color: "c5def5", description: "外部贡献" },
                { name: "Backlog", color: "ededed", description: "" },
            ],
            [
                { name: "bug", color: "D73A4A", description: "缺陷" },
                { name: "backlog", color: "ededed", description: "" },
                { name: "wontfix", color: "ffffff", description: "x" },
            ],
        );
        const plan = [
            p.create.length === 1 && p.create[0].name === "external",
            p.update.length === 1 && p.update[0].from.name === "backlog" && p.update[0].to.name === "Backlog",
            p.extra.length === 1 && p.extra[0].name === "wontfix",
        ];
        t("同步计划：新建、改名、多余各一项，颜色大小写不算差异", false, plan.every(Boolean) ? [] : ["计划不对"]);
        const same = planLabels(LABELS, LABELS);
        t("同步计划：一致时无事可做", false, same.create.length + same.update.length + same.extra.length ? ["x"] : []);
        const changed = planLabels(LABELS, [{ ...LABELS[0], description: "旧说明" }, LABELS[1]]);
        t("同步计划：说明不同要改", true, changed.update.length ? ["要改"] : []);
        return caseResult(cases);
    },
};
