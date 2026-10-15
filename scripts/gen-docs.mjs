// SPDX-License-Identifier: GPL-3.0-or-later
//
// 文档生成器：全仓唯一的一个，没有第二个。块模块放在 scripts/gen-docs.d/<name>.mjs，只新增模块、不改本引擎。
//
// 块模块协议（发布后不再改动；要改先走维护者裁定）：默认导出
//   name      块名，等于文件名
//   sources   生成所依据的真源文件（仓内相对路径数组），仅用于说明
//   surfaces  落地面数组，每项 { file, id, required? }：file 里用一对标记圈出生成区
//               <!-- BEGIN gen:<name>:<id> -->
//               …生成内容…
//               <!-- END gen:<name>:<id> -->
//             文件不存在，或存在但还没有这对标记，称为「未接入」，--check 与 --write 都跳过它；
//             required 为真时未接入判红。一接入就逐字节比对，所以接入一个落地面 = 在文件里放上一对标记再 --write。
//   render    render(ctx, id) -> 区块正文（字符串，LF 换行，写入时补一个结尾换行）；真源有问题时抛错
//   validate  可选，validate(ctx) -> 问题字符串数组；不依赖落地面的真源检查（如译文是否过期、假变量试渲染）
//   emit      可选，emit(ctx, opts) -> 字符串，供 --emit <name> 输出到 stdout；opts = { platform, vars }
// ctx = { root }，root 是仓根。
//
// 命令行：
//   node scripts/gen-docs.mjs --check                          逐字节比对全部已接入的落地面，另跑各模块的 validate
//   node scripts/gen-docs.mjs --write                          回写全部落地面（真源有问题时拒绝回写）
//   node scripts/gen-docs.mjs --emit <name> [--platform win|mac] [--vars <json 或 json 文件>]
//   node scripts/gen-docs.mjs --print-hashes                   打印中文真源各段的 sha256，更新译文时抄进 i18n 文件
//   node scripts/gen-docs.mjs --emit notices-provenance | --print-entity-hash
//                                                              入口已预留，对应模块加入后才有输出

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import {
    findRegion,
    generatedMarkerNames,
    loadSource,
    readRepoText,
    replaceRegion,
    zhHashes,
} from "./lib/docs-kit.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const MODULE_DIR = path.join(HERE, "gen-docs.d");
const ROOT = path.resolve(HERE, "..");
const blockName = (mod, id) => `gen:${mod.name}:${id}`;

/** 读出全部块模块，形状不对时抛错（不悄悄跳过）。 */
export async function loadModules() {
    const mods = [];
    for (const file of fs
        .readdirSync(MODULE_DIR)
        .filter((f) => f.endsWith(".mjs"))
        .sort()) {
        const m = (await import(pathToFileURL(path.join(MODULE_DIR, file)).href)).default;
        const name = file.slice(0, -4);
        const bad =
            !m || m.name !== name
                ? `name 应为 ${name}`
                : !Array.isArray(m.sources) || !Array.isArray(m.surfaces) || typeof m.render !== "function"
                  ? "缺 sources / surfaces / render"
                  : null;
        if (bad) throw new Error(`gen-docs.d/${file}：${bad}`);
        mods.push(m);
    }
    return mods;
}

/** 落地面的状态：absent（无文件）、unadopted（无标记）、ok、stale（与生成结果不一致）。 */
function surfaceState(ctx, mod, s) {
    const text = readRepoText(ctx.root, s.file);
    if (text === null) return { state: "absent", text };
    const region = findRegion(text, blockName(mod, s.id));
    if (!region) return { state: "unadopted", text };
    const want = mod.render(ctx, s.id).replace(/\n*$/, "\n");
    return { state: region.body === want ? "ok" : "stale", text, want };
}

/** 模块的 validate 结果；validate 自己抛错也算一条问题。 */
function validateOf(mod, ctx) {
    try {
        return (mod.validate && mod.validate(ctx)) || [];
    } catch (e) {
        return [e.message];
    }
}

/** 仓根与 docs/ 下的 Markdown（相对路径），用来找没有对应块模块的生成块标记。 */
function markdownFiles(root) {
    const out = [];
    const walk = (rel) => {
        for (const e of fs.readdirSync(path.join(root, rel), { withFileTypes: true })) {
            const p = rel ? `${rel}/${e.name}` : e.name;
            if (e.isDirectory() && p.startsWith("docs")) walk(p);
            else if (e.isFile() && p.endsWith(".md")) out.push(p);
        }
    };
    walk("");
    return out;
}

/** 全量检查，返回问题数组；空数组即通过。 */
export async function checkAll(root = ROOT, mods) {
    const ctx = { root };
    const out = [];
    const files = new Set();
    const declared = new Set();
    for (const mod of mods ?? (await loadModules())) {
        let broken = false;
        for (const p of validateOf(mod, ctx)) {
            out.push(`${mod.name}：${p}`);
            broken = true;
        }
        for (const s of mod.surfaces) {
            files.add(s.file);
            declared.add(`${s.file}|${blockName(mod, s.id)}`);
        }
        if (broken) continue; // 真源有问题时渲染没有意义
        for (const s of mod.surfaces) {
            try {
                const r = surfaceState(ctx, mod, s);
                if (r.state === "stale")
                    out.push(`${s.file} 的生成块 ${blockName(mod, s.id)} 与真源不一致（改真源后运行 --write）`);
                else if (s.required && r.state !== "ok") out.push(`${s.file} 缺必选的生成块 ${blockName(mod, s.id)}`);
            } catch (e) {
                out.push(`${s.file}：${e.message}`);
            }
        }
    }
    for (const f of new Set([...files, ...markdownFiles(root)])) {
        const text = readRepoText(root, f);
        for (const n of text === null ? [] : generatedMarkerNames(text)) {
            if (!declared.has(`${f}|${n}`))
                out.push(`${f} 里的标记 ${n} 不是任何块模块声明的落地面（拼写或路径错误？）`);
        }
    }
    return out;
}

/** 回写全部已接入的落地面，返回改动的文件。真源有问题时抛错、不写。 */
export async function writeAll(root = ROOT, mods) {
    const ctx = { root };
    mods ??= await loadModules();
    const fatal = mods.flatMap((m) => validateOf(m, ctx).map((p) => `${m.name}：${p}`));
    if (fatal.length) throw new Error(`真源有问题，不回写：\n  ${fatal.join("\n  ")}`);
    const changed = new Set();
    for (const mod of mods) {
        for (const s of mod.surfaces) {
            const r = surfaceState(ctx, mod, s);
            if (r.state !== "stale") continue;
            fs.writeFileSync(path.join(root, s.file), replaceRegion(r.text, blockName(mod, s.id), r.want));
            changed.add(s.file);
        }
    }
    return [...changed];
}

/** 解析 --vars：JSON 字符串，或指向 JSON 文件的路径。 */
export function parseVars(arg) {
    if (!arg) return {};
    return JSON.parse(arg.trim().startsWith("{") ? arg : fs.readFileSync(arg, "utf8"));
}

const RESERVED = { "notices-provenance": "notices-provenance", "print-entity-hash": "entity-hash" };

async function main(argv) {
    const opt = (k) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : undefined);
    if (argv.includes("--check")) {
        const problems = await checkAll();
        problems.forEach((p) => console.error(`gen-docs：${p}`));
        console.log(problems.length ? `gen-docs --check：${problems.length} 处问题` : "gen-docs --check：通过");
        return problems.length ? 1 : 0;
    }
    if (argv.includes("--write")) {
        const changed = await writeAll();
        console.log(changed.length ? `gen-docs --write：已更新 ${changed.join("、")}` : "gen-docs --write：无需更新");
        return 0;
    }
    if (argv.includes("--print-hashes")) {
        const h = zhHashes(loadSource(ROOT));
        console.log(JSON.stringify(h, null, 2));
        return 0;
    }
    const emit = opt("--emit") ?? (argv.includes("--print-entity-hash") ? "print-entity-hash" : undefined);
    if (emit) {
        const want = RESERVED[emit] ?? emit;
        const mod = (await loadModules()).find((m) => m.name === want);
        if (!mod?.emit) {
            console.error(`gen-docs：没有可输出 ${emit} 的块模块（${want} 尚未加入）`);
            return 2;
        }
        process.stdout.write(mod.emit({ root: ROOT }, { platform: opt("--platform"), vars: parseVars(opt("--vars")) }));
        return 0;
    }
    console.error("用法：node scripts/gen-docs.mjs --check | --write | --emit <name> | --print-hashes（见文件头）");
    return 2;
}

if (process.argv[1] && fs.realpathSync(process.argv[1]) === fs.realpathSync(fileURLToPath(import.meta.url))) {
    main(process.argv.slice(2)).then(
        (code) => (process.exitCode = code),
        (e) => {
            console.error(`gen-docs：${e.message}`);
            process.exitCode = 1;
        },
    );
}
