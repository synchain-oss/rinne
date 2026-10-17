// SPDX-License-Identifier: GPL-3.0-or-later
//
// 静态导出的清空守卫。build-static 只会清空自己写死的输出目录，清之前由纯函数 cleanVerdict 判定。
//   ① 纯函数部分：只传路径字符串和事实，断言放行或拒绝，不碰任何真实目录；符号链接与 junction 指向外部，
//      用伪造的 realpath 字符串表达；
//   ② 端到端部分：只在 os.tmpdir() 下 mkdtemp 出来的仓库副本里跑 build-static，从不对真实仓库跑导出，
//      也不对任何非自己创建的目录做删除。
//
// 用法：node web-preview/tests/smoke-embedded-resources-outdir.mjs [仓库根]

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { pathToFileURL } from "node:url";
import { createRun, repoRoot } from "./lib/harness.mjs";

const ROOT = repoRoot();
const run = createRun("静态导出清空守卫");
const { cleanVerdict, OUT_REL, MARKER } = await import(
    pathToFileURL(path.join(ROOT, "web-preview", "build-static.mjs")).href
);

// ---- ① 纯函数：全部用虚构路径 ----
const rootReal = path.resolve("/fake/work/rinne");
const baseReal = path.join(rootReal, ...OUT_REL);
const ok = { rootReal, baseReal, targetReal: baseReal, exists: true, hasMarker: true };
const verdict = (patch) => cleanVerdict({ ...ok, ...patch });
const rejected = (patch, label) => run.check(!verdict(patch).ok, `${label}：拒绝`);

run.check(verdict({}).ok, "固定目录、有标记：放行");
run.check(verdict({ exists: false, hasMarker: false }).ok, "固定目录不存在：放行");
rejected({ targetReal: path.join(baseReal, "demo") }, "固定目录下的子目录（不再有子名）");
rejected({ hasMarker: false }, "已存在却没有标记文件");

for (const [label, targetReal] of [
    ["仓库根", rootReal],
    ["仓库根的上级", path.dirname(rootReal)],
    ["盘根", path.parse(rootReal).root],
    ["web", path.join(rootReal, "web")],
    ["web-preview", path.join(rootReal, "web-preview")],
    ["build", path.join(rootReal, "build")],
    ["build 下的另一个目录", path.join(rootReal, "build", "other")],
    ["固定目录的上级写成 ..", path.join(baseReal, "..")],
    ["固定目录的兄弟", `${baseReal}-x`],
    ["用户目录", path.resolve("/fake/home/user")],
    ["无关的绝对路径", path.resolve("/fake/elsewhere/dist")],
]) {
    rejected({ targetReal }, `目标 ${label}`);
}
// 符号链接或 junction：字面上在固定目录里，真实路径却指向外部
rejected({ targetReal: path.resolve("/fake/elsewhere/dist") }, "固定目录的子目录是 junction 指向外部");
rejected(
    { baseReal: path.resolve("/fake/elsewhere/static"), targetReal: path.resolve("/fake/elsewhere/static") },
    "build/web-preview-static 本身是 junction 指向外部",
);
rejected(
    { baseReal: path.join(rootReal, "web"), targetReal: path.join(rootReal, "web") },
    "固定目录的真实路径指向 web/",
);
rejected({ baseReal: rootReal, targetReal: rootReal }, "固定目录的真实路径指向仓库根");
// 输入不合法
for (const bad of ["", ".", "..", "relative/dir", undefined, null, 7]) {
    rejected({ targetReal: bad }, `targetReal 为 ${JSON.stringify(bad)}`);
    rejected({ rootReal: bad }, `rootReal 为 ${JSON.stringify(bad)}`);
}
// 仓库根自己是符号链接：以真实路径为准
const realRoot2 = path.resolve("/fake/real/rinne");
run.check(
    cleanVerdict({
        rootReal: realRoot2,
        baseReal: path.join(realRoot2, ...OUT_REL),
        targetReal: path.join(realRoot2, ...OUT_REL),
        exists: true,
        hasMarker: true,
    }).ok,
    "仓库根是符号链接、真实路径下的固定目录：放行",
);
rejected({ rootReal: realRoot2 }, "目录在字面仓库根下、真实仓库根却是别处");

// ---- ② 端到端：只在 mkdtemp 出来的仓库副本里跑 ----
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "rinne-static-"));
try {
    fs.cpSync(path.join(ROOT, "web"), path.join(tmp, "web"), { recursive: true });
    fs.cpSync(path.join(ROOT, "web-preview"), path.join(tmp, "web-preview"), {
        recursive: true,
        filter: (src) => !src.includes(`${path.sep}.shots`),
    });
    const script = path.join(tmp, "web-preview", "build-static.mjs");
    const exec = (...args) => spawnSync(process.execPath, [script, ...args], { cwd: tmp, encoding: "utf8" });
    const out = path.join(tmp, ...OUT_REL);
    const bridge = path.join(tmp, "web", "shared", "bridge.js");

    let r = exec();
    run.check(
        r.status === 0 && fs.existsSync(path.join(out, "index.html")) && fs.existsSync(path.join(out, MARKER)),
        "副本里导出：写出 index.html 与标记文件",
    );
    fs.writeFileSync(path.join(out, "stale.txt"), "x");
    r = exec();
    run.check(r.status === 0 && !fs.existsSync(path.join(out, "stale.txt")), "有标记的目录可以被清空重建");

    // 目录里放一个没有标记的子目录，被拒绝的调用都不得删掉它
    const foreign = path.join(out, "foreign");
    fs.mkdirSync(foreign);
    fs.writeFileSync(path.join(foreign, "keep.txt"), "keep");
    for (const args of [
        ["--out", tmp],
        ["--out", "."],
        ["--name", "demo"],
        ["--name"],
        ["--name", ".."],
        ["--bogus"],
    ]) {
        r = exec(...args);
        run.check(r.status === 1, `参数 ${JSON.stringify(args)}：应当被拒绝（退出码 1），实得 ${r.status}`);
        run.check(fs.existsSync(bridge), `参数 ${JSON.stringify(args)}：被拒之后副本里的 web/ 仍在`);
        run.check(
            fs.existsSync(path.join(foreign, "keep.txt")),
            `参数 ${JSON.stringify(args)}：被拒之后输出目录里的内容仍在`,
        );
    }

    // 固定目录丢了标记文件：拒绝，什么都不删
    fs.rmSync(path.join(out, MARKER));
    r = exec();
    run.check(r.status === 1 && fs.existsSync(path.join(foreign, "keep.txt")), "固定目录丢了标记文件：拒绝，内容还在");

    r = exec("--check");
    run.check(r.status === 0, "--check 只自检不写文件");
} finally {
    fs.rmSync(tmp, { recursive: true }); // 只删上面 mkdtemp 出来的目录
}

run.finish();
