// SPDX-License-Identifier: GPL-3.0-or-later
//
// 冒烟脚本共用的断言与收尾。退出码约定：0 全绿，1 有失败，2 整套跳过（缺浏览器之类的外部件）。
// 跳过不是通过：run-all 和 gates 会把它如实记成 SKIP。

import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

export const EXIT_PASS = 0;
export const EXIT_FAIL = 1;
export const EXIT_SKIP = 2;

/** 仓库根：命令行第一个不以 -- 开头的参数，缺省取本文件往上三级。 */
export function repoRoot(argv = process.argv.slice(2)) {
    const given = argv.find((a) => !a.startsWith("--"));
    return given ? resolve(given) : resolve(dirname(fileURLToPath(import.meta.url)), "..", "..", "..");
}

/** 一次冒烟的记录器。 */
export function createRun(name) {
    let fail = 0;
    let n = 0;
    const out = (s) => console.log(s);
    const run = {
        name,
        log: out,
        check(cond, msg) {
            n++;
            if (cond) return true;
            fail++;
            out(`  [失败] ${msg}`);
            return false;
        },
        eq(got, want, msg) {
            n++;
            const a = JSON.stringify(got);
            const b = JSON.stringify(want);
            if (a === b) return true;
            fail++;
            out(`  [失败] ${msg}\n         实得 ${a}\n         应为 ${b}`);
            return false;
        },
        /** 整套跳过：打印原因并以 2 退出。 */
        skip(reason) {
            out(`${name}：跳过（${reason}）`);
            process.exit(EXIT_SKIP);
        },
        /** 收尾：打印汇总并设退出码。 */
        finish() {
            out(`${name}：${fail === 0 ? "通过" : `${fail} 项失败`}（${n} 项断言）`);
            process.exitCode = fail === 0 ? EXIT_PASS : EXIT_FAIL;
            return fail === 0;
        },
    };
    return run;
}
