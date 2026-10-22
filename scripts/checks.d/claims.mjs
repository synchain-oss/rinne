// SPDX-License-Identifier: GPL-3.0-or-later
//
// 强声明登记与证据（docs-truth 的一部分）。数据是 docs/claims.json：每条声明登记它的 strong 短语（肯定式的强断言）、
// hedge 短语（免责或「按设计」这类限定语）、evidence（证据）与 pending（证据产物尚未到位）。
//
// 判据：
//   claims-shape        claims.json 是合法 JSON；每条有 id / strong / hedge / evidence；id 唯一且为小写连字符；strong 非空；
//                       strong / hedge 的每个短语都是能编译的、不会匹配任意句子的正则；scope 里的每一项都是扫描范围内的文件
//   evidence-kind       每条 evidence / pending 的类型是 validation: / issue: / ci: / golden: 之一，形态可解析；
//                       validation 的文件名通配（*）只允许写在 pending 里（记录的日期要等上机才知道）
//   evidence-satisfied  evidence 里的每一条都成立：validation 文件存在；issue 是 https URL；ci 是仓内存在的 job 名；
//                       golden 的谓词为真（目标文件不存在时按 pending 处理，不在这里判红）
//   pending-listed      证据产物尚不存在时允许直述，并把待办列出来（INFO 行，不判红）
//   strong-needs-hedge  一条声明既无成立的 evidence 也无 pending 时，它的 strong 短语只能出现在同一句里含 hedge 短语的句子上
//   unregistered-absolute 用户面文本里出现绝对化强断言句式、又没有被任何已登记声明的 strong 覆盖时判红
//
// 证据待到位的口径：pending 里的产物还没进仓时，直述允许、只列待办；产物已在仓内但谓词为假，说明声明被证据反驳，判红。
// 这样今天不会因为证据产物还没造出来就判红，产物一落地又立刻被它自己的数据约束。
//
// 判据名见 RULES，报错行以 [名字] 开头。自测的每个坏样例写明它必须触发哪几条判据，并核对每条判据都至少被一个坏样例
// 点名：删掉任何一条判据，点名它的坏样例就变红。
//
// golden 的两种形状都认：映射形（顶层键就是参数 id）与数组形（{"params":[{"id":…}]}）。
// 同一条谓词路径（如 feedback.default）在两种形状下必须取到同一个值，否则参数真源换个形状，
// 同一条强声明就会无端判红。params-table 块模块认的是同一套形状。
//
// 句子切分、否定与免责、引号放行、扫描范围都和 wording 检查共用同一份实现（从 wording.mjs 引入），不各抄一份。
// 参数表块模块（gen-docs.d/params-table.mjs）的自测也在这里跑：它认的参数形状就是上面谓词路径认的形状。

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { ROOT, caseResult, readText } from "../lib/check-kit.mjs";
import { selfTestCases as paramsTableCases } from "../gen-docs.d/params-table.mjs";
import { SCOPE, negatedAt, proseUnits, quotedSpans, scopedFiles, sentences as plainSentences } from "./wording.mjs";
import { parseWorkflow } from "./workflows.mjs";

export { SCOPE, scopedFiles };
export const CLAIMS_FILE = "docs/claims.json";

/** 判据名 → 说明。 */
export const RULES = {
    "claims-shape":
        "claims.json 的形状：id 唯一且为小写连字符，每条有 id / strong / hedge / evidence，strong 非空，短语是可用的正则，scope 在扫描范围内",
    "evidence-kind":
        "每条 evidence / pending 的类型与形态可解析（validation: / issue: / ci: / golden:），通配只允许 pending",
    "evidence-satisfied": "evidence 里的每一条都成立",
    "pending-listed": "证据产物尚未到位时列出待办（INFO，不判红）",
    "strong-needs-hedge": "既无成立 evidence 也无 pending 的声明，strong 短语只能出现在含 hedge 的同一句里",
    "unregistered-absolute": "绝对化强断言必须被某条已登记声明的 strong 覆盖",
};

/**
 * 绝对化强断言的句式（只拦肯定式；含否定与免责的句子放行）。
 * 「零延迟」「支持所有 DAW」这类由 wording 检查直接禁用，不可能被登记成声明，所以不在这里重复。
 */
export const ABSOLUTE_PATTERNS = [
    /\bdoes not change with\b/i,
    /\bnever changes\b/i,
    /\bis identical to\b/i,
    /\bidentical output\b/i,
    /\balways the same output\b/i,
    /\bguarantees\b/i,
    /不随[^。\n]{0,20}变化/,
    /完全一致/,
    /始终一致/,
    /保证[^。\n]{0,20}(?:不变|一致)/,
];

/** 句子切分：先去掉围栏代码块（命令不是声明），再按 wording 的口径切。 */
export function sentences(text) {
    return plainSentences(text.replace(/```[\s\S]*?```/g, " "));
}

/** 声明的 id 形态：小写连字符。 */
const ID_RE = /^[a-z0-9]+(-[a-z0-9]+)*$/;

/** 短语按正则匹配，大小写不敏感。写坏的正则直接抛错——悄悄退回逐字匹配会让一条声明永远不命中。 */
export function matchPhrase(phrase, text) {
    if (typeof phrase !== "string" || !phrase) throw new Error("短语不是非空字符串");
    return new RegExp(phrase, "i").test(text);
}

/** 用来测「过于宽泛」的探针句：任何正常的 strong / hedge 短语都不该匹配它们。 */
const PROBES = ["a", "x y", "The quick brown fox.", "这是一个句子。"];

/** 一个 strong / hedge 短语的问题；没问题返回 null。 */
export function phraseProblem(phrase) {
    if (typeof phrase !== "string" || !phrase.trim()) return "不是非空字符串";
    let re;
    try {
        re = new RegExp(phrase, "i");
    } catch (e) {
        return `不是能编译的正则（${e.message}）`;
    }
    if (re.test("")) return "能匹配空串，会吞掉所有句子";
    if (PROBES.some((t) => re.test(t))) return "过于宽泛，能匹配任意句子";
    return null;
}

/** scope 里的一项是否在扫描范围内（固定清单，或 web/i18n/ 下的词条文件）。 */
const inScope = (f) => typeof f === "string" && (SCOPE.includes(f) || /^web\/i18n\/[^/]+\.js$/.test(f));

/**
 * 把一条 evidence / pending 拆成 { kind, body }；形态不对时返回 null。
 * glob 为真时 validation 的文件名允许用 *（只在最后一段里、不跨目录），给还不知道日期的上机记录用。
 */
export function parseEvidence(item, { glob = false } = {}) {
    if (typeof item !== "string") return null;
    const i = item.indexOf(":");
    if (i < 0) return null;
    const kind = item.slice(0, i);
    const body = item.slice(i + 1);
    if (!body) return null;
    if (kind === "validation") {
        if (!/^(?:docs|tests)\//.test(body)) return null;
        if (!body.includes("*")) return { kind, body };
        const last = body.slice(body.lastIndexOf("/") + 1);
        const dir = body.slice(0, body.lastIndexOf("/"));
        return glob && !dir.includes("*") && last.includes("*") && !last.includes("**")
            ? { kind, body, glob: true }
            : null;
    }
    if (kind === "issue") return /^https:\/\/\S+$/.test(body) ? { kind, body } : null;
    if (kind === "ci") return /^[a-z0-9][a-z0-9_-]*$/.test(body) ? { kind, body } : null;
    if (kind === "golden") return /^[^#\s]+\.json#\S+\s+(?:==|!=|>=|<=|>|<)\s+\S+$/.test(body) ? { kind, body } : null;
    return null;
}

/** validation 记录是否存在；带 * 的按最后一段的文件名通配。 */
function validationExists(root, body, glob) {
    if (!glob) return fs.existsSync(path.join(root, body));
    const slash = body.lastIndexOf("/");
    const re = new RegExp(
        `^${body
            .slice(slash + 1)
            .split("*")
            .map((s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"))
            .join("[^/]*")}$`,
    );
    try {
        return fs.readdirSync(path.join(root, body.slice(0, slash))).some((f) => re.test(f));
    } catch {
        return false;
    }
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

/**
 * 把字面量转成与实际值同类的值。布尔值认 true / false 与 1 / 0：
 * 参数表把「开」存成 true 还是 1，同一条谓词（== 1）都要成立。
 */
function coerce(raw, sample) {
    if (typeof sample === "number") {
        const n = Number(raw);
        return Number.isNaN(n) ? raw : n;
    }
    if (typeof sample === "boolean") {
        if (raw === "true" || raw === "1") return true;
        if (raw === "false" || raw === "0") return false;
        return raw;
    }
    return raw;
}

/** 仓内 .github/workflows 里的 job 名（用 workflows 检查的解析器，口径与它一致）。 */
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
        const text = readText(path.join(dir, file));
        if (text === null) continue;
        for (const job of parseWorkflow(text).jobs) jobs.add(job.id);
    }
    return jobs;
}

/**
 * 一条 evidence 是否成立。返回 "ok" / "missing"（产物不在）/ "false"（谓词不成立）/ "malformed"。
 * glob 为真时 validation 允许文件名通配（只给 pending 用）。
 */
export function evidenceState(root, item, jobs, { glob = false } = {}) {
    const e = parseEvidence(item, { glob });
    if (!e) return "malformed";
    if (e.kind === "validation") return validationExists(root, e.body, e.glob) ? "ok" : "missing";
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
    const unusable = new Set(); // 短语写坏的声明不参与后面的匹配，免得把正则错误当成扫描结果
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
        for (const [field, list] of [
            ["strong", c?.strong],
            ["hedge", c?.hedge],
        ])
            for (const phrase of Array.isArray(list) ? list : []) {
                const why = phraseProblem(phrase);
                if (why) {
                    add("claims-shape", `${CLAIMS_FILE} 的条目「${where}」的 ${field} 短语「${phrase}」${why}`);
                    unusable.add(c);
                }
            }
        if (c?.scope !== undefined) {
            if (!Array.isArray(c.scope)) add("claims-shape", `${CLAIMS_FILE} 的条目「${where}」的 scope 不是数组`);
            else
                for (const f of c.scope)
                    if (!inScope(f))
                        add(
                            "claims-shape",
                            `${CLAIMS_FILE} 的条目「${where}」的 scope「${f}」不在扫描范围内，写错了文件名？`,
                        );
        }
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
            const state = evidenceState(root, item, jobs, { glob: true });
            if (state === "malformed")
                add("evidence-kind", `${CLAIMS_FILE} 的条目「${id}」的 pending「${item}」形态不认识`);
            else if (state === "missing") note("pending-listed", `待办：${id} 的证据「${item}」尚未到位`);
            else if (state === "false")
                // 产物已经在仓内但谓词不成立：这条声明被证据反驳，判红
                add(
                    "evidence-satisfied",
                    `${CLAIMS_FILE} 的条目「${id}」的 pending「${item}」指向的产物已在仓内但不成立，` +
                        `对应的强声明「${c.strong.join("」「")}」判红`,
                );
            else note("pending-listed", `待办：${id} 的 pending「${item}」已经成立，请把它移到 evidence`);
        }
        if (unusable.has(c)) continue;
        covered.push({ id, phrases: c.strong, hedges: c.hedge ?? [], hasProof: hasProof || pendings.length > 0, c });
    }

    /** 一个文件里的全部句子（JSON / JS 词条先取出字符串值）。解析失败的文件由 wording 的 scope-parse 报，这里按原文扫。 */
    const sentencesOf = (file, text) => {
        let units;
        try {
            units = proseUnits(file, text);
        } catch {
            units = [text];
        }
        return units.flatMap((u) => sentences(u));
    };

    // strong-needs-hedge：既无成立 evidence 也无 pending 的声明
    for (const { id, phrases, hedges, hasProof, c } of covered) {
        if (hasProof) continue;
        const scope = c?.scope;
        for (const [file, text] of Object.entries(files)) {
            if (Array.isArray(scope) && scope.length && !scope.includes(file)) continue;
            for (const s of sentencesOf(file, text)) {
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
        for (const s of sentencesOf(file, text)) {
            const spans = quotedSpans(s);
            for (const re of ABSOLUTE_PATTERNS) {
                const m = re.exec(s);
                if (!m) continue;
                // 引号里引用的句式、紧挨命中之前或之后的否定与免责：放行（口径与 wording 检查相同）
                if (spans.some(([a, b]) => m.index >= a && m.index < b)) continue;
                if (negatedAt(s, m.index, m[0].length)) continue;
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

/** 自测临时目录的前缀；删除前核对它。tiers 的自测也用它建目录、用 removeTemp 删。 */
export const TEMP_PREFIX = "rinne-claims-";

/**
 * 删除自测建的临时目录。递归删除前先断言：真实路径在系统临时目录之下、目录名带本自测的前缀，
 * 且不是盘根、用户目录或仓根。
 */
export function removeTemp(dir) {
    const real = fs.realpathSync.native(dir);
    const tmp = fs.realpathSync.native(os.tmpdir());
    const bad =
        !real.startsWith(tmp + path.sep) ||
        !path.basename(real).startsWith(TEMP_PREFIX) ||
        path.parse(real).root === real ||
        real === fs.realpathSync.native(os.homedir()) ||
        real === fs.realpathSync.native(ROOT);
    if (bad) throw new Error(`拒绝删除 ${real}：它不是本次自测新建的临时目录`);
    fs.rmSync(real, { recursive: true, force: true });
}

export async function selfTest(root = ROOT) {
    const temps = [];
    const mkTemp = () => {
        const d = fs.mkdtempSync(path.join(os.tmpdir(), TEMP_PREFIX));
        temps.push(d);
        return d;
    };
    /**
     * 一个带 golden 的临时目录：golden 里的 feedback.default 可改。
     * shape 是 "map"（顶层键就是参数 id）或 "list"（{"params":[{"id":…}]}）——
     * 两种形状都要让同一条谓词路径取到同一个值，否则真源换个形状强声明就会无端判红。
     * limiter 是另一个参数，用来测布尔值与 1 / 0 的两种写法。
     */
    const withGolden = (value, shape = "map", limiter = true) => {
        const dir = mkTemp();
        fs.mkdirSync(path.join(dir, "tests", "golden"), { recursive: true });
        const body =
            shape === "list"
                ? {
                      params: [
                          { id: "feedback", default: value, min: 0, max: 100 },
                          { id: "limiter", default: limiter },
                      ],
                  }
                : { feedback: { default: value }, limiter: { default: limiter } };
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
    const goldenPred = "golden:tests/golden/params-input.json#feedback.default == 0";
    const withClaim = (over) => ({ ...baseClaims, claims: [{ ...baseClaims.claims[0], ...over }] });
    const onlyAuto = { version: 1, claims: [baseClaims.claims[1]] };

    const run = (over = {}) =>
        claimProblems({
            data: over.data ?? baseClaims,
            files: over.files ?? files,
            root: over.root ?? root,
            jobs: over.jobs ?? jobs,
        });
    /** 规则名 → 触发它的行（errors 与 infos 合在一起看）。 */
    const fired = (result) => [...result.errors, ...result.infos];

    let cases;
    try {
        const golden05 = withGolden(0.5);
        const golden0 = withGolden(0);
        const golden05List = withGolden(0.5, "list");
        const golden0List = withGolden(0, "list");
        const goldenMissing = mkTemp();
        const goldenLimiterBool = withGolden(0, "map", true);
        const goldenLimiterOff = withGolden(0, "list", false);
        // 带一份 validation 记录的仓，用来测文件名通配
        const withRecord = mkTemp();
        fs.mkdirSync(path.join(withRecord, "docs", "validation"), { recursive: true });
        fs.writeFileSync(path.join(withRecord, "docs", "validation", "2026-11-02-cubase15-win.md"), "x\n");
        // 三种 workflow 写法：列 0 注释行夹在 jobs 下、job 键带行尾注释、下划线 job 名
        const wfRoot = mkTemp();
        fs.mkdirSync(path.join(wfRoot, ".github", "workflows"), { recursive: true });
        fs.writeFileSync(
            path.join(wfRoot, ".github", "workflows", "a.yml"),
            "name: x\non: push\njobs:\n  one:\n    runs-on: x\n# 列 0 的注释\n  two: # 行尾注释\n    runs-on: x\n\n  three_x:\n    runs-on: x\n",
        );
        const wfJobs = ciJobs(wfRoot);

        cases = [
            ["登记完整、证据成立", [], run()],
            [
                "golden 里的默认值改成 0.5",
                ["evidence-satisfied"],
                run({ root: golden05, data: withClaim({ pending: [], evidence: [goldenPred] }) }),
            ],
            [
                "golden 是数组形时默认值改成 0.5",
                ["evidence-satisfied"],
                run({ root: golden05List, data: withClaim({ pending: [], evidence: [goldenPred] }) }),
            ],
            [
                "golden 是数组形时默认值是 0",
                [],
                run({ root: golden0List, data: withClaim({ pending: [], evidence: [goldenPred] }) }),
            ],
            [
                "golden 里的默认值是 0",
                [],
                run({ root: golden0, data: withClaim({ pending: [], evidence: [goldenPred] }) }),
            ],
            [
                "布尔写法的 limiter 默认值满足 == 1",
                [],
                run({
                    root: goldenLimiterBool,
                    data: withClaim({
                        pending: [],
                        evidence: ["golden:tests/golden/params-input.json#limiter.default == 1"],
                    }),
                }),
            ],
            [
                "limiter 默认值是关时 == 1 不成立",
                ["evidence-satisfied"],
                run({
                    root: goldenLimiterOff,
                    data: withClaim({
                        pending: [],
                        evidence: ["golden:tests/golden/params-input.json#limiter.default == 1"],
                    }),
                }),
            ],
            ["证据产物还没进仓时列为待办", ["pending-listed"], run({ root: goldenMissing })],
            [
                "pending 的产物已在仓内但谓词不成立时判红",
                ["evidence-satisfied"],
                run({ root: golden05, data: withClaim({ pending: [goldenPred], evidence: [] }) }),
            ],
            [
                "pending 里的通配记录还没有对应文件：列为待办",
                ["pending-listed"],
                run({
                    root: goldenMissing,
                    data: withClaim({ pending: ["validation:docs/validation/*-cubase15-win.md"] }),
                }),
            ],
            [
                "pending 里的通配记录已有对应文件：提醒移到 evidence",
                ["pending-listed"],
                run({
                    root: withRecord,
                    data: withClaim({ pending: ["validation:docs/validation/*-cubase15-win.md"] }),
                }),
            ],
            [
                "evidence 里不许写通配",
                ["evidence-kind"],
                run({
                    root: withRecord,
                    data: withClaim({ pending: [], evidence: ["validation:docs/validation/*-cubase15-win.md"] }),
                }),
            ],
            [
                "claims.json 不是合法形状",
                ["claims-shape"],
                run({ data: { version: 1, claims: [{ id: "Bad Id", strong: [] }] } }),
            ],
            ["缺 claims 数组", ["claims-shape"], run({ data: { version: 1 } })],
            ["id 不是小写连字符（其余字段都合法）", ["claims-shape"], run({ data: withClaim({ id: "Bad Id" }) })],
            ["缺 strong", ["claims-shape"], run({ data: withClaim({ strong: undefined }) })],
            ["strong 是空数组", ["claims-shape"], run({ data: withClaim({ strong: [] }) })],
            ["缺 hedge", ["claims-shape"], run({ data: withClaim({ hedge: undefined }) })],
            ["缺 evidence", ["claims-shape"], run({ data: withClaim({ evidence: undefined }) })],
            ["scope 不是数组", ["claims-shape"], run({ data: withClaim({ scope: "README.md" }) })],
            [
                "evidence 指向不存在的 validation 记录",
                ["evidence-satisfied"],
                run({ data: withClaim({ pending: [], evidence: ["validation:docs/validation/nowhere.md"] }) }),
            ],
            ["pending 的形态不认识", ["evidence-kind"], run({ data: withClaim({ pending: ["wiki:x"] }) })],
            [
                "id 重复",
                ["claims-shape"],
                run({
                    data: { ...baseClaims, claims: [baseClaims.claims[0], { ...baseClaims.claims[0], scope: [] }] },
                }),
            ],
            [
                "strong 短语的正则没写完",
                ["claims-shape"],
                run({ data: withClaim({ strong: ["starts (with feedback"] }) }),
            ],
            ["strong 短语能匹配空串", ["claims-shape"], run({ data: withClaim({ strong: [".*"] }) })],
            ["hedge 短语宽到匹配任意句子", ["claims-shape"], run({ data: withClaim({ hedge: ["."] }) })],
            ["scope 里写错了文件名", ["claims-shape"], run({ data: withClaim({ scope: ["README.mdd"] }) })],
            [
                "scope 在 web/i18n 之内是合法的",
                [],
                run({ data: withClaim({ scope: ["README.md", "web/i18n/input.js"] }) }),
            ],
            [
                "evidence 的类型不认识",
                ["evidence-kind"],
                run({ data: { ...baseClaims, claims: [{ ...baseClaims.claims[1], evidence: ["wiki:something"] }] } }),
            ],
            [
                "ci 证据指向不存在的 job",
                ["evidence-satisfied"],
                run({ data: { ...baseClaims, claims: [{ ...baseClaims.claims[1], evidence: ["ci:no-such-job"] }] } }),
            ],
            [
                "强声明既无 evidence 也无 pending，且同句没有 hedge",
                ["strong-needs-hedge"],
                run({ data: withClaim({ pending: [], evidence: [] }) }),
            ],
            [
                "强声明既无证据也无 pending，但同句有 hedge",
                [],
                run({
                    data: withClaim({ pending: [], evidence: [] }),
                    files: { "README.md": "Rinne is designed so that it starts with feedback at zero.\n" },
                }),
            ],
            [
                "JSON 词条里的强声明同样要 hedge",
                ["strong-needs-hedge"],
                run({
                    data: withClaim({ pending: [], evidence: [], scope: ["docs/hard-rules.i18n.json"] }),
                    files: { "docs/hard-rules.i18n.json": '{"en":{"body":"Rinne starts with feedback at zero."}}' },
                }),
            ],
            [
                "绝对化强断言没有登记",
                ["unregistered-absolute"],
                run({ data: onlyAuto, files: { "README.md": "The loop delay is identical output every time.\n" } }),
            ],
            [
                "JSON 词条里的绝对化强断言没有登记",
                ["unregistered-absolute"],
                run({
                    data: onlyAuto,
                    files: { "docs/hard-rules.i18n.json": '{"en":{"body":"The delay never changes."}}' },
                }),
            ],
            [
                "声明的 scope 之外的文件不算被覆盖",
                ["unregistered-absolute"],
                run({ data: onlyAuto, files: { "docs/OTHER.md": "The loop delay is identical output every time.\n" } }),
            ],
            [
                "含否定的绝对化句式放行",
                [],
                run({ data: onlyAuto, files: { "README.md": "No software guarantees the loop will never howl.\n" } }),
            ],
            [
                "引号里引用的绝对化句式放行",
                [],
                run({ data: onlyAuto, files: { "README.md": "我们不写「完全一致」这种话。\n" } }),
            ],
            [
                "命中片段自带的否定词不算免责",
                ["unregistered-absolute"],
                run({ data: onlyAuto, files: { "README.md": "环路延迟不随缓冲大小变化。\n" } }),
            ],
            [
                "同句里远处的单字否定不放过强断言",
                ["unregistered-absolute"],
                run({ data: onlyAuto, files: { "README.md": "Rinne guarantees identical output, no exceptions.\n" } }),
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
        // 报错要指到对应的强声明
        const golden05Lines = fired(run({ root: golden05, data: withClaim({ pending: [], evidence: [goldenPred] }) }));
        cases.push({
            label: "golden 谓词不成立时报错指到对应的强声明",
            bad: false,
            passed: golden05Lines.some((l) => l.includes("starts with feedback at zero")),
            expect: [],
        });
        // 工具函数
        const throws = (fn) => {
            try {
                fn();
                return false;
            } catch {
                return true;
            }
        };
        cases.push({
            label: "写坏的正则不悄悄退回字面匹配：matchPhrase 直接抛错",
            bad: true,
            passed: throws(() => matchPhrase("starts (with", "starts (with")),
            expect: [],
        });
        cases.push({
            label: "弯撇号与直撇号写同一句都命中 host['’]s 这种写法",
            bad: false,
            passed: matchPhrase("host['’]s own", "the host's own") && matchPhrase("host['’]s own", "the host’s own"),
            expect: [],
        });
        cases.push({
            label: "ciJobs 认得列 0 注释、行尾注释与下划线的 job 名",
            bad: false,
            passed: ["one", "two", "three_x"].every((j) => wfJobs.has(j)) && wfJobs.size === 3,
            expect: [],
        });
        cases.push({
            label: "临时目录之外的路径拒绝删除",
            bad: true,
            passed: throws(() => removeTemp(root)),
            expect: [],
        });
    } finally {
        for (const d of temps) removeTemp(d);
    }
    const unnamed = Object.keys(RULES).filter((r) => !cases.some((c) => c.expect.includes(r)));
    cases.push({
        label: `每条判据都有坏样例点名${unnamed.length ? `（没有：${unnamed.join("、")}）` : ""}`,
        bad: false,
        passed: unnamed.length === 0,
        expect: [],
    });
    // 参数表块模块和 golden 谓词认同一套参数形状，它的自测在这里一并跑
    cases.push(...paramsTableCases().map((c) => ({ ...c, label: `params-table 块：${c.label}` })));
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
