// SPDX-License-Identifier: GPL-3.0-or-later
//
// mac 构建产物的身份核对：构建目录里恰好有本次身份的 2 个 .vst3 与 2 个 .component，逐个核
//   - 目录名、CFBundleIdentifier、CFBundleExecutable 对应的主程序存在且可执行；
//   - 架构集合：正式构建恰好 arm64，探针构建恰好 arm64 与 x86_64（universal）；
//   - 每个架构切片的最低系统版本（LC_BUILD_VERSION 的 minos）等于部署目标；
//   - .component 的 Info.plist 恰好声明一个 AudioComponent，类型 / 子类型 / 厂商是 aufx / 插件码 / Snch。
// 期望值一律来自 tests/golden/identity.json（与 contract-static-identity 同一份 golden），脚本里不写第二份。
//   node scripts/ci/mac/check-bundles.mjs --build <构建目录> --minos <部署目标> [--probe] [--list <文件>] [--triples <文件>]
//   node scripts/ci/mac/check-bundles.mjs --self-test
// --list 把通过核对的 bundle 路径逐行写进文件，--triples 把 AU 三元组逐行写进文件（类型 子类型 厂商），给后面的 auval 步骤用。

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..", "..");

/** AU_MAIN_TYPE 到四字符类型码。golden 里只有效果器；别的取值说明身份变了，直接报错。 */
const AU_TYPES = { kAudioUnitType_Effect: "aufx" };

/** 从 golden 推出本次身份的期望：[{ role, name, code, bundleId }] 与共用的厂商码、AU 类型。 */
export function expectedIdentity(golden, probe) {
    const flavor = probe ? "probe" : "formal";
    const plugins = Object.entries(golden.plugins).map(([role, p]) => ({
        role,
        name: p.identity[flavor].PRODUCT_NAME[0],
        code: p.identity[flavor].PLUGIN_CODE[0],
        bundleId: p.identity[flavor].BUNDLE_ID[0],
        mfr: p.shared.PLUGIN_MANUFACTURER_CODE[0],
        auType: AU_TYPES[p.shared.AU_MAIN_TYPE[0]],
    }));
    const bad = plugins.find((p) => !p.auType);
    if (bad) throw new Error(`golden 里 ${bad.role} 的 AU_MAIN_TYPE 不是已知的类型，先在本脚本里登记`);
    return plugins;
}

/** `lipo -archs` 的输出（空格分隔）与期望的架构集合是否相同，顺序无关。 */
export function archsMatch(output, want) {
    const got = output.trim().split(/\s+/).filter(Boolean).sort();
    const exp = [...want].sort();
    return got.length === exp.length && got.every((a, i) => a === exp[i]);
}

/** `otool -l` 输出里所有 LC_BUILD_VERSION 的 minos；每个架构切片一条。 */
export function minosList(otoolOutput) {
    const out = [];
    let inBuild = false;
    for (const line of otoolOutput.split(/\r?\n/)) {
        if (/^\s*cmd LC_BUILD_VERSION\b/.test(line)) inBuild = true;
        else if (/^\s*cmd\s/.test(line)) inBuild = false;
        const m = inBuild ? /^\s*minos\s+(\S+)/.exec(line) : null;
        if (m) out.push(m[1]);
    }
    return out;
}

/** 最低系统版本的核对：条数要等于架构数，且每条都等于部署目标。返回错误文字，对上了返回空串。 */
export function minosError(list, archCount, target) {
    if (list.length !== archCount) return `LC_BUILD_VERSION 有 ${list.length} 条，架构有 ${archCount} 个`;
    const off = list.filter((v) => v !== target);
    return off.length ? `minos 是 ${off.join("、")}，部署目标是 ${target}` : "";
}

/** .component 的 AudioComponents 与期望三元组的核对。返回错误文字，对上了返回空串。 */
export function audioComponentError(plist, want) {
    const list = plist.AudioComponents;
    if (!Array.isArray(list) || list.length !== 1)
        return `AudioComponents 应当恰好一项，现在 ${Array.isArray(list) ? list.length : "没有"}`;
    const c = list[0];
    const got = `${c.type}/${c.subtype}/${c.manufacturer}`;
    const exp = `${want.auType}/${want.code}/${want.mfr}`;
    return got === exp ? "" : `AudioComponents[0] 是 ${got}，期望 ${exp}`;
}

/** 在 dir 下找名字以 ext 结尾的目录（不进 _deps，不进 bundle 内部）。 */
export function findBundles(dir, ext) {
    const found = [];
    const walk = (d) => {
        for (const e of fs.readdirSync(d, { withFileTypes: true })) {
            if (!e.isDirectory() || e.name === "_deps") continue;
            const p = path.join(d, e.name);
            if (e.name.endsWith(ext)) found.push(p);
            else walk(p);
        }
    };
    walk(dir);
    return found;
}

function run(cmd, args) {
    const r = spawnSync(cmd, args, { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
    if (r.status !== 0)
        throw new Error(`${cmd} ${args.join(" ")} 失败：${(r.stderr || r.error || "").toString().trim()}`);
    return r.stdout;
}

const readPlist = (file) => JSON.parse(run("plutil", ["-convert", "json", "-o", "-", file]));

function checkAll({ build, probe, minos }) {
    const golden = JSON.parse(fs.readFileSync(path.join(ROOT, "tests", "golden", "identity.json"), "utf8"));
    const plugins = expectedIdentity(golden, probe);
    const wantArchs = probe ? ["arm64", "x86_64"] : ["arm64"];
    const errors = [];
    const rows = [];
    const bundles = [];
    const triples = [];

    for (const ext of [".vst3", ".component"]) {
        const found = findBundles(build, ext);
        const wantNames = plugins.map((p) => p.name + ext).sort();
        const gotNames = found.map((f) => path.basename(f)).sort();
        if (JSON.stringify(gotNames) !== JSON.stringify(wantNames))
            errors.push(`${ext} 应当恰好是 ${wantNames.join("、")}，现在是 ${gotNames.join("、") || "（没有）"}`);
        for (const p of plugins) {
            const dir = found.find((f) => path.basename(f) === p.name + ext);
            if (!dir) continue;
            const label = p.name + ext;
            const problems = [];
            try {
                const plist = readPlist(path.join(dir, "Contents", "Info.plist"));
                if (plist.CFBundleIdentifier !== p.bundleId)
                    problems.push(`CFBundleIdentifier 是 ${plist.CFBundleIdentifier}，期望 ${p.bundleId}`);
                const exe = path.join(dir, "Contents", "MacOS", String(plist.CFBundleExecutable));
                if (!fs.existsSync(exe)) problems.push(`主程序 ${plist.CFBundleExecutable} 不存在`);
                else {
                    if (!(fs.statSync(exe).mode & 0o100)) problems.push("主程序没有可执行位");
                    const archs = run("lipo", ["-archs", exe]);
                    if (!archsMatch(archs, wantArchs))
                        problems.push(`架构是 ${archs.trim()}，期望 ${wantArchs.join(" ")}`);
                    const err = minosError(minosList(run("otool", ["-l", exe])), wantArchs.length, minos);
                    if (err) problems.push(err);
                    rows.push(
                        `| \`${label}\` | ${archs.trim()} | ${minosList(run("otool", ["-l", exe])).join(", ")} |`,
                    );
                }
                if (ext === ".component") {
                    const err = audioComponentError(plist, p);
                    if (err) problems.push(err);
                    else triples.push(`${p.auType} ${p.code} ${p.mfr}`);
                }
            } catch (e) {
                problems.push(e.message);
            }
            if (problems.length) errors.push(`${label}：${problems.join("；")}`);
            else bundles.push(dir);
        }
    }
    return { errors, rows, bundles, triples };
}

function selfTest() {
    const cases = [];
    const t = (name, ok) => cases.push([name, ok]);
    const golden = {
        plugins: {
            input: {
                identity: {
                    formal: {
                        PRODUCT_NAME: ["Rinne Input"],
                        PLUGIN_CODE: ["Rnni"],
                        BUNDLE_ID: ["com.synchain.rinne.input"],
                    },
                    probe: {
                        PRODUCT_NAME: ["Rinne Probe Input"],
                        PLUGIN_CODE: ["Rnpi"],
                        BUNDLE_ID: ["com.synchain.rinne.probe.input"],
                    },
                },
                shared: { PLUGIN_MANUFACTURER_CODE: ["Snch"], AU_MAIN_TYPE: ["kAudioUnitType_Effect"] },
            },
        },
    };
    const formal = expectedIdentity(golden, false)[0];
    const probe = expectedIdentity(golden, true)[0];
    t(
        "正式身份取自 golden 的 formal",
        formal.name === "Rinne Input" && formal.code === "Rnni" && formal.auType === "aufx",
    );
    t("探针身份取自 golden 的 probe", probe.code === "Rnpi" && probe.bundleId === "com.synchain.rinne.probe.input");
    t(
        "非效果器的 AU 类型直接报错",
        (() => {
            try {
                expectedIdentity(
                    {
                        plugins: {
                            x: {
                                ...golden.plugins.input,
                                shared: {
                                    PLUGIN_MANUFACTURER_CODE: ["Snch"],
                                    AU_MAIN_TYPE: ["kAudioUnitType_MusicDevice"],
                                },
                            },
                        },
                    },
                    false,
                );
                return false;
            } catch {
                return true;
            }
        })(),
    );

    t("arm64 单架构对得上", archsMatch("arm64\n", ["arm64"]));
    t("多出 x86_64 判错（正式包不许 universal）", !archsMatch("x86_64 arm64", ["arm64"]));
    t("探针要 universal，顺序无关", archsMatch("x86_64 arm64", ["arm64", "x86_64"]));
    t("探针只有 arm64 判错", !archsMatch("arm64", ["arm64", "x86_64"]));

    const otool = (versions) =>
        versions
            .map(
                (v) =>
                    `Load command 9\n      cmd LC_BUILD_VERSION\n  cmdsize 32\n platform 1\n    minos ${v}\n      sdk 15.0\n`,
            )
            .join("");
    t("minos 逐切片解析", minosList(otool(["11.0", "11.0"])).join() === "11.0,11.0");
    t(
        "别的 load command 里的 minos 字样不算",
        minosList("      cmd LC_VERSION_MIN_MACOSX\n    minos 10.9\n").length === 0,
    );
    t("minos 对得上", minosError(["11.0"], 1, "11.0") === "");
    t("minos 不等于部署目标判错", minosError(["12.0"], 1, "11.0") !== "");
    t("切片数对不上判错", minosError(["11.0"], 2, "11.0") !== "");

    const comp = (type, subtype, manufacturer) => ({ AudioComponents: [{ type, subtype, manufacturer }] });
    t("AU 三元组对得上", audioComponentError(comp("aufx", "Rnni", "Snch"), formal) === "");
    t("subtype 差一个字母判错", audioComponentError(comp("aufx", "Rnnj", "Snch"), formal) !== "");
    t("类型不是 aufx 判错", audioComponentError(comp("aumf", "Rnni", "Snch"), formal) !== "");
    t("声明了两个组件判错", audioComponentError({ AudioComponents: [{}, {}] }, formal) !== "");
    t("没有 AudioComponents 判错", audioComponentError({}, formal) !== "");

    let failed = 0;
    for (const [name, ok] of cases) {
        if (!ok) failed++;
        console.log(`${ok ? "PASS" : "FAIL"}  ${name}`);
    }
    if (failed) {
        console.error(`check-bundles 自测：${failed} 项失败`);
        process.exit(1);
    }
    console.log(`check-bundles 自测：${cases.length} 项通过`);
}

function main(argv) {
    if (argv.includes("--self-test")) return selfTest();
    const arg = (name) => {
        const i = argv.indexOf(name);
        return i < 0 ? null : argv[i + 1];
    };
    const build = arg("--build");
    const minos = arg("--minos");
    if (!build || !minos) {
        console.error(
            "用法：check-bundles.mjs --build <构建目录> --minos <部署目标> [--probe] [--list <文件>] [--triples <文件>]",
        );
        process.exit(2);
    }
    const probe = argv.includes("--probe");
    const { errors, rows, bundles, triples } = checkAll({ build, probe, minos });
    for (const r of rows) console.log(r);
    if (arg("--list")) fs.writeFileSync(arg("--list"), bundles.map((b) => path.resolve(b) + "\n").join(""));
    if (arg("--triples")) fs.writeFileSync(arg("--triples"), triples.map((t) => t + "\n").join(""));
    if (process.env.GITHUB_STEP_SUMMARY) {
        const head = `### ${probe ? "探针" : "正式"}构建的 bundle\n\n| bundle | 架构 | minos |\n|---|---|---|\n`;
        fs.appendFileSync(process.env.GITHUB_STEP_SUMMARY, head + rows.join("\n") + "\n\n");
    }
    if (errors.length) {
        for (const e of errors) console.error(`::error::${e}`);
        process.exit(1);
    }
    console.log(`bundle 核对通过：${bundles.length} 个${probe ? "（探针，universal）" : "（arm64）"}`);
}

main(process.argv.slice(2));
