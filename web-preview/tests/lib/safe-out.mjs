// SPDX-License-Identifier: GPL-3.0-or-later
//
// 工具脚本写输出文件的护栏。截图器、设计稿检查的 DELTA 草稿这类「把结果写成一个文件」的出口都经这里：
// 调用方只能给一个文件名（不带目录），文件只会写进仓库 build/ 之下写死的目录，后缀限定，
// 目标和目录都不能是符号链接或 junction，也不能覆盖目录。绝不接受任意路径。
//   - 判定是纯函数 outVerdict：只看路径字符串与事实，测试只传伪造的 realpath，不碰真实目录；
//   - resolveOutFile 取事实（realpath、lstat）后调用它，通过才创建目录并返回目标路径。

import fs from "node:fs";
import path from "node:path";
import { realpathLoose } from "../../build-static.mjs";

const NAME_RE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;
const samePath = (a, b) => path.relative(a, b) === "";

/**
 * 纯函数判定。
 * @param {object} i
 * @param {string} i.rootReal 仓库根的真实路径
 * @param {string[]} i.baseRel 写死的输出目录（相对仓库根，如 ["build","web-preview-shots"]）
 * @param {string} i.baseReal 输出目录的真实路径（不存在时是已存在的上级的真实路径拼回去）
 * @param {string} i.name 文件名（不带目录）
 * @param {string[]} i.exts 允许的后缀，小写带点
 * @param {string} i.targetReal 目标文件的真实路径（同上）
 * @param {boolean} i.targetIsLink 目标本身是不是符号链接或 junction（lstat 判）
 * @param {boolean} i.targetIsDir 目标是不是已存在的目录
 */
export function outVerdict({ rootReal, baseRel, baseReal, name, exts, targetReal, targetIsLink, targetIsDir }) {
    const no = (reason) => ({ ok: false, reason });
    for (const [k, v] of Object.entries({ rootReal, baseReal, targetReal })) {
        if (typeof v !== "string" || v === "" || !path.isAbsolute(v)) return no(`${k} 必须是非空的绝对路径`);
    }
    if (typeof name !== "string" || !NAME_RE.test(name) || name.includes("..")) {
        return no(`文件名只能是不带目录的名字（字母数字点横线下划线），实得 ${JSON.stringify(name)}`);
    }
    if (!exts.includes(path.extname(name).toLowerCase())) return no(`后缀只允许 ${exts.join("、")}`);
    if (!samePath(baseReal, path.join(rootReal, ...baseRel))) {
        return no(
            `输出目录的真实路径 ${baseReal} 不在仓库根真实路径下的 ${baseRel.join("/")}（符号链接或 junction？）`,
        );
    }
    if (!samePath(targetReal, path.join(baseReal, name))) return no(`目标的真实路径 ${targetReal} 不在输出目录里`);
    if (targetIsLink) return no("目标是符号链接或 junction，拒绝写入");
    if (targetIsDir) return no("目标是一个目录，不能当文件覆盖");
    return { ok: true, reason: "" };
}

/**
 * 解析并校验输出文件：返回绝对路径，失败抛错。通过后才创建输出目录。
 * @param {string} root 仓库根
 * @param {string[]} baseRel 写死的输出目录
 * @param {string} name 文件名
 * @param {string[]} exts 允许的后缀
 */
export function resolveOutFile(root, baseRel, name, exts) {
    const base = path.join(root, ...baseRel);
    const target = path.join(base, String(name));
    let st = null;
    try {
        st = fs.lstatSync(target);
    } catch {
        st = null;
    }
    const v = outVerdict({
        rootReal: fs.realpathSync(root),
        baseRel,
        baseReal: realpathLoose(base),
        name,
        exts,
        targetReal: realpathLoose(target),
        targetIsLink: st !== null && st.isSymbolicLink(),
        targetIsDir: st !== null && st.isDirectory(),
    });
    if (!v.ok) throw new Error(`拒绝写入输出文件：${v.reason}`);
    fs.mkdirSync(base, { recursive: true });
    return target;
}
