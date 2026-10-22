// SPDX-License-Identifier: GPL-3.0-or-later
//
// 强声明登记与证据（docs-truth 的一部分）。数据是 docs/claims.json：每条声明登记它的 strong 短语（肯定式的强断言）、
// hedge 短语（免责或「按设计」这类限定语）、evidence（证据）与 pending（证据产物尚未到位）。
//
// 判据：
//   claims-shape        claims.json 是合法 JSON；每条有 id / strong / hedge / evidence；id 唯一且为小写连字符；strong 非空
//   evidence-kind       每条 evidence / pending 的类型是 validation: / issue: / ci: / golden: 之一，形态可解析
//   evidence-satisfied  evidence 里的每一条都成立：validation 文件存在；issue 是 https URL；ci 是仓内存在的 job 名；
//                       golden 的谓词为真（目标文件不存在时按 pending 处理，不在这里判红）
//   pending-listed      证据产物尚不存在时允许直述，并把待办列出来（INFO 行，不判红）
//   strong-needs-hedge  一条声明既无成立的 evidence 也无 pending 时，它的 strong 短语只能出现在同一句里含 hedge 短语的句子上
//   unregistered-absolute 用户面文本里出现绝对化强断言句式、又没有被任何已登记声明的 strong 覆盖时判红
//
// 判据名见 RULES，报错行以 [名字] 开头。自测的每个坏样例写明它必须触发哪几条判据，并核对每条判据都至少被一个坏样例
// 点名：删掉任何一条判据，点名它的坏样例就变红。
//
// golden 的两种形状都认：映射形（顶层键就是参数 id）与数组形（{"params":[{"id":…}]}）。
// 同一条谓词路径（如 feedback.default）在两种形状下必须取到同一个值，否则参数真源换个形状，
// 同一条强声明就会无端判红。params-table 块模块认的是同一套形状。

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { ROOT, caseResult, readText } from "../lib/check-kit.mjs";

export const CLAIMS_FILE = "docs/claims.json";

/** 判据名 → 说明。 */
export const RULES = {
    "claims-shape": "claims.json 的形状：id 唯一且为小写连字符，每条有 id / strong / hedge / evidence，strong 非空",
    "evidence-kind": "每条 evidence / pending 的类型与形态可解析（validation: / issue: / ci: / golden:）",
    "evidence-satisfied": "evidence 里的每一条都成立",
    "pending-listed": "证据产物尚未到位时列出待办（INFO，不判红）",
    "strong-needs-hedge": "既无成立 evidence 也无 pending 的声明，strong 短语只能出现在含 hedge 的同一句里",
    "unregistered-absolute": "绝对化强断言必须被某条已登记声明的 strong 覆盖",
};

/** 扫描范围：用户面文本（web/i18n/ 下的词条文件在运行时扫全部 .js）。 */
export const SCOPE = [
    "README.md",
    "README.zh-CN.md",
    "docs/USER_GUIDE.md",
    "docs/USER_GUIDE.zh-CN.md",
    "docs/INSTALL.md",
    "docs/KNOWN_ISSUES.md",
    "docs/DAW_COMPATIBILITY.md",
    "docs/hard-rules.i18n.json",
];

/** 绝对化强断言的句式（只拦肯定式；含否定与免责的句子放行）。 */
export const ABSOLUTE_PATTERNS = [
    /\bdoes not change with\b/i,
    /\bnever changes\b/i,
    /\bis identical to\b/i,
    /\bidentical output\b/i,
    /\balways the same output\b/i,
    /\bzero[- ]latency\b/i,
    /\bguarantees\b/i,
    /\bsupports? all DAWs?\b/i,
    /不随[^。\n]{0,20}变化/,
    /完全一致/,
    /始终一致/,
    /零延迟/,
    /保证[^。\n]{0,20}(?:不变|一致)/,
];

/** 否定与免责标记：同句里有它们的强断言不算命中。 */
const NEGATION = [/\bnot\b/i, /\bno\b/i, /\bnever\b/i, /\bcannot\b/i, /\bcan't\b/i, /\bwithout\b/i, /不/, /没有/, /无/];

/**
 * 句子切分：句号、问号、感叹号、分号、换行都算一句的边界；英文的句点后跟空白也算。
 * 「同一句」是 strong-needs-hedge 的判据，所以切分口径要与 wording 检查一致。
 */
export function sentences(text) {
    return text
        .replace(/```[\s\S]*?```/g, " ")
        .split(/(?<=[。！？!?;；\n])|(?<=\.)[ \t]+/)
        .map((s) => s.trim())
        .filter(Boolean);
}

/** 一句话里有没有否定或免责标记。 */
export function hasNegation(sentence) {
    return NEGATION.some((re) => re.test(sentence));
}

/** 声明的 id 形态：小写连字符。 */
const ID_RE = /^[a-z0-9]+(-[a-z0-9]+)*$/;

/** 短语按正则匹配（写坏的正则退回逐字匹配）。 */
export function matchPhrase(phrase, text) {
    if (typeof phrase !== "string" || !phrase) return false;
    try {
        return new RegExp(phrase, "i").test(text);
    } catch {
        return text.includes(phrase);
    }
}

/** 把一条 evidence / pending 拆成 { kind, body }；形态不对时返回 null。 */
export function parseEvidence(item) {
    if (typeof item !== "string") return null;
    const i = item.indexOf(":");
    if (i < 0) return null;
    const kind = item.slice(0, i);
    const body = item.slice(i + 1);
    if (!body) return null;
    if (kind === "validation") return /^(?:docs|tests)\//.test(body) ? { kind, body } : null;
    if (kind === "issue") return /^https:\/\/\S+$/.test(body) ? { kind, body } : null;
    if (kind === "ci") return /^[a-z0-9][a-z0-9-]*$/.test(body) ? { kind, body } : null;
    if (kind === "golden") return /^[^#\s]+\.json#\S+\s+(?:==|!=|>=|<=|>|<)\s+\S+$/.test(body) ? { kind, body } : null;
    return null;
}

/** golden 谓词求值：body 是 "<file>#<path> <op> <value>"。返回 true / false / null（文件不在）。 */
export function evalGolden(root, body) {
    const [file, rest] = body.split("#");
    const m = /^(.*?)\s+(==|!=|>=|<=|>|<)\s+(\S+)$/.exec(rest ?? "");
    if (!m) return null;
    const [, pathExpr, op, raw] = m;
    const text = readText(path.join(root, file));
    if (text === null) return null;
    let data;
    try {
        data = JSON.parse(text);
    } catch {
        return false;
    }
    const value = resolvePath(data, pathExpr) ?? resolveParamPath(data, pathExpr);
    if (value === undefined) return false;
    const want = coerce(raw, value);
    switch (op) {
        case "==":
            return value === want;
        case "!=":
            return value !== want;
        case ">":
            return typeof value === "number" && typeof want === "number" && value > want;
        case "<":
            return typeof value === "number" && typeof want === "number" && value < want;
        case ">=":
            return typeof value === "number" && typeof want === "number" && value >= want;
        case "<=":
            return typeof value === "number" && typeof want === "number" && value <= want;
        default:
            return false;
    }
}
/** 点路径取值：a.b[0].c 与 a.b.0.c 都支持。 */
function resolvePath(data, expr) {
    const parts = expr
        .replace(/\[(\d+)\]/g, ".$1")
        .split(".")
        .filter(Boolean);
    let cur = data;
    for (const p of parts) {
        if (cur === null || cur === undefined || typeof cur !== "object") return undefined;
        cur = cur[p];
    }
    return cur;
}

/**
 * 参数集合里按 id 取一条参数。
 * golden 有两种形状：映射形（顶层键就是参数 id）与数组形（{"params":[{"id":…}]}）。
 * 两种形状下同一条谓词路径（如 feedback.default）都要取到同一个值，
 * 否则真源换个形状，同一条强声明就会无端判红。
 */
export function findParam(data, id) {
    if (!data || typeof data !== "object") return null;
    const direct = data[id];
    if (direct && typeof direct === "object" && !Array.isArray(direct)) return direct;
    const list = Array.isArray(data.params)
        ? data.params
        : Array.isArray(data.parameters)
          ? data.parameters
          : Object.entries(data)
                .filter(([, v]) => v && typeof v === "object" && !Array.isArray(v))
                .map(([k, v]) => ({ id: k, ...v }));
    const hit = list.find((p) => p && typeof p === "object" && p.id === id);
    return hit ?? null;
}

/** 谓词路径的第一段是参数 id 时，改走参数集合（两种 golden 形状都认）。 */
function resolveParamPath(data, expr) {
    const dot = expr.indexOf(".");
    if (dot <= 0) return undefined;
    const id = expr.slice(0, dot);
    const rest = expr.slice(dot + 1);
    const param = findParam(data, id);
    if (!param) return undefined;
    return resolvePath(param, rest);
}

/** 把字面量转成与实际值同类的值。 */
function coerce(raw, sample) {
    if (typeof sample === "number") {
        const n = Number(raw);
        return Number.isNaN(n) ? raw : n;
    }
    if (typeof sample === "boolean") return raw === "true";
    return raw;
}

/** 仓内 .github/workflows 里的 job 名。 */
export function ciJobs(root) {
    const dir = path.join(root, ".github", "workflows");
    const jobs = new Set();
    let files = [];
    try {
        files = fs.readdirSync(dir).filter((f) => f.endsWith(".yml"));
    } catch {
        return jobs;
    }
    for (const file of files) {
        const text = readText(path.join(dir, file)) ?? "";
        let inJobs = false;
        for (const line of text.split("\n")) {
            if (/^jobs:\s*$/.test(line)) {
                inJobs = true;
                continue;
            }
            if (!inJobs) continue;
            if (/^\S/.test(line)) break; // 出了 jobs 段
            const m = /^ {2}([A-Za-z0-9_-]+):\s*$/.exec(line);
            if (m) jobs.add(m[1]);
        }
    }
    return jobs;
}

/** 一条 evidence 是否成立。返回 "ok" / "missing"（产物不在）/ "false"（谓词不成立）/ "malformed"。 */
export function evidenceState(root, item, jobs) {
    const e = parseEvidence(item);
    if (!e) return "malformed";
    if (e.kind === "validation") return fs.existsSync(path.join(root, e.body)) ? "ok" : "missing";
    if (e.kind === "issue") return "ok";
    if (e.kind === "ci") return jobs.has(e.body) ? "ok" : "false";
    if (e.kind === "golden") {
        const v = evalGolden(root, e.body);
        if (v === null) return "missing";
        return v ? "ok" : "false";
    }
    return "malformed";
}

/**
 * 判据的实现。data 是 claims.json 解析后的对象；files 是 { 路径: 文本 }；root 是仓根（golden 与 ci 查表用）。
 * 返回 { errors, infos }；errors 判红，infos 只列出待办。
 */
export function claimProblems({ data, files, root, jobs = ciJobs(root) }) {
    const errors = [];
    const infos = [];
    const add = (rule, message) => {
        if (!Object.hasOwn(RULES, rule)) throw new Error(`未登记的判据 ${rule}`);
        errors.push(`[${rule}] ${message}`);
    };
    const note = (rule, message) => {
        if (!Object.hasOwn(RULES, rule)) throw new Error(`未登记的判据 ${rule}`);
        infos.push(`[${rule}] ${message}`);
    };

    if (!data || typeof data !== "object" || !Array.isArray(data.claims)) {
        add("claims-shape", `${CLAIMS_FILE} 缺 claims 数组`);
        return { errors, infos };
    }
    const ids = new Set();
    for (const c of data.claims) {
        const where = c?.id ?? "(没有 id)";
        if (typeof c?.id !== "string" || !ID_RE.test(c.id))
            add("claims-shape", `${CLAIMS_FILE} 的条目 id「${where}」不是小写连字符`);
        else if (ids.has(c.id)) add("claims-shape", `${CLAIMS_FILE} 的条目 id「${c.id}」重复`);
        else ids.add(c.id);
        if (!Array.isArray(c?.strong) || !c.strong.length)
            add("claims-shape", `${CLAIMS_FILE} 的条目「${where}」缺 strong 短语`);
        if (!Array.isArray(c?.hedge)) add("claims-shape", `${CLAIMS_FILE} 的条目「${where}」缺 hedge 短语`);
        if (!Array.isArray(c?.evidence)) add("claims-shape", `${CLAIMS_FILE} 的条目「${where}」缺 evidence 数组`);
    }

    // 证据形态与成立情况
    const covered = [];
    for (const c of data.claims) {
        const id = c?.id ?? "(没有 id)";
        if (!Array.isArray(c?.strong) || !Array.isArray(c?.evidence)) continue;
        let hasProof = false;
        for (const item of c.evidence) {
            const state = evidenceState(root, item, jobs);
            if (state === "malformed")
                add("evidence-kind", `${CLAIMS_FILE} 的条目「${id}」的 evidence「${item}」形态不认识`);
            else if (state === "missing")
                add("evidence-satisfied", `${CLAIMS_FILE} 的条目「${id}」的 evidence「${item}」指向的产物不在仓内`);
            else if (state === "false")
                add(
                    "evidence-satisfied",
                    `${CLAIMS_FILE} 的条目「${id}」的 evidence「${item}」不成立，` +
                        `对应的强声明「${c.strong.join("」「")}」判红`,
                );
            else hasProof = true;
        }
        const pendings = Array.isArray(c.pending) ? c.pending : [];
        for (const item of pendings) {
            const state = evidenceState(root, item, jobs);
            if (state === "malformed")
                add("evidence-kind", `${CLAIMS_FILE} 的条目「${id}」的 pending「${item}」形态不认识`);
            else if (state === "missing") note("pending-listed", `待办：${id} 的证据「${item}」尚未到位`);
            else if (state === "false")
                // 产物已经在仓内但谓词不成立：这条声明被证据反驳，判红（统筹裁定 A）
                add(
                    "evidence-satisfied",
                    `${CLAIMS_FILE} 的条目「${id}」的 pending「${item}」指向的产物已在仓内但不成立，` +
                        `对应的强声明「${c.strong.join("」「")}」判红`,
                );
            else note("pending-listed", `待办：${id} 的 pending「${item}」已经成立，请把它移到 evidence`);
        }
        covered.push({ id, phrases: c.strong, hedges: c.hedge ?? [], hasProof: hasProof || pendings.length > 0, c });
    }

    // strong-needs-hedge：既无成立 evidence 也无 pending 的声明
    for (const { id, phrases, hedges, hasProof, c } of covered) {
        if (hasProof) continue;
        const scope = c?.scope;
        for (const [file, text] of Object.entries(files)) {
            if (Array.isArray(scope) && scope.length && !scope.includes(file)) continue;
            for (const s of sentences(text)) {
                for (const phrase of phrases) {
                    if (!matchPhrase(phrase, s)) continue;
                    if (!hedges.some((h) => matchPhrase(h, s)))
                        add(
                            "strong-needs-hedge",
                            `${file} 里的强声明「${phrase}」既没有成立的 evidence 也没有 pending，同一句里要有 hedge 短语`,
                        );
                }
            }
        }
    }

    // unregistered-absolute：绝对化句式没有被任何声明的 strong 覆盖
    for (const [file, text] of Object.entries(files)) {
        for (const s of sentences(text)) {
            for (const re of ABSOLUTE_PATTERNS) {
                const m = re.exec(s);
                if (!m) continue;
                // 免责标记要在这句里、且不在命中片段自己身上（命中片段自带否定词的不算免责）
                const rest = s.slice(0, m.index) + " " + s.slice(m.index + m[0].length);
                if (hasNegation(rest)) continue;
                // 声明的 scope 限定它管哪些文件；scope 外的文件不算这条声明覆盖到了
                const covering = covered.filter((c) => {
                    const sc = c.c?.scope;
                    return !(Array.isArray(sc) && sc.length) || sc.includes(file);
                });
                if (!covering.some((c) => c.phrases.some((p) => matchPhrase(p, s))))
                    add("unregistered-absolute", `${file} 里的绝对化强断言「${m[0]}」没有登记在 ${CLAIMS_FILE}`);
            }
        }
    }
    return { errors, infos };
}

/** 扫描范围内的用户面文本。 */
export function scopedFiles(root) {
    const files = {};
    for (const rel of SCOPE) {
        const t = readText(path.join(root, rel));
        if (t !== null) files[rel] = t;
    }
    const i18n = path.join(root, "web", "i18n");
    try {
        for (const f of fs
            .readdirSync(i18n)
            .filter((x) => x.endsWith(".js"))
            .sort())
            files[`web/i18n/${f}`] = readText(path.join(i18n, f)) ?? "";
    } catch {
        // 没有 web/i18n 目录时只扫上面的固定清单
    }
    return files;
}

export async function selfTest(root = ROOT) {
    /**
     * 一个带 golden 的临时目录：golden 里的 feedback.default 可改。
     * shape 是 "map"（顶层键就是参数 id）或 "list"（{"params":[{"id":…}]}）——
     * 两种形状都要让同一条谓词路径取到同一个值，否则真源换个形状强声明就会无端判红。
     */
    const withGolden = (value, shape = "map") => {
        const dir = fs.mkdtempSync(path.join(os.tmpdir(), "rinne-claims-"));
        fs.mkdirSync(path.join(dir, "tests", "golden"), { recursive: true });
        const body =
            shape === "list"
                ? { params: [{ id: "feedback", default: value, min: 0, max: 100 }] }
                : { feedback: { default: value } };
        fs.writeFileSync(path.join(dir, "tests", "golden", "params-input.json"), JSON.stringify(body, null, 2));
        return dir;
    };
    const baseClaims = {
        version: 1,
        claims: [
            {
                id: "safe-defaults",
                scope: ["README.md"],
                strong: ["starts with feedback at zero"],
                hedge: ["designed so"],
                evidence: [],
                pending: ["golden:tests/golden/params-input.json#feedback.default == 0"],
            },
            {
                id: "auto-protection",
                scope: ["README.md"],
                strong: ["auto-protection"],
                hedge: ["designed so"],
                evidence: ["ci:core-sanitizers"],
                pending: [],
            },
        ],
    };
    const files = {
        "README.md": "Rinne starts with feedback at zero and has a limiter.\n",
        "README.zh-CN.md": "Rinne 默认反馈为 0。\n",
    };
    const jobs = new Set(["core-sanitizers"]);

    const run = (over = {}) =>
        claimProblems({
            data: over.data ?? baseClaims,
            files: over.files ?? files,
            root: over.root ?? root,
            jobs: over.jobs ?? jobs,
        });
    /** 规则名 → 触发它的行（errors 与 infos 合在一起看）。 */
    const fired = (result) => [...result.errors, ...result.infos];

    const golden05 = withGolden(0.5);
    const golden0 = withGolden(0);
    const golden05List = withGolden(0.5, "list");
    const golden0List = withGolden(0, "list");
    const goldenMissing = fs.mkdtempSync(path.join(os.tmpdir(), "rinne-claims-empty-"));
    let cases;
    try {
        cases = [
            ["登记完整、证据成立", [], run()],
            [
                "golden 里的默认值改成 0.5",
                ["evidence-satisfied"],
                run({
                    root: golden05,
                    data: {
                        ...baseClaims,
                        claims: [
                            {
                                ...baseClaims.claims[0],
                                pending: [],
                                evidence: ["golden:tests/golden/params-input.json#feedback.default == 0"],
                            },
                        ],
                    },
                }),
            ],
            [
                "golden 是数组形时默认值改成 0.5",
                ["evidence-satisfied"],
                run({
                    root: golden05List,
                    data: {
                        ...baseClaims,
                        claims: [
                            {
                                ...baseClaims.claims[0],
                                pending: [],
                                evidence: ["golden:tests/golden/params-input.json#feedback.default == 0"],
                            },
                        ],
                    },
                }),
            ],
            [
                "golden 是数组形时默认值是 0",
                [],
                run({
                    root: golden0List,
                    data: {
                        ...baseClaims,
                        claims: [
                            {
                                ...baseClaims.claims[0],
                                pending: [],
                                evidence: ["golden:tests/golden/params-input.json#feedback.default == 0"],
                            },
                        ],
                    },
                }),
            ],
            [
                "golden 里的默认值是 0",
                [],
                run({
                    root: golden0,
                    data: {
                        ...baseClaims,
                        claims: [
                            {
                                ...baseClaims.claims[0],
                                pending: [],
                                evidence: ["golden:tests/golden/params-input.json#feedback.default == 0"],
                            },
                        ],
                    },
                }),
            ],
            ["证据产物还没进仓时列为待办", ["pending-listed"], run({ root: goldenMissing })],
            [
                "pending 的产物已在仓内但谓词不成立时判红",
                ["evidence-satisfied"],
                run({
                    root: golden05,
                    data: {
                        ...baseClaims,
                        claims: [
                            {
                                ...baseClaims.claims[0],
                                pending: ["golden:tests/golden/params-input.json#feedback.default == 0"],
                                evidence: [],
                            },
                        ],
                    },
                }),
            ],
            [
                "claims.json 不是合法形状",
                ["claims-shape"],
                run({ data: { version: 1, claims: [{ id: "Bad Id", strong: [] }] } }),
            ],
            [
                "id 重复",
                ["claims-shape"],
                run({
                    data: {
                        ...baseClaims,
                        claims: [baseClaims.claims[0], { ...baseClaims.claims[0], scope: [] }],
                    },
                }),
            ],
            [
                "evidence 的类型不认识",
                ["evidence-kind"],
                run({
                    data: {
                        ...baseClaims,
                        claims: [{ ...baseClaims.claims[1], evidence: ["wiki:something"] }],
                    },
                }),
            ],
            [
                "ci 证据指向不存在的 job",
                ["evidence-satisfied"],
                run({
                    data: {
                        ...baseClaims,
                        claims: [{ ...baseClaims.claims[1], evidence: ["ci:no-such-job"] }],
                    },
                }),
            ],
            [
                "强声明既无 evidence 也无 pending，且同句没有 hedge",
                ["strong-needs-hedge"],
                run({
                    data: {
                        ...baseClaims,
                        claims: [{ ...baseClaims.claims[0], pending: [], evidence: [] }],
                    },
                }),
            ],
            [
                "绝对化强断言没有登记",
                ["unregistered-absolute"],
                run({
                    data: { version: 1, claims: [baseClaims.claims[1]] },
                    files: { "README.md": "The loop delay is identical output every time.\n" },
                }),
            ],
            [
                "声明的 scope 之外的文件不算被覆盖",
                ["unregistered-absolute"],
                run({
                    data: { version: 1, claims: [baseClaims.claims[1]] },
                    files: { "docs/OTHER.md": "The loop delay is identical output every time.\n" },
                }),
            ],
            [
                "含否定的绝对化句式放行",
                [],
                run({
                    data: { version: 1, claims: [baseClaims.claims[1]] },
                    files: { "README.md": "No software can guarantee the loop will never howl.\n" },
                }),
            ],
            [
                "命中片段自带的否定词不算免责",
                ["unregistered-absolute"],
                run({
                    data: { version: 1, claims: [baseClaims.claims[1]] },
                    files: { "README.md": "环路延迟不随缓冲大小变化。\n" },
                }),
            ],
        ].map(([label, expect, result]) => {
            const lines = fired(result);
            const missed = expect.filter((r) => !lines.some((e) => e.startsWith(`[${r}] `)));
            return {
                label: missed.length ? `${label}（没有触发 ${missed.join("、")}）` : label,
                bad: !!expect.length,
                passed: expect.length ? missed.length === 0 : result.errors.length === 0,
                expect,
            };
        });
        // 卡验收：golden 的默认值改成 0.5 时，报错要指到「Feedback starts at zero」那句强声明
        const golden05Lines = fired(
            run({
                root: golden05,
                data: {
                    ...baseClaims,
                    claims: [
                        {
                            ...baseClaims.claims[0],
                            pending: [],
                            evidence: ["golden:tests/golden/params-input.json#feedback.default == 0"],
                        },
                    ],
                },
            }),
        );
        cases.push({
            label: "golden 谓词不成立时报错指到对应的强声明",
            bad: false,
            passed: golden05Lines.some((l) => l.includes("starts with feedback at zero")),
        });
    } finally {
        for (const d of [golden05, golden0, golden05List, golden0List, goldenMissing])
            fs.rmSync(d, { recursive: true, force: true });
    }
    const unnamed = Object.keys(RULES).filter((r) => !cases.some((c) => c.expect.includes(r)));
    cases.push({
        label: `每条判据都有坏样例点名${unnamed.length ? `（没有：${unnamed.join("、")}）` : ""}`,
        bad: false,
        passed: unnamed.length === 0,
    });
    return caseResult(cases);
}

export default {
    name: "claims",
    group: "docs",
    inputs: [],
    run(ctx) {
        const text = readText(path.join(ctx.root, CLAIMS_FILE));
        if (text === null) return { ok: false, lines: [`[claims-shape] 缺 ${CLAIMS_FILE}`] };
        let data;
        try {
            data = JSON.parse(text);
        } catch (e) {
            return { ok: false, lines: [`[claims-shape] ${CLAIMS_FILE} 不是合法 JSON：${e.message}`] };
        }
        const { errors, infos } = claimProblems({
            data,
            files: scopedFiles(ctx.root),
            root: ctx.root,
        });
        return { ok: errors.length === 0, lines: [...errors, ...infos] };
    },
    selfTest: (ctx) => selfTest(ctx.root),
};
