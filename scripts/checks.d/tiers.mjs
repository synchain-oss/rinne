// SPDX-License-Identifier: GPL-3.0-or-later
//
// 宿主分级一致性（docs-truth 的一部分）。数据是 docs/daw-tiers.json：platforms 是平台与格式的清单，hosts 是一行一个宿主，
// 记它的目标等级（targetTier）、当前等级（tier）、状态（status）、证据与已过的验收项（checks）。
// 等级与状态的取值、显示文案由生成块模块 gen-docs.d/daw-tiers.mjs 登记一份（本检查和它共用，不各抄一份）。
//
// 等级的含义：Tier 1 完整验证（真机上过完整验收矩阵）；Tier 2 部分验证（只验证了列出的项目）；Tier 3 未在真机验证
// （只有 CI 证据）；out-of-scope 不提供该格式或明确不支持。数字越小等级越高。
//
// 判据：
//   tiers-shape         顶层有 platforms 与 hosts；每条 host 有 host / versions / os / formats / tier / targetTier /
//                       status / evidence / checks / notes；notes 是 en 与 zh 都非空的对象；checks 是七个键都为布尔的对象
//   tier-values         tier 与 targetTier ∈ {null, 1, 2, 3, "out-of-scope"}
//   status-values       status ∈ {awaiting-test, partial, verified, out-of-scope}
//   awaiting-null       status == "awaiting-test" ⇒ tier 为 null 且 evidence 为空
//   tier-evidence       有等级的行必须有证据，且每条都成立：Tier 1 / 2 至少一条 validation 记录；Tier 3 至少一条 ci 证据；
//                       validation 记录必须是这一行自己的：文件名 <YYYY-MM-DD>-<整个宿主名的 slug>[-版本]-<系统后缀>.md，
//                       宿主名整个对上（cubase-pro 不是 cubase，fl-studio 不是 studio），版本对得上（文件名里的版本，
//                       或文件名没写时正文里有「宿主名 版本」），系统对得上；同族宿主、别的版本、别的系统的记录都不算。
//                       系统后缀只有一种写法 win / macos / linux（与 claims 的 validation 路径共用 recordNameTail，
//                       不认 -windows.md、-mac.md）；记录路径只准是仓内的规范相对路径（与 claims 共用 repoPathProblem：
//                       拒绝 .. 段、绝对路径、反斜杠、盘符，规范化后仍在仓根之内）
//   tier1-checks        tier === 1 ⇒ checks 全部为 true
//   verified-consistent status 与等级自洽，是「当且仅当」：verified ⇔ tier 是数字且 tier ≤ targetTier；
//                       partial 只在 tier 是 1…3 的具体等级且 tier > targetTier（还没到目标）时才对；
//                       awaiting-test 与 out-of-scope 要求 tier 为 null
//   out-of-scope        status == "out-of-scope" ⇒ tier 为 null、evidence 为空、targetTier 为 "out-of-scope"
//   hosts-unique        (host, versions, os) 不重复，同一宿主不会写两行互相矛盾的等级
//   hosts-platform      不是 out-of-scope 的行：os 在平台清单里（或 any），formats 是同 os 平台所发布格式的子集
//   platforms-shape     platforms 每项有 id / os / arch / formats / minVersion；formats 取值在 identity.json 登记的格式内；
//                       id 唯一；macOS 的最低版本与 CMake 的部署目标一致
//
// 判据名见 RULES，报错行以 [名字] 开头。自测的每个坏样例写明它必须触发哪几条判据，并核对每条判据都至少被一个坏样例
// 点名：删掉任何一条判据，点名它的坏样例就变红。生成块模块（daw-tiers、platform-badge）的自测也在这里跑，
// 它们读的是同一份 daw-tiers.json。

import fs from "node:fs";
import path from "node:path";
import { ROOT, caseResult, readText } from "../lib/check-kit.mjs";
import { cleanupTempDir, makeTempDir } from "../lib/safe-rm.mjs";
import { CHECK_FIELDS, STATUS_VALUES, TIER_VALUES, selfTestCases as dawTiersCases } from "../gen-docs.d/daw-tiers.mjs";
import { selfTestCases as platformBadgeCases } from "../gen-docs.d/platform-badge.mjs";
import { ciJobs, evidenceState, recordNameTail, recordOsSuffixes, repoPathProblem } from "./claims.mjs";

export { STATUS_VALUES, TIER_VALUES };
export const TIERS_FILE = "docs/daw-tiers.json";
export const IDENTITY_FILE = "tests/golden/identity.json";
export const MAC_CMAKE_FILE = "cmake/RinneMac.cmake";

/** 判据名 → 说明。 */
export const RULES = {
    "tiers-shape": "daw-tiers.json 的形状：每条 host 的字段齐全，notes 有 en 与 zh，checks 七个键都是布尔",
    "tier-values": "tier 与 targetTier 的取值在登记的集合内",
    "status-values": "status 的取值在登记的集合内",
    "awaiting-null": "awaiting-test 的行 tier 为 null 且 evidence 为空",
    "tier-evidence":
        "有等级的行必须有成立的证据：Tier 1 与 Tier 2 要有这一行自己的 validation 记录（整个宿主名、版本、系统都对得上），Tier 3 要有 ci 证据",
    "tier1-checks": "Tier 1 的 checks 全部为 true",
    "verified-consistent":
        "status 与等级自洽（当且仅当）：verified ⇔ tier ≤ targetTier；partial 要求 tier 是 1…3 且 tier > targetTier；awaiting-test 与 out-of-scope 要求 tier 为 null",
    "out-of-scope": "out-of-scope 的行 tier 为 null、evidence 为空、targetTier 为 out-of-scope",
    "hosts-unique": "(host, versions, os) 不重复",
    "hosts-platform": "不是 out-of-scope 的行：os 在平台清单里（或 any），formats 是同 os 平台所发布格式的子集",
    "platforms-shape":
        "platforms 每项的字段齐全、formats 取值在登记的格式内、id 唯一、macOS 最低版本与 CMake 部署目标一致",
};

/** 每条 host 必须有的字段。 */
export const HOST_FIELDS = [
    "host",
    "versions",
    "os",
    "formats",
    "tier",
    "targetTier",
    "status",
    "evidence",
    "checks",
    "notes",
];
/** 平台必须有的字段。 */
export const PLATFORM_FIELDS = ["id", "os", "arch", "formats", "minVersion"];

const isTier = (v) => TIER_VALUES.some((t) => t === v);
const isStatus = (v) => STATUS_VALUES.includes(v);
const isNull = (v) => v === null || v === undefined;
const isNum = (v) => typeof v === "number";
const isObj = (v) => typeof v === "object" && v !== null && !Array.isArray(v);

/** identity.json 登记的格式（base 加 apple）；读不到返回 null。 */
export function identityFormats(root) {
    const text = readText(path.join(root, IDENTITY_FILE));
    if (text === null) return null;
    try {
        const f = JSON.parse(text).formats;
        const list = [...(f?.base ?? []), ...(f?.apple ?? [])];
        return list.length ? list : null;
    } catch {
        return null;
    }
}

/** CMake 里 macOS 部署目标的主版本号（如 11）；没有这个文件或读不出来返回 null。 */
export function macDeploymentMajor(root) {
    const text = readText(path.join(root, MAC_CMAKE_FILE));
    const m = text && /CMAKE_OSX_DEPLOYMENT_TARGET\s+"(\d+)(?:\.\d+)*"/.exec(text);
    return m ? Number(m[1]) : null;
}

/** 宿主名的 slug：小写，非字母数字的片段换成连字符，多词宿主名整个连字符化（Cubase Pro → cubase-pro）。 */
export function hostSlug(host) {
    return String(host ?? "")
        .toLowerCase()
        .replace(/[^a-z0-9]+/g, "-")
        .replace(/^-+|-+$/g, "");
}

const escapeRe = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

/**
 * 一条 validation 记录能不能给这一行作证；返回不能的原因，能则返回 null。
 * 记录文件名的形态是本检查约定的 <YYYY-MM-DD>-<宿主 slug>[-版本]-<系统后缀>.md：
 *   路径：只准是仓内的规范相对路径（repoPathProblem，与 claims 的 validation 证据同一个函数）：
 *         拒绝 .. 段、绝对路径、反斜杠、盘符，规范化后必须仍在仓根之内，不去读仓外的文件；
 *   宿主：slug 要是整个宿主名（cubase-pro，不是 cubase；fl-studio，不是 studio），后面直接是版本或系统，不许多出别的词；
 *   版本：这一行写了具体版本时，文件名里的版本要对得上（15 对 15 与 15.0.30，不对 14）；
 *         文件名没写版本时，记录正文里要出现「宿主名 版本」；版本写 any 的行不核对；
 *   系统：后缀只有一种写法，win / macos / linux（recordNameTail，与 claims 的 validation 路径同一个函数生成）：
 *         windows 的行只认 -win.md，macos 的行只认 -macos.md，不认 -windows.md、-mac.md；系统写 any 的行认全部后缀。
 * 记录的正文是人写的自由格式，这里只核对文件名与版本，不去解析 checks 各项的结论。
 */
export function recordProblem(row, item, root) {
    const rel = String(item).slice("validation:".length);
    const pathWhy = repoPathProblem(rel, root);
    if (pathWhy) return `路径${pathWhy}，验证记录只准是仓内的规范相对路径`;
    const file = path.basename(rel).toLowerCase();
    const slug = hostSlug(row?.host);
    if (!slug) return null; // 没有宿主名由 tiers-shape 报
    const osName = String(row?.os ?? "").toLowerCase();
    const tail = recordNameTail(osName);
    if (!tail) return `系统「${osName || "未写"}」没有登记记录文件名的系统后缀，不能用验证记录作证`;
    const m = new RegExp(`^\\d{4}-\\d{2}-\\d{2}-${escapeRe(slug)}(?:-?(\\d+(?:\\.\\d+)*))?${tail}$`).exec(file);
    const suffixes = recordOsSuffixes(osName);
    const want = `<YYYY-MM-DD>-${slug}[-版本]-${suffixes.length === 1 ? suffixes[0] : "<系统>"}.md`;
    if (!m) return `文件名不是这一行（${row.host}，${osName}）自己的记录，应写成 ${want}`;
    const versions = String(row?.versions ?? "").trim();
    if (!versions || versions.toLowerCase() === "any") return null;
    const ver = versions.replace(/\.x$/i, "");
    const inName = m[1];
    if (inName !== undefined)
        return inName === ver || inName.startsWith(`${ver}.`)
            ? null
            : `文件名里的版本 ${inName} 与这一行的 ${versions} 对不上`;
    const text = readText(path.join(root, rel)) ?? "";
    const words = String(row.host).trim().split(/\s+/).map(escapeRe).join("[\\s-]+");
    return new RegExp(`${words}[^a-z0-9]{0,3}v?${escapeRe(ver)}(?![0-9])`, "i").test(text)
        ? null
        : `文件名没写版本，记录正文里也没有「${row.host} ${ver}」，对不上这一行的版本 ${versions}`;
}

/**
 * 判据的实现。data 是 daw-tiers.json 解析后的对象；root 是仓根（evidence 查表用）；
 * formats 是 identity.json 登记的格式；macMajor 是 CMake 的 macOS 部署目标主版本（null 时不核对）。
 * 返回问题数组（每行以 [判据名] 开头）。
 */
export function tierProblems({
    data,
    root,
    jobs = ciJobs(root),
    formats = identityFormats(root),
    macMajor = macDeploymentMajor(root),
}) {
    const out = [];
    const add = (rule, message) => {
        if (!Object.hasOwn(RULES, rule)) throw new Error(`未登记的判据 ${rule}`);
        out.push(`[${rule}] ${message}`);
    };
    if (!data || typeof data !== "object" || !Array.isArray(data.platforms) || !Array.isArray(data.hosts)) {
        add("tiers-shape", `${TIERS_FILE} 顶层缺 platforms 或 hosts`);
        return out;
    }
    if (!formats) add("platforms-shape", `没有读到 ${IDENTITY_FILE} 登记的格式，没法核对 formats`);
    const known = formats ?? [];

    // 平台
    const ids = new Set();
    for (const p of data.platforms) {
        const id = p?.id ?? "(没有 id)";
        for (const f of PLATFORM_FIELDS)
            if (p?.[f] === undefined) add("platforms-shape", `${TIERS_FILE} 的平台「${id}」缺字段 ${f}`);
        if (typeof p?.id === "string") {
            if (ids.has(p.id)) add("platforms-shape", `${TIERS_FILE} 的平台 id「${p.id}」重复`);
            ids.add(p.id);
        }
        for (const f of Array.isArray(p?.formats) ? p.formats : [])
            if (formats && !known.includes(f))
                add(
                    "platforms-shape",
                    `${TIERS_FILE} 的平台「${id}」的格式「${f}」不在 ${IDENTITY_FILE} 登记的 {${known.join(", ")}} 内`,
                );
        if (p?.os === "macOS" && macMajor !== null && typeof p.minVersion === "string") {
            const major = /^macOS (\d+)/.exec(p.minVersion)?.[1];
            if (Number(major) !== macMajor)
                add(
                    "platforms-shape",
                    `${TIERS_FILE} 的平台「${id}」最低版本「${p.minVersion}」与 ${MAC_CMAKE_FILE} 的部署目标 ${macMajor} 不一致`,
                );
        }
    }
    const platformOs = new Set(data.platforms.map((p) => String(p?.os ?? "").toLowerCase()));
    /** os 为 osName 的平台发布的格式；any 取全部平台。 */
    const shippedFormats = (osName) =>
        new Set(
            data.platforms
                .filter((p) => osName === "any" || String(p?.os ?? "").toLowerCase() === osName)
                .flatMap((p) => (Array.isArray(p?.formats) ? p.formats : [])),
        );

    // 宿主
    const seen = new Set();
    for (const h of data.hosts) {
        const name = h?.host ?? "(没有 host)";
        for (const f of HOST_FIELDS)
            if (h?.[f] === undefined) add("tiers-shape", `${TIERS_FILE} 的宿主「${name}」缺字段 ${f}`);
        if (h?.notes !== undefined && (!isObj(h.notes) || !h.notes.en || !h.notes.zh))
            add("tiers-shape", `${TIERS_FILE} 的宿主「${name}」的 notes 缺 en 或 zh`);
        if (h?.checks !== undefined) {
            if (!isObj(h.checks)) add("tiers-shape", `${TIERS_FILE} 的宿主「${name}」的 checks 不是对象`);
            else
                for (const k of CHECK_FIELDS)
                    if (typeof h.checks[k] !== "boolean")
                        add("tiers-shape", `${TIERS_FILE} 的宿主「${name}」的 checks.${k} 不是布尔值`);
        }
        if (h?.formats !== undefined && (!Array.isArray(h.formats) || h.formats.some((f) => typeof f !== "string")))
            add("tiers-shape", `${TIERS_FILE} 的宿主「${name}」的 formats 不是字符串数组`);
        if (h?.evidence !== undefined && !Array.isArray(h.evidence))
            add("tiers-shape", `${TIERS_FILE} 的宿主「${name}」的 evidence 不是数组`);

        if (h?.tier !== undefined && !isTier(h.tier))
            add("tier-values", `${TIERS_FILE} 的宿主「${name}」的 tier「${h.tier}」不在登记的取值内`);
        if (h?.targetTier !== undefined && !isTier(h.targetTier))
            add("tier-values", `${TIERS_FILE} 的宿主「${name}」的 targetTier「${h.targetTier}」不在登记的取值内`);
        if (h?.status !== undefined && !isStatus(h.status))
            add("status-values", `${TIERS_FILE} 的宿主「${name}」的 status「${h.status}」不在登记的取值内`);

        const key = `${h?.host}|${h?.versions}|${h?.os}`;
        if (seen.has(key))
            add("hosts-unique", `${TIERS_FILE} 的宿主「${name}」（版本 ${h?.versions}，${h?.os}）写了不止一行`);
        seen.add(key);

        const evidence = Array.isArray(h?.evidence) ? h.evidence : [];
        const tier = h?.tier;
        const status = h?.status;
        const target = h?.targetTier;

        if (status === "awaiting-test") {
            if (!isNull(tier)) add("awaiting-null", `${TIERS_FILE} 的宿主「${name}」是 awaiting-test，tier 应为 null`);
            if (evidence.length)
                add("awaiting-null", `${TIERS_FILE} 的宿主「${name}」是 awaiting-test，evidence 应为空`);
        }
        if (isNum(tier) && isTier(tier)) {
            if (tier === 1 || tier === 2) {
                if (!evidence.length)
                    add("tier-evidence", `${TIERS_FILE} 的宿主「${name}」是 Tier ${tier}，evidence 不能为空`);
                else if (!evidence.some((e) => typeof e === "string" && e.startsWith("validation:")))
                    add(
                        "tier-evidence",
                        `${TIERS_FILE} 的宿主「${name}」是 Tier ${tier}，要有真机的 validation 记录，光有 ci 证据是 Tier 3`,
                    );
            } else if (!evidence.some((e) => typeof e === "string" && e.startsWith("ci:")))
                add("tier-evidence", `${TIERS_FILE} 的宿主「${name}」是 Tier 3，至少要有一条 ci 证据`);
            for (const item of evidence) {
                const state = evidenceState(root, item, jobs);
                if (state === "malformed")
                    add("tier-evidence", `${TIERS_FILE} 的宿主「${name}」的证据「${item}」形态不认识`);
                else if (state !== "ok") add("tier-evidence", `${TIERS_FILE} 的宿主「${name}」的证据「${item}」不成立`);
                else if (item.startsWith("validation:")) {
                    // 记录要是这一行自己的：整个宿主名、版本、系统都对得上，别拿同族宿主或别的版本的记录当证据
                    const why = recordProblem(h, item, root);
                    if (why) add("tier-evidence", `${TIERS_FILE} 的宿主「${name}」的证据「${item}」${why}`);
                }
            }
        }
        if (tier === 1 && isObj(h?.checks)) {
            const falseOnes = Object.entries(h.checks)
                .filter(([, v]) => v !== true)
                .map(([k]) => k);
            if (falseOnes.length)
                add(
                    "tier1-checks",
                    `${TIERS_FILE} 的宿主「${name}」是 Tier 1，checks 的 ${falseOnes.join("、")} 还要 true`,
                );
        }
        // status 与等级自洽：verified 当且仅当 tier ≤ targetTier
        if (status === "verified") {
            if (!isNum(tier) || !isNum(target) || tier > target)
                add(
                    "verified-consistent",
                    `${TIERS_FILE} 的宿主「${name}」是 verified，但 tier（${tier}）与 targetTier（${target}）不自洽`,
                );
        } else if (status === "partial") {
            // partial = 只验证了一部分，还没到目标：tier 必须是具体等级，且比目标低（数字更大）。
            // tier ≤ targetTier 就已经达到目标，该写 verified。
            if (!isNum(tier) || tier < 1 || tier > 3)
                add(
                    "verified-consistent",
                    `${TIERS_FILE} 的宿主「${name}」是 partial，tier（${tier}）应为 1…3 的具体等级`,
                );
            else if (!isNum(target))
                add(
                    "verified-consistent",
                    `${TIERS_FILE} 的宿主「${name}」是 partial，targetTier（${target}）应为具体等级`,
                );
            else if (tier <= target)
                add(
                    "verified-consistent",
                    `${TIERS_FILE} 的宿主「${name}」是 partial，但 tier（${tier}）已达到 targetTier（${target}），status 应为 verified`,
                );
        } else if (status === "awaiting-test" || status === "out-of-scope") {
            if (!isNull(tier))
                add("verified-consistent", `${TIERS_FILE} 的宿主「${name}」是 ${status}，tier 应为 null`);
        }
        if (status === "out-of-scope") {
            if (evidence.length) add("out-of-scope", `${TIERS_FILE} 的宿主「${name}」是 out-of-scope，evidence 应为空`);
            if (target !== "out-of-scope")
                add("out-of-scope", `${TIERS_FILE} 的宿主「${name}」是 out-of-scope，targetTier 应为 out-of-scope`);
        }
        // 平台与格式：out-of-scope 的行本来就是「不在我们发布的平台里」，不核对
        if (status !== "out-of-scope" && typeof h?.os === "string" && Array.isArray(h?.formats)) {
            const osName = h.os.toLowerCase();
            if (osName !== "any" && !platformOs.has(osName))
                add("hosts-platform", `${TIERS_FILE} 的宿主「${name}」的 os「${h.os}」不在 platforms 里，也不是 any`);
            else {
                const shipped = shippedFormats(osName);
                const extra = h.formats.filter((f) => !shipped.has(f));
                if (extra.length || !h.formats.length)
                    add(
                        "hosts-platform",
                        `${TIERS_FILE} 的宿主「${name}」在 ${h.os} 上的 formats（${h.formats.join("、") || "空"}）不是平台所发布格式（${[...shipped].join("、")}）的非空子集`,
                    );
            }
        }
    }
    return out;
}

export async function selfTest(root = ROOT) {
    const jobs = new Set(["core-sanitizers"]);
    const checks = (v) => Object.fromEntries(CHECK_FIELDS.map((k) => [k, v]));
    const host = (over = {}) => ({
        host: "LUNA",
        versions: "3.x",
        os: "macos",
        formats: ["VST3", "AU"],
        tier: null,
        targetTier: 2,
        status: "awaiting-test",
        evidence: [],
        checks: checks(false),
        notes: { en: "Not tested yet.", zh: "尚未测试。" },
        ...over,
    });
    const rec = (f) => `validation:docs/validation/${f}`;
    const cubaseRecord = rec("2026-11-02-cubase-pro-15-win.md");
    const lunaRecord = "validation:docs/validation/2026-11-03-luna-macos.md";
    const cubase = (over = {}) =>
        host({
            host: "Cubase Pro",
            versions: "15",
            os: "windows",
            formats: ["VST3"],
            tier: 1,
            targetTier: 1,
            status: "verified",
            evidence: [cubaseRecord],
            checks: checks(true),
            notes: { en: "Verified.", zh: "已验证。" },
            ...over,
        });
    const luna = host();
    /** 部分验证的 Windows 行（Tier 2，目标 Tier 1）：只有证据这一项是被测的变量。 */
    const partial2 = (over = {}) => cubase({ tier: 2, status: "partial", checks: checks(false), ...over });
    const good = {
        version: 1,
        platforms: [
            { id: "win-x64", os: "Windows", arch: "x64", formats: ["VST3"], minVersion: "Windows 10" },
            { id: "mac-arm64", os: "macOS", arch: "arm64", formats: ["VST3", "AU"], minVersion: "macOS 11" },
        ],
        hosts: [cubase(), luna],
    };
    const withHosts = (...hosts) => ({ ...good, hosts });
    const without = (h, k) => {
        const c = { ...h };
        delete c[k];
        return c;
    };
    // 临时目录里放一批 validation 记录，Tier 1 / 2 的证据要真的存在。
    // 同族宿主（Cubase 三个版本、Studio One 与 FL Studio）与别的版本、别的系统都各放一份，用来证明它们互相不能作证；
    // 文件名没写版本的记录（luna、reaper）靠正文里的「宿主名 版本」对版本。
    const records = {
        "2026-11-02-cubase-pro-15-win.md": "Cubase Pro 15.0.30 on Windows 11\n",
        "2026-11-02-cubase-pro-14-win.md": "Cubase Pro 14.0.5 on Windows 11\n",
        "2026-11-02-cubase-pro-15-macos.md": "Cubase Pro 15.0.30 on macOS 14\n",
        // 系统后缀的旧写法（windows / mac）：文件存在也不能作证
        "2026-11-02-cubase-pro-15-windows.md": "Cubase Pro 15.0.30 on Windows 11\n",
        "2026-11-02-cubase-pro-15-mac.md": "Cubase Pro 15.0.30 on macOS 14\n",
        "2026-11-02-cubase-elements-15-win.md": "Cubase Elements 15 on Windows 11\n",
        "2026-11-04-studio-one-win.md": "Studio One 7 on Windows 11\n",
        "2026-11-04-fl-studio-win.md": "FL Studio 2025 on Windows 11\n",
        "2026-11-03-luna-macos.md": "LUNA 3.1 on macOS 14\n",
        "2026-11-05-reaper-win.md": "REAPER 7.20 on Windows 11\n",
        "2026-11-06-reaper-win.md": "REAPER 6.80 on Windows 11\n",
        "cubase-pro-15-win.md": "Cubase Pro 15 on Windows 11\n",
    };
    // 临时目录的建与删都走 scripts/lib/safe-rm.mjs
    const tmp = makeTempDir("rinne-tiers-");
    const outside = makeTempDir("rinne-tiers-out-");
    let cases;
    try {
        fs.mkdirSync(path.join(tmp, "docs", "validation"), { recursive: true });
        for (const [f, body] of Object.entries(records))
            fs.writeFileSync(path.join(tmp, "docs", "validation", f), body);
        // 仓根之外有一份文件名完全合规的记录：用 .. 绕出去的路径在旧写法下会读到它、算作成立的证据
        const escapeRepo = path.join(outside, "repo");
        fs.mkdirSync(path.join(escapeRepo, "docs", "validation"), { recursive: true });
        fs.writeFileSync(path.join(outside, "2026-11-02-cubase-pro-15-win.md"), "Cubase Pro 15.0.30 on Windows 11\n");
        const run = (data, opts = {}) =>
            tierProblems({ data: data ?? good, root: tmp, jobs, formats: ["VST3", "AU"], macMajor: 11, ...opts });
        cases = [
            ["一条 Tier 1 与一条未测的行都合规", [], null],
            [
                "partial 的行比目标低（Tier 2 对目标 Tier 1）合规",
                [],
                withHosts(cubase({ tier: 2, status: "partial", checks: checks(false) })),
            ],
            [
                "partial 的行是 Tier 3（对目标 Tier 2，只有 CI 证据）合规",
                [],
                withHosts(cubase(), host({ tier: 3, status: "partial", evidence: ["ci:core-sanitizers"] })),
            ],
            [
                "Tier 3 对目标 Tier 3 写 verified 并带 CI 证据合规",
                [],
                withHosts(
                    cubase(),
                    host({
                        host: "REAPER",
                        versions: "7",
                        os: "windows",
                        formats: ["VST3"],
                        tier: 3,
                        targetTier: 3,
                        status: "verified",
                        evidence: ["ci:core-sanitizers"],
                    }),
                ),
            ],
            [
                "partial 却已达到目标（Tier 2 对目标 Tier 2）：该写 verified",
                ["verified-consistent"],
                withHosts(cubase(), host({ tier: 2, status: "partial", evidence: [lunaRecord] })),
            ],
            [
                "partial 却已达到目标（Tier 1 对目标 Tier 1）",
                ["verified-consistent"],
                withHosts(cubase({ status: "partial" })),
            ],
            [
                "tier 比 targetTier 好却写 partial",
                ["verified-consistent"],
                withHosts(
                    cubase(),
                    host({ tier: 1, targetTier: 2, status: "partial", evidence: [lunaRecord], checks: checks(true) }),
                ),
            ],
            [
                "partial 的 targetTier 不是具体等级",
                ["verified-consistent"],
                withHosts(
                    cubase(),
                    host({ tier: 3, targetTier: "out-of-scope", status: "partial", evidence: ["ci:core-sanitizers"] }),
                ),
            ],
            [
                "partial 却没给 tier",
                ["verified-consistent"],
                withHosts(cubase(), host({ tier: null, status: "partial", evidence: ["ci:core-sanitizers"] })),
            ],
            [
                "partial 的 tier 不在 1…3 内",
                ["verified-consistent", "tier-values"],
                withHosts(cubase(), host({ tier: 4, status: "partial", evidence: ["ci:core-sanitizers"] })),
            ],
            ["Tier 1 的 evidence 为空", ["tier-evidence"], withHosts(cubase({ evidence: [] }), luna)],
            [
                "Tier 1 只有 ci 证据，没有真机记录",
                ["tier-evidence"],
                withHosts(cubase({ evidence: ["ci:core-sanitizers"] }), luna),
            ],
            [
                "Tier 3 verified 没有 ci 证据",
                ["tier-evidence"],
                withHosts(
                    cubase(),
                    host({
                        host: "REAPER",
                        versions: "7",
                        os: "windows",
                        formats: ["VST3"],
                        tier: 3,
                        targetTier: 3,
                        status: "verified",
                        evidence: [],
                    }),
                ),
            ],
            [
                "Tier 2 的证据指向不存在的 validation 文件",
                ["tier-evidence"],
                withHosts(
                    cubase({
                        tier: 2,
                        status: "partial",
                        evidence: [rec("2026-11-02-cubase-pro-15-nowhere-win.md")],
                        checks: checks(false),
                    }),
                ),
            ],
            ["Tier 1 拿别的宿主的记录当证据", ["tier-evidence"], withHosts(cubase({ evidence: [lunaRecord] }), luna)],
            // 记录必须是这一行自己的：整个宿主名、版本、系统、文件名形态都对得上
            [
                "Studio One 的记录不能给 FL Studio 作证",
                ["tier-evidence"],
                withHosts(
                    partial2({ host: "FL Studio", versions: "any", evidence: [rec("2026-11-04-studio-one-win.md")] }),
                ),
            ],
            [
                "FL Studio 用自己的记录作证合规",
                [],
                withHosts(
                    partial2({ host: "FL Studio", versions: "any", evidence: [rec("2026-11-04-fl-studio-win.md")] }),
                ),
            ],
            [
                "Cubase Pro 的记录不能给 Cubase Elements 作证",
                ["tier-evidence"],
                withHosts(partial2({ host: "Cubase Elements", evidence: [cubaseRecord] })),
            ],
            [
                "Cubase Elements 用自己的记录作证合规",
                [],
                withHosts(
                    partial2({ host: "Cubase Elements", evidence: [rec("2026-11-02-cubase-elements-15-win.md")] }),
                ),
            ],
            [
                "更泛的宿主名（Cubase）不能拿 Cubase Pro 的记录作证",
                ["tier-evidence"],
                withHosts(partial2({ host: "Cubase", evidence: [cubaseRecord] })),
            ],
            // 版本写 any 的行不核对版本，宿主名只能靠文件名的结构挡住：多出来的词（pro、elements）不许混过去
            [
                "版本写 any 的泛宿主名（Cubase）也不能拿 Cubase Pro 的记录作证",
                ["tier-evidence"],
                withHosts(partial2({ host: "Cubase", versions: "any", evidence: [cubaseRecord] })),
            ],
            [
                "版本写 any 的 Cubase Elements 也不能拿 Cubase Pro 的记录作证",
                ["tier-evidence"],
                withHosts(partial2({ host: "Cubase Elements", versions: "any", evidence: [cubaseRecord] })),
            ],
            [
                "文件名里的版本（14）与这一行的版本（15）对不上",
                ["tier-evidence"],
                withHosts(partial2({ evidence: [rec("2026-11-02-cubase-pro-14-win.md")] })),
            ],
            [
                "文件名没写版本，正文里有这一行的版本（LUNA 3.x 对 LUNA 3.1）合规",
                [],
                withHosts(host({ tier: 2, status: "verified", evidence: [lunaRecord], checks: checks(false) })),
            ],
            [
                "文件名没写版本，正文里的版本（6.80）对不上这一行的版本（7）",
                ["tier-evidence"],
                withHosts(partial2({ host: "REAPER", versions: "7", evidence: [rec("2026-11-06-reaper-win.md")] })),
            ],
            [
                "文件名没写版本，正文里有这一行的版本（REAPER 7.20）合规",
                [],
                withHosts(partial2({ host: "REAPER", versions: "7", evidence: [rec("2026-11-05-reaper-win.md")] })),
            ],
            [
                "版本写 any 的行不核对版本",
                [],
                withHosts(
                    partial2({ host: "FL Studio", versions: "any", evidence: [rec("2026-11-04-fl-studio-win.md")] }),
                ),
            ],
            [
                "Windows 的记录不能给 macOS 的行作证",
                ["tier-evidence"],
                withHosts(partial2({ os: "macos", formats: ["VST3", "AU"], evidence: [cubaseRecord] })),
            ],
            [
                "macOS 的行用 macOS 的记录作证合规",
                [],
                withHosts(
                    partial2({
                        os: "macos",
                        formats: ["VST3", "AU"],
                        evidence: [rec("2026-11-02-cubase-pro-15-macos.md")],
                    }),
                ),
            ],
            // 系统后缀只有一种写法（win / macos / linux）：生成与校验是同一个函数
            [
                "Windows 的行拿 -windows.md 的记录作证：后缀不是登记的写法（win），文件存在也不行",
                ["tier-evidence"],
                withHosts(partial2({ evidence: [rec("2026-11-02-cubase-pro-15-windows.md")] })),
            ],
            [
                "macOS 的行拿 -mac.md 的记录作证：后缀不是登记的写法（macos），文件存在也不行",
                ["tier-evidence"],
                withHosts(
                    partial2({
                        os: "macos",
                        formats: ["VST3", "AU"],
                        evidence: [rec("2026-11-02-cubase-pro-15-mac.md")],
                    }),
                ),
            ],
            [
                "系统写 any 的行认全部登记的后缀（-macos.md）",
                [],
                withHosts(partial2({ os: "any", evidence: [rec("2026-11-02-cubase-pro-15-macos.md")] })),
            ],
            [
                "系统写 any 的行也不认 -mac.md",
                ["tier-evidence"],
                withHosts(partial2({ os: "any", evidence: [rec("2026-11-02-cubase-pro-15-mac.md")] })),
            ],
            [
                "系统没有登记后缀（freebsd）的行不能用验证记录作证",
                ["tier-evidence"],
                withHosts(partial2({ os: "freebsd", evidence: [cubaseRecord] })),
            ],
            // 记录路径只准是仓内的规范相对路径（与 claims 的 validation 证据同一个函数）
            [
                "validation 证据用 .. 绕到仓根之外的真实记录（文件名完全合规）：不当证据，旧写法会读到它",
                ["tier-evidence"],
                withHosts(partial2({ evidence: ["validation:docs/../../2026-11-02-cubase-pro-15-win.md"] })),
                { root: escapeRepo },
            ],
            ...[
                ["validation:docs/../../x", "docs/../../x"],
                ["validation:/etc/x", "/etc/x"],
                ["validation:docs\\x", "docs\\x"],
                ["validation:C:/x-win.md", "盘符"],
                ["validation:docs//x-win.md", "空段"],
            ].map(([item, label]) => [
                `validation 证据「${label}」：形状错误`,
                ["tier-evidence"],
                withHosts(partial2({ evidence: [item] })),
            ]),
            [
                "记录文件名没有日期，不是模板规定的形态",
                ["tier-evidence"],
                withHosts(partial2({ evidence: [rec("cubase-pro-15-win.md")] })),
            ],
            [
                "awaiting-test 却给了 tier",
                ["awaiting-null", "verified-consistent"],
                withHosts(cubase(), host({ tier: 1 })),
            ],
            [
                "awaiting-test 却有 evidence",
                ["awaiting-null"],
                withHosts(cubase(), host({ evidence: ["ci:core-sanitizers"] })),
            ],
            [
                "Tier 1 但 checks 里有 false",
                ["tier1-checks"],
                withHosts(cubase({ checks: { ...checks(true), realtime: false } }), luna),
            ],
            [
                "verified 但 tier 比 targetTier 低（数字更大）",
                ["verified-consistent"],
                withHosts(cubase({ tier: 3 }), luna),
            ],
            ["缺 notes.zh", ["tiers-shape"], withHosts(cubase(), host({ notes: { en: "x" } }))],
            ["notes 是 null", ["tiers-shape"], withHosts(cubase(), host({ notes: null }))],
            ["checks 是 null", ["tiers-shape"], withHosts(cubase(), host({ checks: null }))],
            [
                "checks 缺一个键",
                ["tiers-shape"],
                withHosts(cubase(), host({ checks: without(checks(false), "freeze") })),
            ],
            [
                "out-of-scope 却带 evidence",
                ["out-of-scope"],
                withHosts(
                    cubase(),
                    host({ status: "out-of-scope", targetTier: "out-of-scope", evidence: ["ci:core-sanitizers"] }),
                ),
            ],
            ["出现未登记的 tier 取值", ["tier-values"], withHosts(cubase(), host({ tier: 4 }))],
            ["出现未登记的 status 取值", ["status-values"], withHosts(cubase(), host({ status: "maybe" }))],
            ["出现未登记的 targetTier 取值", ["tier-values"], withHosts(cubase(), host({ targetTier: 7 }))],
            ["顶层缺 hosts", ["tiers-shape"], { version: 1, platforms: good.platforms }],
            ["host 的 formats 不是字符串数组", ["tiers-shape"], withHosts(cubase(), host({ formats: "VST3" }))],
            ["host 的 evidence 不是数组", ["tiers-shape"], withHosts(cubase(), host({ evidence: "x" }))],
            [
                "Tier 1 的证据里混进一条形态不认识的",
                ["tier-evidence"],
                withHosts(cubase({ evidence: [cubaseRecord, "wiki:x"] }), luna),
            ],
            [
                "out-of-scope 的 targetTier 不是 out-of-scope",
                ["out-of-scope"],
                withHosts(cubase(), host({ status: "out-of-scope", targetTier: 2 })),
            ],
            ["host 的 formats 是空数组", ["hosts-platform"], withHosts(cubase({ formats: [] }), luna)],
            ["没有读到 identity.json 登记的格式", ["platforms-shape"], null, { formats: null }],
            ["host 缺字段", ["tiers-shape"], { ...good, hosts: [{ host: "X" }] }],
            ["同一宿主同系统同版本写了两行", ["hosts-unique"], withHosts(cubase(), luna, host())],
            [
                "同一宿主两个系统各一行合规",
                [],
                withHosts(cubase(), host({ host: "Cubase Pro", versions: "15", os: "macos", targetTier: 3 })),
            ],
            ["Windows 上的宿主写了 AU", ["hosts-platform"], withHosts(cubase({ formats: ["VST3", "AU"] }), luna)],
            ["os 不在 platforms 里也不是 any", ["hosts-platform"], withHosts(cubase(), host({ os: "linux" }))],
            ["os 为 any 的行 formats 取全部平台的并集合规", [], withHosts(cubase(), host({ os: "any" }))],
            [
                "out-of-scope 的行不核对平台与格式",
                [],
                withHosts(
                    cubase(),
                    host({
                        host: "Pro Tools",
                        versions: "any",
                        os: "any",
                        formats: ["AAX"],
                        status: "out-of-scope",
                        targetTier: "out-of-scope",
                    }),
                ),
            ],
            ["平台缺字段", ["platforms-shape"], { ...good, platforms: [{ id: "win-x64" }] }],
            [
                "平台的格式不在 identity.json 登记的集合内",
                ["platforms-shape"],
                { ...good, platforms: [{ ...good.platforms[0], formats: ["LV2"] }] },
            ],
            ["平台 id 重复", ["platforms-shape"], { ...good, platforms: [good.platforms[0], good.platforms[0]] }],
            [
                "macOS 最低版本与 CMake 部署目标不一致",
                ["platforms-shape"],
                { ...good, platforms: [good.platforms[0], { ...good.platforms[1], minVersion: "macOS 12" }] },
            ],
        ].map(([label, expect, data, opts]) => {
            const errors = run(data ?? null, opts);
            const missed = expect.filter((r) => !errors.some((e) => e.startsWith(`[${r}] `)));
            return {
                label: missed.length ? `${label}（没有触发 ${missed.join("、")}）` : label,
                bad: !!expect.length,
                passed: expect.length ? missed.length === 0 : errors.length === 0,
                expect,
            };
        });
        // 直接调 recordProblem：路径不合格就在读文件之前拒绝，不去碰仓外的文件
        for (const [label, item, root] of [
            ["docs/../../x", "validation:docs/../../x", tmp],
            ["/etc/x", "validation:/etc/x", tmp],
            ["docs\\x", "validation:docs\\x", tmp],
            ["C:/x-win.md", "validation:C:/x-win.md", tmp],
            ["仓外的真实记录", "validation:docs/../../2026-11-02-cubase-pro-15-win.md", escapeRepo],
        ])
            cases.push({
                label: `recordProblem 直接拒绝「${label}」`,
                bad: true,
                passed: String(recordProblem(cubase(), item, root)).startsWith("路径"),
                expect: [],
            });
        cases.push({
            label: "recordProblem：仓内规范路径、文件名与这一行对得上：放行",
            bad: false,
            passed: recordProblem(cubase(), cubaseRecord, tmp) === null,
            expect: [],
        });
    } finally {
        cleanupTempDir(tmp);
        cleanupTempDir(outside);
    }
    const unnamed = Object.keys(RULES).filter((r) => !cases.some((c) => c.expect.includes(r)));
    cases.push({
        label: `每条判据都有坏样例点名${unnamed.length ? `（没有：${unnamed.join("、")}）` : ""}`,
        bad: false,
        passed: unnamed.length === 0,
        expect: [],
    });
    // 共用 daw-tiers.json 的两个生成块模块：它们的自测在这里一并跑
    cases.push(...dawTiersCases().map((c) => ({ ...c, label: `daw-tiers 块：${c.label}` })));
    cases.push(...platformBadgeCases().map((c) => ({ ...c, label: `platform-badge 块：${c.label}` })));
    return caseResult(cases);
}

export default {
    name: "tiers",
    group: "docs",
    inputs: [],
    run(ctx) {
        const text = readText(path.join(ctx.root, TIERS_FILE));
        if (text === null) return { ok: false, lines: [`[tiers-shape] 缺 ${TIERS_FILE}`] };
        let data;
        try {
            data = JSON.parse(text);
        } catch (e) {
            return { ok: false, lines: [`[tiers-shape] ${TIERS_FILE} 不是合法 JSON：${e.message}`] };
        }
        const problems = tierProblems({ data, root: ctx.root });
        return { ok: problems.length === 0, lines: problems };
    },
    selfTest: (ctx) => selfTest(ctx.root),
};
