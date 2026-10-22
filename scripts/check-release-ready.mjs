// SPDX-License-Identifier: GPL-3.0-or-later
//
// 发版就绪检查（release 流水线 verify-tag 之后的那一道）：被判的那棵提交（被打 tag 的那一个）
// 必须满足下面五条，任何一条不成立都不往下走：
//   1. README 的状态行不再写「开发中 / in development」：还写着就说明没准备好发版。
//   2. CHANGELOG 有与本次版本对应的小节（`## [1.2.3]` 或 `## [1.2.3-rc.1]`）。
//   3. frReview 已审校：status 不是 pending；标了 reviewed 时，审校记录要齐（审校人、审校时中文的合并哈希、确认链接）。
//   4. 强声明有证据：docs/claims.json 里每条声明都要有至少一条成立的 evidence；evidence 为空（强短语无证据）、
//      指向的产物不在被判的提交里、golden 求值不成立，都判红。
//      文件还不存在时跳过这一段并写明（声明登记由另一张卡建）。
//   5. INSTALL.txt 引用的仓内文件都存在于被判的那棵提交里：至少 docs/BUILDING.md。
//      打包脚本会把 INSTALL.txt 放进 zip，用户照着它去那个 tag 上找构建步骤；文件不在就是断链。
//
// 它是独立 CLI（scripts/check-release-ready.mjs），不进 scripts/checks.d/：运行器只认
// `--group/--only/--needles/--self-test/...`，没有 --version 入口，而本检查必须按版本判。
//   node scripts/check-release-ready.mjs --version 0.9.0-rc.1
//   node scripts/check-release-ready.mjs --tag v0.9.0-rc.1
//   node scripts/check-release-ready.mjs --self-test
// --version / --tag 省略时只做仓内 lint（不按版本判）：第 2 条与第 5 条跳过。

import os from "node:os";
import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { assertNodeVersion, caseResult, isMain } from "./lib/check-kit.mjs";
import { I18N_FILE, SOURCE_FILE, parseSource, readRepoText, zhHashes } from "./lib/docs-kit.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "..");

/** README 里表示「还没准备好发版」的状态措辞（中英各几种）。 */
const IN_DEVELOPMENT = [
    /状态：开发中/,
    /in development/i,
    /there is no release yet/i,
    /work happens on the `?feature\/v1`? branch/i,
];

/** INSTALL.txt 里点名的仓内文件必须存在于被判的提交里。 */
const REQUIRED_AT_TAG = ["docs/BUILDING.md"];

const CLAIMS_FILE = "docs/claims.json";

/** 从一棵提交里读一个文件（git show <commit>:<path>）。读不到返回 null。 */
function readAtCommit(root, commit, rel) {
    const r = spawnSync("git", ["-C", root, "show", `${commit}:${rel}`], {
        encoding: "utf8",
        maxBuffer: 64 * 1024 * 1024,
    });
    if (r.status !== 0) return null;
    return r.stdout.replace(/\r\n/g, "\n");
}

/** 一棵树里的某个文件在 git 的某次提交下是否存在。 */
function existsAtCommit(root, commit, rel) {
    const r = spawnSync("git", ["-C", root, "ls-tree", "-r", "--name-only", commit, "--", rel], {
        encoding: "utf8",
    });
    if (r.status !== 0) return false;
    return r.stdout.split("\n").some((l) => l.trim() === rel);
}

/** CHANGELOG 里有没有与版本对应的小节（整行匹配：`## [1.2.3]`）。 */
export function hasChangelogSection(text, version) {
    if (!text || !version) return false;
    const esc = version.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    // 标志写在构造参数里，不是模式里：`new RegExp` 的模式里出现 `(?m)` 会被当成非法分组。
    return new RegExp(`^##\\s+\\[${esc}\\]\\s*$`, "m").test(text);
}

/** README 的状态行是不是还写着「开发中」。 */
export function readmeStatusProblem(text) {
    if (text === null) return "README.md 读不到";
    for (const re of IN_DEVELOPMENT) {
        const m = re.exec(text);
        if (m) return `README 的状态行还写着「${m[0]}」：发版前要改成已发布，并写上版本号`;
    }
    return null;
}

/** frReview 的问题：status 不是 pending 就过；标 reviewed 时审校记录要齐。 */
export function frReviewProblems(i18nText, combinedHash) {
    if (i18nText === null) return [`${I18N_FILE} 读不到`];
    let data;
    try {
        data = JSON.parse(i18nText);
    } catch (e) {
        return [`${I18N_FILE} 不是合法 JSON：${e.message}`];
    }
    const fr = data.frReview;
    if (!fr || !["pending", "reviewed"].includes(fr.status))
        return [`${I18N_FILE} 的 frReview.status 应为 pending 或 reviewed`];
    if (fr.status === "pending")
        return [`${I18N_FILE} 的 frReview.status 还是 pending：发版前要有一次三语交叉核对，审校记录写进这份文件`];
    const out = [];
    if (!fr.reviewedBy) out.push(`${I18N_FILE} 的 frReview 已标 reviewed，但 reviewedBy 为空`);
    if (combinedHash && fr.reviewedZhSha256 !== combinedHash)
        out.push(`${I18N_FILE} 的 frReview 已标 reviewed，但 reviewedZhSha256 不是当前中文的合并哈希：审校记录已过期`);
    if (!/^https:\/\/\S+$/.test(fr.approvalUrl ?? ""))
        out.push(`${I18N_FILE} 的 frReview 已标 reviewed，但 approvalUrl 不是 https 链接`);
    return out;
}

/** 点路径取值：a.b.c；取不到返回 undefined。 */
function pathGet(obj, dotted) {
    let cur = obj;
    for (const key of dotted.split(".")) {
        if (cur === null || typeof cur !== "object" || !Object.hasOwn(cur, key)) return undefined;
        cur = cur[key];
    }
    return cur;
}

/** golden 证据的比较：值先当 JSON 解（0、true、"x"），解不出来就当字符串。 */
function goldenHolds(actual, op, wantRaw) {
    let want;
    try {
        want = JSON.parse(wantRaw);
    } catch {
        want = wantRaw;
    }
    switch (op) {
        case "==":
            return JSON.stringify(actual) === JSON.stringify(want);
        case "!=":
            return JSON.stringify(actual) !== JSON.stringify(want);
        case ">":
            return typeof actual === "number" && typeof want === "number" && actual > want;
        case "<":
            return typeof actual === "number" && typeof want === "number" && actual < want;
        case ">=":
            return typeof actual === "number" && typeof want === "number" && actual >= want;
        case "<=":
            return typeof actual === "number" && typeof want === "number" && actual <= want;
        default:
            return false;
    }
}

/**
 * 一条 evidence 的形态与成立情况：ok / malformed / missing（产物不在）/ false（产物在但求值不成立）。
 * exists/read 是被判的那棵提交的读接口：证据产物要在那棵提交里找。
 */
export function evidenceState(exists, read, item) {
    if (typeof item !== "string") return "malformed";
    const i = item.indexOf(":");
    if (i < 0) return "malformed";
    const kind = item.slice(0, i);
    const body = item.slice(i + 1);
    if (!body) return "malformed";
    if (kind === "validation") return exists(body) ? "ok" : "missing";
    if (kind === "issue") return /^https:\/\/\S+$/.test(body) ? "ok" : "malformed";
    if (kind === "ci") return "ok"; // 检查名由别处核对；这里只判形态
    if (kind === "golden") {
        const m = /^([^#\s]+\.json)#(\S+)\s+(==|!=|>=|<=|>|<)\s+(\S.*)$/.exec(body);
        if (!m) return "malformed";
        if (!exists(m[1])) return "missing";
        const text = read(m[1]);
        if (text === null) return "missing";
        let data;
        try {
            data = JSON.parse(text);
        } catch {
            return "false";
        }
        const actual = pathGet(data, m[2]);
        return actual !== undefined && goldenHolds(actual, m[3], m[4].trim()) ? "ok" : "false";
    }
    return "malformed";
}

/**
 * 强声明的证据问题。
 * claims.json 还不存在时返回 null（跳过并写明）；存在时逐条看：
 *   - evidence 为空 → 红（强短语没有证据，只能靠 hedge 句式出现，发版时不放行）；
 *   - evidence 形态不认识 → 红；
 *   - evidence 指向的产物不在被判的提交里 → 红；
 *   - golden 求值不成立 → 红（声明被证据反驳）。
 */
export function claimsProblems(claimsText, exists, read) {
    if (claimsText === null) return null;
    let data;
    try {
        data = JSON.parse(claimsText);
    } catch (e) {
        return [`${CLAIMS_FILE} 不是合法 JSON：${e.message}`];
    }
    if (!data || !Array.isArray(data.claims)) return [`${CLAIMS_FILE} 缺 claims 数组`];
    const out = [];
    for (const c of data.claims) {
        const id = c?.id ?? "(没有 id)";
        const strong = (c?.strong ?? []).join("」「");
        if (!Array.isArray(c?.evidence)) {
            out.push(`${CLAIMS_FILE} 的条目「${id}」缺 evidence 数组`);
            continue;
        }
        if (!c.evidence.length) {
            out.push(`${CLAIMS_FILE} 的条目「${id}」是强短语但没有证据（evidence 为空），强声明「${strong}」判红`);
            continue;
        }
        for (const item of c.evidence) {
            const state = evidenceState(exists, read, item);
            if (state === "malformed") out.push(`${CLAIMS_FILE} 的条目「${id}」的 evidence「${item}」形态不认识`);
            else if (state !== "ok")
                out.push(
                    `${CLAIMS_FILE} 的条目「${id}」的 evidence「${item}」不成立` +
                        `（${state === "missing" ? "产物不在被判的提交里" : "求值为假"}），强声明「${strong}」判红`,
                );
        }
    }
    return out;
}

/**
 * 中文真源的合并哈希：导语与每条规则的哈希拼起来再哈希。
 * 解析与哈希都走 docs-kit 的那一份，不在这里再抄一套：口径有两份，迟早对不上。
 * 解析不了（文件缺或格式坏）返回空串，判据会因此报「审校记录已过期」。
 */
export function zhCombined(srcText) {
    if (!srcText) return "";
    try {
        return zhHashes(parseSource(srcText)).combined;
    } catch {
        return "";
    }
}

/**
 * 判据的纯函数入口：吃一棵树（相对路径 -> 文本或 null）与版本，输出问题数组与注记数组。
 * treeOf(rel) 返回文本或 null（文件不在）；version 为空表示不按版本判（第 2、5 条跳过）。
 * exists / read 是被判的那棵提交的读接口：claims 的证据产物要在那棵提交里找。
 * 返回 { problems, notes }：problems 判红，notes 只说明本轮跳过了什么。
 */
export function readyProblems({
    treeOf,
    version = "",
    combinedHash = "",
    claimsText = null,
    exists = () => false,
    read = () => null,
}) {
    const problems = [];
    const notes = [];
    const readme = readmeStatusProblem(treeOf("README.md"));
    if (readme) problems.push(readme);
    if (version) {
        const changelog = treeOf("CHANGELOG.md");
        if (changelog === null) problems.push("CHANGELOG.md 读不到");
        else if (!hasChangelogSection(changelog, version))
            problems.push(`CHANGELOG.md 里没有与 ${version} 对应的小节（要一行 \`## [${version}]\`）`);
    }
    problems.push(...frReviewProblems(treeOf(I18N_FILE), combinedHash));
    const claims = claimsProblems(claimsText, exists, read);
    if (claims) problems.push(...claims);
    else notes.push(`（跳过）${CLAIMS_FILE} 在被判的提交里还不存在，强声明的证据本轮不判`);
    if (version) {
        for (const rel of REQUIRED_AT_TAG) {
            if (treeOf(rel) === null)
                problems.push(`被判的提交里缺 ${rel}：INSTALL.txt 指向它的构建步骤，缺了就是断链`);
        }
    }
    return { problems, notes };
}

/**
 * 真跑一次：算出被判的那棵提交（tag 或版本），把该提交里的文件取出来判。
 * root 是仓根；commit 是被判的提交（空 = HEAD）；version 为空 = 仓内 lint 模式。
 */
export function checkRepo(root, { version = "", commit = "" } = {}) {
    const problems = [];
    const notes = [];
    const rev = commit || "HEAD";
    const treeOf = (rel) => readAtCommit(root, rev, rel);

    const readme = readmeStatusProblem(treeOf("README.md"));
    if (readme) problems.push(readme);

    if (version) {
        const changelog = treeOf("CHANGELOG.md");
        if (changelog === null) problems.push("CHANGELOG.md 读不到");
        else if (!hasChangelogSection(changelog, version))
            problems.push(`CHANGELOG.md 里没有与 ${version} 对应的小节（要一行 \`## [${version}]\`）`);
        for (const rel of REQUIRED_AT_TAG) {
            if (!existsAtCommit(root, rev, rel))
                problems.push(`被判的提交里缺 ${rel}：INSTALL.txt 指向它的构建步骤，缺了就是断链`);
        }
    }

    // 中文真源读不到或解析不了时，审校哈希无从核对：判红，不能悄悄跳过那一条。
    const guide = treeOf(SOURCE_FILE);
    const combined = zhCombined(guide);
    if (!combined) problems.push(`${SOURCE_FILE} 读不到或解析不了：frReview 的审校哈希无从核对`);
    problems.push(...frReviewProblems(treeOf(I18N_FILE), combined));

    const claimsText = treeOf(CLAIMS_FILE);
    const claims = claimsProblems(
        claimsText,
        (rel) => existsAtCommit(root, rev, rel),
        (rel) => readAtCommit(root, rev, rel),
    );
    if (claims) problems.push(...claims);
    else notes.push(`（跳过）${CLAIMS_FILE} 在被判的提交里还不存在，强声明的证据本轮不判`);

    return { problems, notes };
}

// ---- 自测 -------------------------------------------------------------------------------

/** 一棵内存里的树：{ 相对路径 -> 文本 }，不在树里的文件读不到。 */
function treeOf(files) {
    return (rel) => (Object.hasOwn(files, rel) ? files[rel] : null);
}

/** frReview 已审校、各项齐备的那一份。 */
const reviewedI18n = (hash) =>
    JSON.stringify(
        {
            frReview: {
                status: "reviewed",
                reviewedBy: "维护者",
                reviewedZhSha256: hash,
                approvalUrl: "https://example.com/x",
            },
        },
        null,
        2,
    );

/** 一棵最小的「什么都合规」的树。 */
function goodTree(extra = {}) {
    return {
        "README.md": "# Synchain Rinne\n\n> Released. See the releases page.\n",
        "CHANGELOG.md": "## [Unreleased]\n\n## [0.9.0-rc.1]\n\n- 第一个候选版\n",
        [I18N_FILE]: reviewedI18n("0".repeat(64)),
        "docs/BUILDING.md": "# Build\n",
        ...extra,
    };
}

export function selfTest() {
    const HASH = "0".repeat(64);
    const good = goodTree();

    /** 用给定的树与版本跑一遍判据，返回问题数组（只有判红的那些；注记不算问题）。 */
    const run = (files, version = "0.9.0-rc.1", hash = HASH) =>
        readyProblems({
            treeOf: treeOf(files),
            version,
            combinedHash: hash,
            claimsText: files[CLAIMS_FILE] ?? null,
            // 被判的那棵提交里存在的文件：树里有就是有（自测的树就是那棵提交）。
            exists: (rel) => Object.hasOwn(files, rel),
            read: (rel) => (Object.hasOwn(files, rel) ? files[rel] : null),
        }).problems;

    const claimsWith = (claim) => JSON.stringify({ claims: [claim] }, null, 2);
    const cases = [
        ["什么都合规的树没有问题", false, run(good)],
        [
            "README 还写着「开发中」",
            true,
            run({ ...good, "README.md": "> **Status: in development.** There is no release yet.\n" }),
        ],
        ["CHANGELOG 缺对应小节", true, run({ ...good, "CHANGELOG.md": "## [Unreleased]\n" })],
        [
            "frReview 还是 pending",
            true,
            // pending 这一格只因为 status 判红：审校记录那几项照常齐备，
            // 否则「pending」与「审校记录缺项」两条会互相顶掉，判据改了也看不出来。
            run({
                ...good,
                [I18N_FILE]: JSON.stringify({
                    frReview: {
                        status: "pending",
                        reviewedBy: "维护者",
                        reviewedZhSha256: HASH,
                        approvalUrl: "https://example.com/x",
                    },
                }),
            }),
        ],
        ["frReview 已审校但哈希过期", true, run({ ...good, [I18N_FILE]: reviewedI18n("1".repeat(64)) })],
        [
            "frReview 已审校但审校人为空",
            true,
            run({ ...good, [I18N_FILE]: reviewedI18n(HASH).replace('"维护者"', '""') }),
        ],
        [
            "frReview 已审校但确认链接不是 https",
            true,
            run({ ...good, [I18N_FILE]: reviewedI18n(HASH).replace("https://example.com/x", "http://example.com/x") }),
        ],
        [
            "强声明的 evidence 为空（强短语无证据）",
            true,
            run({
                ...good,
                ...{},
                [CLAIMS_FILE]: claimsWith({ id: "safe-defaults", strong: ["反馈默认为零"], hedge: [], evidence: [] }),
            }),
        ],
        [
            "强声明的证据不在（validation 产物缺失）",
            true,
            run({
                ...good,
                ...{},
                [CLAIMS_FILE]: claimsWith({
                    id: "safe-defaults",
                    strong: ["反馈默认为零"],
                    hedge: [],
                    evidence: ["validation:docs/validation/x.md"],
                }),
            }),
        ],
        [
            "强声明的证据在（validation 产物存在）",
            false,
            run({
                ...good,
                ...{ "docs/validation/x.md": "# 记录\n" },
                [CLAIMS_FILE]: claimsWith({
                    id: "safe-defaults",
                    strong: ["反馈默认为零"],
                    hedge: [],
                    evidence: ["validation:docs/validation/x.md"],
                }),
            }),
        ],
        [
            "强声明的 golden 求值不成立（默认值不是 0）",
            true,
            run({
                ...good,
                ...{ "tests/golden/p.json": '{"input":{"feedback":{"default":0.5}}}' },
                [CLAIMS_FILE]: claimsWith({
                    id: "safe-defaults",
                    strong: ["反馈默认为零"],
                    hedge: [],
                    evidence: ["golden:tests/golden/p.json#input.feedback.default == 0"],
                }),
            }),
        ],
        [
            "强声明的 golden 求值成立",
            false,
            run({
                ...good,
                ...{ "tests/golden/p.json": '{"input":{"feedback":{"default":0}}}' },
                [CLAIMS_FILE]: claimsWith({
                    id: "safe-defaults",
                    strong: ["反馈默认为零"],
                    hedge: [],
                    evidence: ["golden:tests/golden/p.json#input.feedback.default == 0"],
                }),
            }),
        ],
        [
            "强声明的 golden 路径取不到",
            true,
            run({
                ...good,
                ...{ "tests/golden/p.json": '{"input":{"feedback":{"default":0}}}' },
                [CLAIMS_FILE]: claimsWith({
                    id: "safe-defaults",
                    strong: ["反馈默认为零"],
                    hedge: [],
                    evidence: ["golden:tests/golden/p.json#input.nope == 0"],
                }),
            }),
        ],
        [
            "强声明的 evidence 形态不认识",
            true,
            run({
                ...good,
                ...{},
                [CLAIMS_FILE]: claimsWith({
                    id: "safe-defaults",
                    strong: ["反馈默认为零"],
                    hedge: [],
                    evidence: ["hearsay:somebody said so"],
                }),
            }),
        ],
        [
            "被判的提交里缺 docs/BUILDING.md",
            true,
            (() => {
                const t = { ...good };
                delete t["docs/BUILDING.md"];
                return run(t);
            })(),
        ],
        ["claims.json 不存在时跳过并写明（不判红）", false, run(good)],
        [
            "不按版本判时跳过 CHANGELOG 与 BUILDING 两条",
            false,
            run({ ...good, "CHANGELOG.md": "## [Unreleased]\n" }, ""),
        ],
    ].map(([label, bad, problems]) => ({
        label,
        bad,
        passed: bad ? problems.length > 0 : problems.length === 0,
    }));

    // 渲染正文的几格：正文的第一行必须是两个资产直链，第二行是「不要下载 Source code」。
    cases.push(...renderCases());

    // 端到端一格：真的建一个临时仓、打一个 tag，在那棵提交上判。
    // 这一格盯住「判的是被打 tag 的提交，不是工作树」：工作树里有 docs/BUILDING.md、
    // 被 tag 的提交里没有时，必须判红。
    const e2e = endToEndCase();
    cases.push(e2e);
    return caseResult(cases);
}

/**
 * 渲染正文的自测格。
 * 盯住三件事：正文只放模板块（不含 RELEASE.md 的其它部分）、第一行是两个直链、
 * 第二行写「不要下载 Source code」、占位符替换不了就判红。
 */
function renderCases() {
    const TEMPLATE = [
        "# 发版",
        "",
        "这一段不进正文。",
        "",
        "<!-- BEGIN release-notes -->",
        "{{HEADER}}",
        "",
        "## Upgrade notes",
        "",
        "tag {{TAG}} version {{VERSION}}",
        "",
        "| Asset | SHA-256 |",
        "| --- | --- |",
        "| `{{WIN_FILE}}` | `{{WIN_SHA256}}` |",
        "| `{{MAC_FILE}}` | `{{MAC_SHA256}}` |",
        "",
        "commit {{SOURCE_COMMIT}} juce {{JUCE_COMMIT}} webview2 {{WEBVIEW2_VERSION}}",
        "<!-- END release-notes -->",
        "",
        "## 附录 A",
        "",
        "这一段也不进正文。",
        "",
    ].join("\n");
    const vars = {
        HEADER: releaseNotesHeader({ version: "0.9.0-rc.1", tag: "v0.9.0-rc.1" }),
        VERSION: "0.9.0-rc.1",
        TAG: "v0.9.0-rc.1",
        WIN_FILE: "Rinne-v0.9.0-rc.1-win64.zip",
        MAC_FILE: "Rinne-v0.9.0-rc.1-macos-arm64.zip",
        WIN_SHA256: "a".repeat(64),
        MAC_SHA256: "b".repeat(64),
        SOURCE_COMMIT: "c".repeat(40),
        JUCE_COMMIT: "d".repeat(40),
        WEBVIEW2_VERSION: "1.0.2957.106",
    };
    const out = [];
    let text = "";
    try {
        text = renderNotes({ template: TEMPLATE, vars });
    } catch (e) {
        return [
            {
                label: "渲染：正文第一行是那两个直链，第二行是「不要下载 Source code」",
                bad: false,
                passed: false,
            },
        ];
    }
    const lines = text.split("\n");
    out.push({
        label: "渲染：正文第一行是那两个直链，第二行是「不要下载 Source code」",
        bad: false,
        passed:
            lines[0] ===
                "[Windows x64](https://github.com/synchain-oss/rinne/releases/download/v0.9.0-rc.1/Rinne-v0.9.0-rc.1-win64.zip) · [macOS arm64](https://github.com/synchain-oss/rinne/releases/download/v0.9.0-rc.1/Rinne-v0.9.0-rc.1-macos-arm64.zip)" &&
            lines[1].startsWith("不要下载 Source code"),
    });
    out.push({
        label: "渲染：正文只放模板块，RELEASE.md 的其它部分不进正文",
        bad: false,
        passed: !text.includes("这一段不进正文。") && !text.includes("这一段也不进正文。") && !text.includes("# 发版"),
    });
    out.push({
        label: "渲染：占位符都替换掉了",
        bad: false,
        passed: !/\{\{[A-Z0-9_]+\}\}/.test(text) && text.includes("v0.9.0-rc.1"),
    });
    out.push({
        label: "渲染：替换不了的占位符判红",
        bad: true,
        passed: (() => {
            try {
                renderNotes({ template: TEMPLATE, vars: { ...vars, TAG: undefined } });
                return false;
            } catch {
                return true;
            }
        })(),
    });
    // 仓里真正的模板块：占位符的名字与这里的变量表对不上（改了一边忘了另一边）就当场红。
    out.push({
        label: "渲染：仓里 docs/RELEASE.md 的模板块能渲染，占位符一个不剩",
        bad: false,
        passed: (() => {
            try {
                const real = readRepoText(REPO, "docs/RELEASE.md");
                if (real === null) return false;
                const rendered = renderNotes({ template: real.replace("{{HEADER}}", () => vars.HEADER), vars });
                return !/\{\{[A-Z0-9_]+\}\}/.test(rendered) && rendered.startsWith("[Windows x64](");
            } catch {
                return false;
            }
        })(),
    });
    out.push({
        label: "渲染：缺模板块标记判红",
        bad: true,
        passed: (() => {
            try {
                renderNotes({ template: "# 发版\n", vars });
                return false;
            } catch {
                return true;
            }
        })(),
    });
    return out;
}

/**
 * 端到端：临时仓里打一个 tag，被 tag 的提交里没有 docs/BUILDING.md → 判红；
 * 补上再打一个 tag → 判绿（README 状态行与 CHANGELOG 都对）。
 */
function endToEndCase() {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "rinne-release-ready-"));
    try {
        const git = (args) => spawnSync("git", ["-C", dir, ...args], { encoding: "utf8" });
        git(["init", "-q", "-b", "main"]);
        git(["config", "user.name", "DLsnows"]);
        git(["config", "user.email", "noreply@synchain.ca"]);
        const put = (rel, text) => {
            // 目录是文件的父目录，不是文件本身：把 rel 当成路径的一部分再 dirname。
            fs.mkdirSync(path.join(dir, path.dirname(rel)), { recursive: true });
            fs.writeFileSync(path.join(dir, rel), text);
        };
        // 最小的中文真源：frReview 的审校哈希要对着它算，不能是占位值。
        const guide = "## 使用前必读\n\n导语一行。\n\n1. **规则一**：正文。\n";
        const HASH = zhCombined(guide);
        const good = goodTree({ [I18N_FILE]: reviewedI18n(HASH) });
        put(SOURCE_FILE, guide);
        put("README.md", good["README.md"]);
        // CHANGELOG 里有两次发版对应的小节：补 BUILDING.md 之后打的是 rc.2。
        put(
            "CHANGELOG.md",
            "## [Unreleased]\n\n## [0.9.0-rc.2]\n\n- 第二个候选版\n\n## [0.9.0-rc.1]\n\n- 第一个候选版\n",
        );
        put(I18N_FILE, good[I18N_FILE]);
        git(["add", "-A"]);
        git(["commit", "-q", "-m", "first"]);
        git(["tag", "v0.9.0-rc.1"]);
        // 被 tag 的提交里没有 docs/BUILDING.md：工作树里也不加，判红。
        let r = checkRepo(dir, { version: "0.9.0-rc.1", commit: "v0.9.0-rc.1^{commit}" });
        const missing = r.problems.length > 0 && r.problems.some((p) => p.includes("docs/BUILDING.md"));

        // 补上 BUILDING.md，打第二个 tag：那棵提交齐了，判绿。
        put("docs/BUILDING.md", "# Build\n");
        git(["add", "-A"]);
        git(["commit", "-q", "-m", "add building"]);
        git(["tag", "v0.9.0-rc.2"]);
        const r2 = checkRepo(dir, { version: "0.9.0-rc.2", commit: "v0.9.0-rc.2^{commit}" });
        const ok = r2.problems.length === 0;

        // 中文真源改了、审校记录没跟上：第三个 tag 的那棵提交里 frReview 的哈希过期，判红。
        put(SOURCE_FILE, guide.replace("正文", "改过的正文"));
        git(["add", "-A"]);
        git(["commit", "-q", "-m", "change source"]);
        git(["tag", "v0.9.0-rc.3"]);
        const r3 = checkRepo(dir, { version: "0.9.0-rc.2", commit: "v0.9.0-rc.3^{commit}" });
        const stale = r3.problems.some((p) => p.includes("审校记录已过期"));
        return {
            label: "端到端：缺 docs/BUILDING.md 判红，补上判绿，中文真源改了而审校哈希没跟上又判红",
            bad: true,
            passed: missing && ok && stale,
        };
    } catch (e) {
        return { label: "端到端：临时仓那一格跑不起来", bad: true, passed: false };
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

/**
 * 渲染 Release 正文。
 * 模板块在 docs/RELEASE.md 里，用一对标记圈出：
 *   <!-- BEGIN release-notes -->
 *   …
 *   <!-- END release-notes -->
 * 渲染只取标记之间的内容并替换占位符：{{HEADER}}、{{VERSION}}、{{TAG}}、{{WIN_SHA256}}、{{MAC_SHA256}}、
 * {{WIN_FILE}}、{{MAC_FILE}}、{{SOURCE_COMMIT}}、{{JUCE_COMMIT}}、{{WEBVIEW2_VERSION}}。
 * 替换不了的占位符留在正文里就是断链，渲染时判红。
 * 模板块本身留在仓里（发版清单要引用它），**不进正文**：正文只放渲染出来的这一段。
 * 返回的这一段第一行是那两个资产直链，第二行是「不要下载 Source code」。
 */
export function renderNotes({ template, vars }) {
    const begin = "<!-- BEGIN release-notes -->";
    const end = "<!-- END release-notes -->";
    const at = template.indexOf(begin);
    const stop = template.indexOf(end);
    if (at < 0 || stop < 0) throw new Error("docs/RELEASE.md 缺发布说明模板块（BEGIN/END release-notes 标记）");
    if (stop < at) throw new Error("docs/RELEASE.md 的发布说明模板块标记顺序不对");
    const body = template.slice(at + begin.length, stop);
    const rendered = body.replace(/\{\{([A-Z0-9_]+)\}\}/g, (_, k) => {
        // 键在但值是空/undefined 的，也要当「替换不了」：那说明这一版的取值没备齐。
        if (!Object.hasOwn(vars, k) || vars[k] === undefined || vars[k] === null || vars[k] === "")
            throw new Error(`发布说明模板块里有替换不了的占位符 {{${k}}}`);
        return String(vars[k]);
    });
    const left = rendered.match(/\{\{[A-Z0-9_]+\}\}/g);
    if (left) throw new Error(`发布说明模板块渲染后还有占位符：${left.join("、")}`);
    return rendered.replace(/^\n+/, "").replace(/\s+$/, "");
}

/** 渲染正文里的那两个直链与校验文件：第一行两个链接，第二行「不要下载 Source code」。 */
export function releaseNotesHeader({ version, tag, repo = "synchain-oss/rinne" }) {
    const base = `https://github.com/${repo}/releases/download/${tag}`;
    const win = `Rinne-v${version}-win64.zip`;
    const mac = `Rinne-v${version}-macos-arm64.zip`;
    return [
        `[Windows x64](${base}/${win}) · [macOS arm64](${base}/${mac})`,
        "不要下载 Source code（那不是发行包，也不含可运行的插件）。",
    ].join("\n");
}

/**
 * 从仓里的模板块与两份 package-summary 渲染完整正文。
 * dist 是两个打包 job 交上来的资产目录（里面有 package-summary-win64.md 与 package-summary-macos-arm64.md）。
 * 正文里的 SHA-256 从 summary 里读出来，不手打：手打的哈希迟早与产物对不上。
 */
export function renderReleaseNotes({ version, tag, dist, root = REPO }) {
    const template = readRepoText(root, "docs/RELEASE.md");
    if (template === null) throw new Error("docs/RELEASE.md 读不到");
    const summary = (name) => {
        const text = readRepoText(dist, name);
        if (text === null) throw new Error(`读不到 ${dist}/${name}：打包 job 的产物没上来`);
        return text;
    };
    const win = summary("package-summary-win64.md");
    const mac = summary("package-summary-macos-arm64.md");
    /** 从 summary 的表里取一行的值（`| key | value |`）。 */
    const field = (text, key) => {
        const m = new RegExp(`^\\|\\s*${key}\\s*\\|([^|]*)\\|`, "m").exec(text);
        return m ? m[1].trim() : "";
    };
    const winSha = field(win, "sha256");
    const macSha = field(mac, "sha256");
    const commit = field(win, "sourceCommit");
    const juceCommit = field(win, "juceCommit");
    const webview2 = field(win, "webview2Version");
    for (const [name, value] of [
        ["sha256（Windows）", winSha],
        ["sha256（macOS）", macSha],
        ["sourceCommit", commit],
        ["juceCommit", juceCommit],
    ]) {
        if (!value) throw new Error(`package-summary 里读不到 ${name}`);
    }
    const vars = {
        VERSION: version,
        TAG: tag,
        WIN_SHA256: winSha,
        MAC_SHA256: macSha,
        WIN_FILE: `Rinne-v${version}-win64.zip`,
        MAC_FILE: `Rinne-v${version}-macos-arm64.zip`,
        SOURCE_COMMIT: commit,
        JUCE_COMMIT: juceCommit,
        WEBVIEW2_VERSION: webview2 || "n/a",
    };
    const header = releaseNotesHeader({ version, tag });
    // 模板块的第一行占位符 {{HEADER}} 放那两个直链：直链按 tag 拼出来，不写死在文档里。
    const withHeader = template.replace("{{HEADER}}", () => header);
    return renderNotes({ template: withHeader, vars });
}

function main(argv) {
    assertNodeVersion();
    if (argv.includes("--self-test") || argv.includes("-SelfTest")) {
        const r = selfTest();
        for (const l of r.lines) console.error(l);
        console.log(`check-release-ready --self-test：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`);
        return r.ok ? 0 : 1;
    }
    const i = argv.indexOf("--version");
    const j = argv.indexOf("--tag");
    const tag = j >= 0 ? (argv[j + 1] ?? "") : "";
    const explicitVersion = i >= 0 ? (argv[i + 1] ?? "") : "";
    const version = explicitVersion || tag.replace(/^v/, "");

    // 按版本判时，判的是被打 tag 的那棵提交；没给 tag 就用当前 HEAD（本地 lint 模式）。
    let commit = "";
    if (tag) {
        const r = spawnSync("git", ["-C", REPO, "rev-parse", "--verify", `${tag}^{commit}`], { encoding: "utf8" });
        if (r.status !== 0) {
            console.error(`check-release-ready：tag ${tag} 在仓库里解析不到提交`);
            return 1;
        }
        commit = r.stdout.trim();
    }

    // 渲染正文：不判就绪，只渲染。发布流水线用它生成 Release 的正文。
    if (argv.includes("--render-notes")) {
        const distIdx = argv.indexOf("--dist");
        const outIdx = argv.indexOf("--out");
        const dist = distIdx >= 0 ? argv[distIdx + 1] : "dist";
        const out = outIdx >= 0 ? argv[outIdx + 1] : "";
        try {
            const text = renderReleaseNotes({ version, tag: tag || `v${version}`, dist });
            if (out) {
                fs.mkdirSync(path.dirname(path.resolve(out)), { recursive: true });
                fs.writeFileSync(out, text, "utf8");
                console.log(`check-release-ready：正文已写到 ${out}`);
            } else {
                process.stdout.write(text);
            }
            return 0;
        } catch (e) {
            console.error(`check-release-ready：${e.message}`);
            return 1;
        }
    }

    const { problems, notes } = checkRepo(REPO, { version, commit });
    for (const n of notes) console.log(n);
    for (const p of problems) console.error(`check-release-ready：${p}`);
    if (problems.length) {
        console.error(`check-release-ready：${problems.length} 处问题`);
        return 1;
    }
    console.log(`check-release-ready 通过${version ? `（${version}）` : "（仓内 lint 模式）"}`);
    return 0;
}

if (isMain(import.meta.url)) process.exitCode = main(process.argv.slice(2));
