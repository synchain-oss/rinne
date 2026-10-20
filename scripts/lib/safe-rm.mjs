// SPDX-License-Identifier: GPL-3.0-or-later
//
// node 侧的递归删除：和 TempDir.psm1 同一套规矩。脚本、测试里的递归删除只能走这里的两个入口，要不然就在那一行带豁免注释
// （checks.d/recursive-delete.mjs 把关）。
//   makeTempDir(prefix, { parent })      在系统临时目录（或指定的已有目录）下一层建随机名目录，记下真实路径，返回路径
//   removeTempDir(dir, { maxRetries })   只删本进程经 makeTempDir 建出来的目录；删之前重新按真实路径核对。返回 null 或不删/删不掉的原因
//   removeFixedDir(dir, { root, rel })   只删「仓内写死的输出目录」：dir 的真实路径必须正好是 root/rel（rel 是写死的相对路径），
//                                         且它本身不是链接、不是仓根或盘根或用户目录。返回 null 或原因
// 核对用的判断都是纯函数（dangerousReason、tempVerdict、fixedVerdict），自测只喂它们假数据，不在真实路径上触发删除。
// 链接的处理：路径逐级解析成真实路径再比；fs.rmSync 递归删除遇到目录里的链接只删链接本身，不进去。

import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const WIN = process.platform === "win32";
const norm = (p) => (WIN ? p.toLowerCase() : p);
const same = (a, b) => norm(a) === norm(b);
const isUnder = (child, dir) => norm(child).startsWith(norm(dir.endsWith(path.sep) ? dir : dir + path.sep));

/** 真实路径：存在的最深一级用 realpath，不存在的部分照字面接上。 */
export function realPathLoose(p) {
    let cur = path.resolve(p);
    const rest = [];
    for (;;) {
        try {
            return path.join(fs.realpathSync.native(cur), ...rest.reverse());
        } catch {
            const up = path.dirname(cur);
            if (up === cur) return path.join(cur, ...rest.reverse());
            rest.push(path.basename(cur));
            cur = up;
        }
    }
}

/** 盘根、用户目录或用户目录的上级：返回原因，否则 null。纯函数（真实路径由调用方给）。 */
export function dangerousReason(real, home = os.homedir()) {
    const r = real.replace(/[\\/]+$/, "") || real;
    if (same(r, path.parse(real).root.replace(/[\\/]+$/, "") || path.parse(real).root)) return `${real} 是盘根`;
    if (home) {
        const h = realPathLoose(home).replace(/[\\/]+$/, "");
        if (same(r, h)) return `${real} 是用户目录`;
        if (isUnder(h, r)) return `${real} 是用户目录的上级`;
    }
    return null;
}

/** 本进程建的目录：真实路径 → { prefix, parent, temp } */
const made = new Map();

/**
 * 临时目录能不能删的判断（纯）：made 是登记表里的条目或 undefined，real 是现在的真实路径，tmpReal 是现在的系统临时目录的真实路径。
 * 返回 null（能删）或原因。
 */
export function tempVerdict({ real, entry, tmpReal, home }) {
    if (!entry) return `不是本进程用 makeTempDir 建的目录（按真实路径比：${real}）`;
    const danger = dangerousReason(real, home);
    if (danger) return danger;
    if (entry.temp && !same(entry.parent, tmpReal)) return `真实路径 ${real} 不在系统临时目录 ${tmpReal} 下一层`;
    if (!same(path.dirname(real), entry.parent)) return `真实路径 ${real} 不在建时的父目录 ${entry.parent} 下一层`;
    if (!path.basename(real).startsWith(entry.prefix)) return `目录名不以建时的前缀 ${entry.prefix} 开头：${real}`;
    return null;
}

/** 仓内写死的输出目录能不能删的判断（纯）。rel 是写死的相对路径（数组或 / 分隔的串）。 */
export function fixedVerdict({ rootReal, targetReal, targetIsLink, rel, home }) {
    const parts = Array.isArray(rel) ? rel : String(rel).split("/");
    if (!parts.length || parts.some((s) => !s || s === "." || s === "..")) return "写死的相对路径不合格";
    if (targetIsLink) return `${targetReal} 本身是链接，不递归删`;
    const danger = dangerousReason(targetReal, home);
    if (danger) return danger;
    if (same(targetReal, rootReal)) return `${targetReal} 就是仓根`;
    const want = path.join(rootReal, ...parts);
    if (!same(targetReal, want)) return `真实路径 ${targetReal} 不是写死的输出目录 ${want}`;
    return null;
}

/** 新建随机名目录并登记。prefix 形如 rinne-xxx-；parent 缺省是系统临时目录。 */
export function makeTempDir(prefix, { parent } = {}) {
    if (!/^rinne-[a-z0-9-]+-$/.test(prefix)) throw new Error(`临时目录前缀不合格：${prefix}`);
    const base = parent ?? os.tmpdir();
    if (parent) {
        const danger = dangerousReason(realPathLoose(parent));
        if (danger) throw new Error(`不在这里建可删的目录：${danger}`);
    }
    for (let i = 0; i < 100; i++) {
        const dir = path.join(base, prefix + crypto.randomBytes(16).toString("hex"));
        try {
            fs.mkdirSync(dir, { mode: 0o700 });
        } catch (e) {
            if (e.code === "EEXIST") continue;
            throw e;
        }
        made.set(norm(realPathLoose(dir)), {
            real: realPathLoose(dir),
            prefix,
            parent: realPathLoose(base),
            temp: !parent,
        });
        return dir;
    }
    throw new Error(`在 ${base} 下建不出 ${prefix} 开头的临时目录（同名的太多）`);
}

/** 删掉本进程建的临时目录。不在了返回 null；不删、删不掉返回原因。 */
export function removeTempDir(dir, { maxRetries = 0 } = {}) {
    if (!dir) return null;
    try {
        fs.lstatSync(dir);
    } catch (e) {
        if (e.code === "ENOENT") return null;
        return `不删 ${dir}：${e.message}`;
    }
    const real = realPathLoose(dir);
    const entry = made.get(norm(real));
    const why = tempVerdict({ real, entry, tmpReal: realPathLoose(os.tmpdir()) });
    if (why) return `不删 ${dir}：${why}`;
    try {
        fs.rmSync(entry.real, { recursive: true, force: true, maxRetries, retryDelay: 300 }); // rinne-delete-ok: 核对过的本进程新建目录
        made.delete(norm(real));
        return null;
    } catch (e) {
        return `删不掉 ${dir}：${e.message}`;
    }
}

/** 删掉仓内写死的输出目录（root 下 rel）。不在返回 null；不删、删不掉返回原因。 */
export function removeFixedDir(dir, { root, rel }) {
    let st;
    try {
        st = fs.lstatSync(dir);
    } catch (e) {
        if (e.code === "ENOENT") return null;
        return `不删 ${dir}：${e.message}`;
    }
    const why = fixedVerdict({
        rootReal: realPathLoose(root),
        targetReal: realPathLoose(dir),
        targetIsLink: st.isSymbolicLink(),
        rel,
    });
    if (why) return `不删 ${dir}：${why}`;
    try {
        fs.rmSync(dir, { recursive: true, force: true }); // rinne-delete-ok: 核对过的仓内写死输出目录
        return null;
    } catch (e) {
        return `删不掉 ${dir}：${e.message}`;
    }
}

/**
 * 自测：返回 [{ label, bad, passed }]。纯函数用假数据；建与删只发生在本进程新建的目录里，
 * 「拒绝」类用例只喂纯函数，不在真实路径上触发删除。
 */
export function selfTestCases() {
    const out = [];
    const t = (label, bad, flagged) => out.push({ label, bad, passed: bad ? !!flagged : !flagged });
    const root = path.parse(process.cwd()).root;
    const home = path.join(root, "users", "me");
    const tmp = path.join(root, "tmp");
    const ok = {
        real: path.join(tmp, "rinne-x-1"),
        entry: { prefix: "rinne-x-", parent: tmp, temp: true },
        tmpReal: tmp,
        home,
    };
    t("临时目录判断：本进程建的、在临时目录下一层、前缀对：通过", false, tempVerdict(ok));
    t("临时目录判断：不是本进程建的：拒绝", true, tempVerdict({ ...ok, entry: undefined }));
    t("临时目录判断：临时目录换了位置：拒绝", true, tempVerdict({ ...ok, tmpReal: path.join(root, "elsewhere") }));
    t(
        "临时目录判断：真实路径变到别处（父目录对不上）：拒绝",
        true,
        tempVerdict({ ...ok, real: path.join(root, "other", "rinne-x-1") }),
    );
    t("临时目录判断：名字不以前缀开头：拒绝", true, tempVerdict({ ...ok, real: path.join(tmp, "other-1") }));
    t(
        "临时目录判断：盘根：拒绝",
        true,
        tempVerdict({ ...ok, real: root, entry: { prefix: "", parent: root, temp: true }, tmpReal: root }),
    );
    t(
        "临时目录判断：用户目录：拒绝",
        true,
        tempVerdict({ ...ok, real: home, entry: { prefix: "me", parent: path.dirname(home), temp: false } }),
    );
    t("危险路径：用户目录的上级", true, dangerousReason(path.dirname(home), home));
    t("危险路径：用户目录里面的子目录不算", false, dangerousReason(path.join(home, "work"), home));
    const repo = path.join(home, "repo");
    const fx = {
        rootReal: repo,
        targetReal: path.join(repo, "build", "out"),
        targetIsLink: false,
        rel: ["build", "out"],
        home,
    };
    t("写死目录判断：正好是 root/rel：通过", false, fixedVerdict(fx));
    t("写死目录判断：别的目录：拒绝", true, fixedVerdict({ ...fx, targetReal: path.join(repo, "src") }));
    t("写死目录判断：就是仓根：拒绝", true, fixedVerdict({ ...fx, targetReal: repo }));
    t("写死目录判断：本身是链接：拒绝", true, fixedVerdict({ ...fx, targetIsLink: true }));
    t("写死目录判断：rel 带 ..：拒绝", true, fixedVerdict({ ...fx, rel: ["..", "x"] }));
    t("写死目录判断：rel 为空：拒绝", true, fixedVerdict({ ...fx, rel: [] }));
    t("写死目录判断：目标是盘根：拒绝", true, fixedVerdict({ ...fx, targetReal: root }));
    t(
        "前缀不合格时建不出目录",
        true,
        (() => {
            try {
                makeTempDir("x-");
                return false;
            } catch {
                return true;
            }
        })(),
    );

    // 真建真删，只在本进程新建的目录里
    const a = makeTempDir("rinne-safermtest-");
    const b = makeTempDir("rinne-safermtest-");
    fs.mkdirSync(path.join(a, "sub", "deeper"), { recursive: true });
    fs.writeFileSync(path.join(a, "sub", "deeper", "f.txt"), "x");
    fs.writeFileSync(path.join(b, "keep.txt"), "x");
    let linked = true;
    try {
        fs.symlinkSync(b, path.join(a, "out"), "junction");
    } catch {
        linked = false; // 没有权限建链接的机器上跳过链接那一项
    }
    const r1 = removeTempDir(a);
    t("删除：建的目录连同子目录删掉", false, !(r1 === null && !fs.existsSync(a)));
    if (linked) t("删除：里面指向别处的链接只删链接，指向的东西不动", false, !fs.existsSync(path.join(b, "keep.txt")));
    t("删除：目录已经不在时什么都不做", false, removeTempDir(a) !== null);
    const sub = path.join(b, "inner");
    fs.mkdirSync(sub);
    t("拒绝：本进程建的目录里面的子目录不是登记的那一个", true, removeTempDir(sub) && fs.existsSync(sub));
    const parentLevel = makeTempDir("rinne-safermtest-", { parent: b });
    t("指定父目录：建在 parent 下一层", false, !same(path.dirname(realPathLoose(parentLevel)), realPathLoose(b)));
    t("指定父目录：建出来的能删", false, removeTempDir(parentLevel) !== null || fs.existsSync(parentLevel));
    t(
        "拒绝：父目录是盘根时建不出来",
        true,
        (() => {
            try {
                makeTempDir("rinne-safermtest-", { parent: root });
                return false;
            } catch {
                return true;
            }
        })(),
    );
    const r2 = removeTempDir(b);
    t("删除：自测建的目录都删掉了", false, r2 !== null || fs.existsSync(b));
    const f = makeTempDir("rinne-safermtest-");
    fs.mkdirSync(path.join(f, "build", "out"), { recursive: true });
    fs.writeFileSync(path.join(f, "build", "out", "x"), "x");
    t(
        "写死目录：不是 rel 指的目录就不删",
        true,
        removeFixedDir(path.join(f, "build"), { root: f, rel: ["build", "out"] }) &&
            fs.existsSync(path.join(f, "build", "out", "x")),
    );
    t(
        "写死目录：正好是 root/rel 的删掉",
        false,
        removeFixedDir(path.join(f, "build", "out"), { root: f, rel: ["build", "out"] }) !== null ||
            fs.existsSync(path.join(f, "build", "out")),
    );
    removeTempDir(f);
    return out;
}
