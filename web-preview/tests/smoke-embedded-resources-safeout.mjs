// SPDX-License-Identifier: GPL-3.0-or-later
//
// 输出文件护栏（Node，纯函数加临时副本）：截图器与设计稿检查的输出只能写进 build/ 之下写死的目录，
// 调用方只给不带目录的文件名，后缀限定，不覆盖目录，不穿过符号链接或 junction。
//   ① 纯函数 outVerdict：只传伪造的路径字符串，断言放行或拒绝，不碰真实目录；
//   ② resolveOutFile 的真实行为：只在 os.tmpdir() 下 mkdtemp 出来的假仓库里跑。
//
// 用法：node web-preview/tests/smoke-embedded-resources-safeout.mjs [仓库根]

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { cleanupTempDir, makeTempDir, removeFixedDirInTemp } from "../../scripts/lib/safe-rm.mjs";

/**
 * 只摘掉链接本身：先确认路径确实是符号链接或 junction，再 unlink。
 * Windows 上 libuv 的 unlink 也能摘目录 junction；POSIX 上 "junction" 类型会被忽略、建出来的是普通符号链接，
 * rmdir 摘不掉它。不是链接就抛错，绝不退回递归删除。
 */
function unlinkLink(p) {
    if (!fs.lstatSync(p).isSymbolicLink()) throw new Error(`不是符号链接或 junction，拒绝摘除：${p}`);
    fs.unlinkSync(p);
}

const ROOT = repoRoot();
const run = createRun("输出文件护栏");
const { outVerdict, resolveOutFile } = await import(
    pathToFileURL(path.join(ROOT, "web-preview/tests/lib/safe-out.mjs")).href
);

// ---- ① 纯函数，全部虚构路径 ----
const rootReal = path.resolve("/fake/work/rinne");
const baseRel = ["build", "web-preview-shots"];
const baseReal = path.join(rootReal, ...baseRel);
const ok = {
    rootReal,
    baseRel,
    baseReal,
    name: "in-playing.png",
    exts: [".png"],
    targetReal: path.join(baseReal, "in-playing.png"),
    targetIsLink: false,
    targetIsDir: false,
};
const verdict = (patch) => outVerdict({ ...ok, ...patch });
const withName = (name) => verdict({ name, targetReal: path.join(baseReal, String(name)) });

run.check(verdict({}).ok, "正常文件名：放行");
run.check(withName("A_b-1.2.png").ok, "字母数字点横线下划线：放行");
for (const bad of [
    "",
    ".",
    "..",
    "../x.png",
    "a/b.png",
    "a\\b.png",
    "/abs/x.png",
    "C:\\x.png",
    "x.txt",
    "x",
    "x.png.exe",
    ".png",
    "a..b.png",
    "a b.png",
    "x".repeat(70) + ".png",
    undefined,
    null,
    7,
]) {
    run.check(!withName(bad).ok, `文件名 ${JSON.stringify(bad)}：拒绝`);
}
run.check(!outVerdict({ ...ok, exts: [".md"] }).ok, "后缀不在允许列表：拒绝");
run.check(outVerdict({ ...ok, name: "A.PNG", targetReal: path.join(baseReal, "A.PNG") }).ok, "后缀大小写不敏感：放行");
for (const [label, patch] of [
    [
        "输出目录真实路径指向别处（junction）",
        { baseReal: path.resolve("/fake/elsewhere"), targetReal: path.resolve("/fake/elsewhere/in-playing.png") },
    ],
    ["输出目录真实路径指向仓库根", { baseReal: rootReal, targetReal: path.join(rootReal, "in-playing.png") }],
    [
        "输出目录真实路径指向 web/",
        { baseReal: path.join(rootReal, "web"), targetReal: path.join(rootReal, "web", "in-playing.png") },
    ],
    ["目标真实路径跑出输出目录", { targetReal: path.resolve("/fake/elsewhere/in-playing.png") }],
    ["目标是符号链接", { targetIsLink: true }],
    ["目标是目录", { targetIsDir: true }],
    ["rootReal 不是绝对路径", { rootReal: "relative" }],
]) {
    run.check(!verdict(patch).ok, `${label}：拒绝`);
}

// ---- ② 真实行为：只在 mkdtemp 出来的假仓库里 ----
const tmp = makeTempDir("rinne-safeout-");
const outside = makeTempDir("rinne-safeout-outside-");
try {
    const resolveIn = (name, rel = baseRel, exts = [".png"]) => resolveOutFile(tmp, rel, name, exts);
    const p = resolveIn("a.png");
    run.check(
        p === path.join(tmp, ...baseRel, "a.png") && fs.existsSync(path.dirname(p)),
        "正常：返回固定目录里的路径并创建目录",
    );
    fs.writeFileSync(p, "x");
    run.check(resolveIn("a.png") === p, "覆盖自己目录里已有的文件：放行");
    for (const bad of ["../a.png", "sub/a.png", "a.txt", ""]) {
        run.check(
            throws(() => resolveIn(bad)),
            `真实目录：文件名 ${JSON.stringify(bad)} 被拒`,
        );
    }
    fs.mkdirSync(path.join(tmp, ...baseRel, "dir.png"));
    run.check(
        throws(() => resolveIn("dir.png")),
        "目标是已有目录：拒绝",
    );
    // 目标是指向外部文件的符号链接
    fs.writeFileSync(path.join(outside, "victim.png"), "victim");
    let madeLink = false;
    try {
        fs.symlinkSync(path.join(outside, "victim.png"), path.join(tmp, ...baseRel, "link.png"), "file");
        madeLink = true;
    } catch {
        // 本机无权创建文件符号链接（Windows 需要管理员或开发者模式）：这一例只在能创建时才跑，纯函数部分已覆盖同一判据
        run.log("  [说明] 目标是文件符号链接的真实用例未跑：本机无权创建文件符号链接");
    }
    if (madeLink) {
        run.check(
            throws(() => resolveIn("link.png")),
            "目标是符号链接：拒绝",
        );
        run.eq(fs.readFileSync(path.join(outside, "victim.png"), "utf8"), "victim", "符号链接被拒之后外部文件没动");
    }
    // 输出目录是 junction
    removeFixedDirInTemp(path.join(tmp, "build"), { tempRoot: tmp, rel: ["build"] }); // 假仓库里的 build/
    fs.mkdirSync(path.join(tmp, "build"));
    fs.symlinkSync(outside, path.join(tmp, ...baseRel), "junction");
    run.check(
        throws(() => resolveIn("b.png")),
        "输出目录是 junction：拒绝",
    );
    run.check(!fs.existsSync(path.join(outside, "b.png")), "junction 被拒之后外部目录里没有新文件");
    unlinkLink(path.join(tmp, ...baseRel)); // 只摘掉假仓库里的链接本身
    // build/ 整个是 junction
    fs.rmdirSync(path.join(tmp, "build"));
    fs.symlinkSync(outside, path.join(tmp, "build"), "junction");
    run.check(
        throws(() => resolveIn("c.png")),
        "build/ 本身是 junction：拒绝",
    );
    run.check(!fs.existsSync(path.join(outside, "web-preview-shots")), "build/ 是 junction 被拒之后没有穿过去建目录");
    unlinkLink(path.join(tmp, "build"));
} finally {
    cleanupTempDir(tmp);
    cleanupTempDir(outside);
}

function throws(fn) {
    try {
        fn();
        return false;
    } catch {
        return true;
    }
}

run.finish();
