// SPDX-License-Identifier: GPL-3.0-or-later
//
// 检查脚本共用的小工具：仓根、git 调用、跟踪文件、本机设置、Node 版本断言、自测用临时仓。
// 零依赖；Node 版本钉在 .node-version。

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { makeTempDir, cleanupTempDir } from "./safe-rm.mjs";

export const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");

/** 片段拼接。规则与夹具都用它拼，源码里不出现任何完整的禁用形态。 */
export const j = (...parts) => parts.join("");

/**
 * 当前模块是否被直接 `node <file>` 执行（被 import 时不跑 CLI）。两边都取真实路径：
 * 经符号链接或 junction 调用时 argv 是链接路径，而 import.meta.url 已被解析成真实路径。
 */
export function isMain(url) {
    try {
        return !!process.argv[1] && fs.realpathSync(process.argv[1]) === fs.realpathSync(fileURLToPath(url));
    } catch {
        return false;
    }
}

/** 运行 git，返回 stdout（raw 时返回 Buffer）。失败时抛错；allowFail 时返回 null。 */
export function git(args, { cwd = ROOT, input, env, allowFail = false, raw = false } = {}) {
    const r = spawnSync("git", ["-c", "core.quotepath=false", ...args], {
        cwd,
        input,
        env,
        encoding: raw ? "buffer" : "utf8",
        maxBuffer: 256 * 1024 * 1024,
    });
    if (r.status === 0) return r.stdout;
    if (allowFail) return null;
    throw new Error(`git ${args[0]} 失败（退出码 ${r.status}）：${String(r.stderr || "").trim()}`);
}

export const trackedFiles = (root = ROOT) => git(["ls-files", "-z"], { cwd: root }).split("\0").filter(Boolean);

/**
 * 按 BOM 解码：UTF-8（去 BOM）、UTF-16LE、UTF-16BE。没有 BOM 时前 8000 字节内有 NUL 就当二进制
 * （与 git 的判断一致），返回 null。Windows PowerShell 5.1 的 `>` 与 Out-File 默认写 UTF-16LE。
 */
export function decodeText(buf) {
    if (buf[0] === 0xef && buf[1] === 0xbb && buf[2] === 0xbf) return buf.subarray(3).toString("utf8");
    if (buf[0] === 0xff && buf[1] === 0xfe) return buf.subarray(2).toString("utf16le");
    if (buf[0] === 0xfe && buf[1] === 0xff) {
        const le = Buffer.from(buf.subarray(2, buf.length - (buf.length % 2)));
        return le.swap16().toString("utf16le");
    }
    return buf.subarray(0, 8000).includes(0) ? null : buf.toString("utf8");
}

/** 读文本文件并按 BOM 解码；二进制或读不到时返回 null。 */
export function readText(abs) {
    try {
        return decodeText(fs.readFileSync(abs));
    } catch {
        return null;
    }
}

/**
 * 本机设置：环境变量优先，其次 git config。主 clone 的本地配置对它的所有 worktree 生效，
 * 所以 hook 不依赖调用方的 shell 环境；仓内不写任何本机路径。
 */
export function localSetting(envName, gitKey, { env = process.env, cwd = process.cwd() } = {}) {
    if (env[envName]) return env[envName];
    return (git(["config", "--get", gitKey], { cwd, env, allowFail: true }) || "").trim();
}

export const inCI = (env = process.env) => env.GITHUB_ACTIONS === "true" || env.CI === "true";

/** 主版本必须等于 .node-version：本机全局 Node 可能是别的大版本，行为不保证一致。 */
export function assertNodeVersion(root = ROOT) {
    const want = fs.readFileSync(path.join(root, ".node-version"), "utf8").trim().split(".")[0];
    const have = process.versions.node.split(".")[0];
    if (want !== have) {
        throw new Error(
            `需要 Node ${want}（见 .node-version），当前是 ${process.versions.node}；` +
                "hook 用 RINNE_NODE 或 git config rinne.node 指定钉版 Node",
        );
    }
}

/** 不受调用方 git 环境影响的 env：去掉 GIT_*，不读系统与全局 git 配置（Git for Windows 也认 /dev/null）。 */
export function isolatedEnv(extra = {}) {
    const env = Object.fromEntries(Object.entries(process.env).filter(([k]) => !k.startsWith("GIT_")));
    return { ...env, GIT_CONFIG_NOSYSTEM: "1", GIT_CONFIG_GLOBAL: "/dev/null", ...extra };
}

/** 自测用临时仓，身份是维护者身份；用完调 cleanup()。 */
export function tempRepo({ bare = false } = {}) {
    const dir = makeTempDir("rinne-selftest-");
    const env = isolatedEnv();
    const run = (args, opts = {}) => git(args, { cwd: dir, env, ...opts });
    run(bare ? ["init", "-q", "--bare"] : ["init", "-q", "-b", "main"]);
    if (!bare) {
        run(["config", "user.name", "DLsnows"]);
        run(["config", "user.email", "noreply@synchain.ca"]);
    }
    return {
        dir,
        env,
        git: run,
        write(rel, text) {
            const abs = path.join(dir, rel);
            fs.mkdirSync(path.dirname(abs), { recursive: true });
            fs.writeFileSync(abs, text);
        },
        cleanup: () => {
            cleanupTempDir(dir); // 在 finally 里调用，不抛，但清不掉要报出来
        },
    };
}

/** 把自测结果折成 run-checks 约定的形状：坏样例与正例都要有。 */
export function caseResult(cases) {
    const failures = cases.filter((c) => !c.passed).map((c) => c.label);
    return {
        ok: failures.length === 0,
        bad: cases.filter((c) => c.bad).length,
        good: cases.filter((c) => !c.bad).length,
        lines: failures.map((l) => `自测未通过：${l}`),
    };
}
