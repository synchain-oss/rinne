// SPDX-License-Identifier: GPL-3.0-or-later
//
// commit-msg hook 的判据（外壳是同目录的 commit-msg）。在 check-commits 同仓档之上再严两条：
// 作者与提交者都必须是维护者身份；签名恰好一行。消息过通用规则与个人针，
// git 写进编辑器的注释行也一起扫（-m 提交时以 # 开头的行会原样进历史）。
//   node scripts/hooks/commit-msg.mjs <消息文件>
//   node scripts/hooks/commit-msg.mjs --self-test    纯函数夹具，加上在临时仓里经真实 hook 提交

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { MAINTAINER, SIGNOFF, checkMessage } from "../check-commits.mjs";
import { FAKE_NEEDLE, loadPersonalNeedles, redact, scan } from "../lib/privacy-needles.mjs";
import { assertNodeVersion, caseResult, git, isMain, j, tempRepo } from "../lib/check-kit.mjs";

export const HOOKS = path.dirname(fileURLToPath(import.meta.url));

export function checkCommitMsg(raw, { authorIdent, committerIdent, personal = [] }) {
    const kept = [];
    const comments = [];
    for (const line of raw.split(/\r?\n/)) {
        if (/^# -+ >8 -+$/.test(line)) break; // --verbose 时剪刀线以下是 diff，不属于消息
        (line.startsWith("#") ? comments : kept).push(line);
    }
    const errors = [];
    if (!authorIdent.startsWith(`${MAINTAINER} `) || !committerIdent.startsWith(`${MAINTAINER} `)) {
        errors.push(`作者与提交者都必须是 ${MAINTAINER}（仓内 user.name / user.email）`);
    }
    const message = `${kept.join("\n").trim()}\n`;
    errors.push(...checkMessage(message, { exactlyOne: true, personal }).errors);
    for (const f of scan(comments.join("\n"), {}, { personal })) errors.push(`注释行 [${f.rule.id}] ${redact(f)}`);
    return errors;
}

export function selfTest() {
    const cases = [];
    const me = `${MAINTAINER} 1760000000 +0800`;
    const ids = { authorIdent: me, committerIdent: me, personal: [FAKE_NEEDLE] };
    const t = (label, bad, raw, extra = {}) => {
        const errors = checkCommitMsg(raw, { ...ids, ...extra });
        cases.push({ label, bad, passed: bad ? errors.length > 0 : errors.length === 0 });
    };
    const good = `ci(hooks): 加入钩子\n\n说明。\n\n${SIGNOFF}\n`;
    t("好消息", false, `${good}# Please enter the commit message\n`);
    const scissors = "# ------------------------ >8 ------------------------";
    t("剪刀线以下不算消息", false, `${good}${scissors}\n+${j("R", "N-12")}\n`);
    t("缺签名", true, "ci(hooks): 加入钩子\n");
    t("两行签名", true, `${good}${SIGNOFF}\n`);
    t("签名身份不对", true, "ci(hooks): 加入钩子\n\nSigned-off-by: Someone <s@example.com>\n");
    t("作者身份不对", true, good, { authorIdent: "Someone <s@example.com> 1760000000 +0800" });
    t("卡号", true, `ci(hooks): ${j("R", "N01")} 收尾\n\n${SIGNOFF}\n`);
    t("注释行里的卡号", true, `${good}# On branch ${j("feat/r", "n-01a")}\n`);
    t("个人针", true, `ci(hooks): 加入钩子\n\n${FAKE_NEEDLE}\n\n${SIGNOFF}\n`);

    // 端到端：临时仓经真实的 sh 外壳与本文件提交
    const repo = tempRepo();
    const nf = `${repo.dir}-needles.txt`;
    fs.writeFileSync(nf, `${FAKE_NEEDLE}\n`);
    const env = { ...repo.env, RINNE_NODE: process.execPath, RINNE_PRIVACY_LOCAL: nf };
    try {
        repo.git(["config", "core.hooksPath", HOOKS]);
        const c = (label, bad, flags, gitOpts = []) => {
            const ok = repo.git([...gitOpts, "commit", "-q", "--allow-empty", ...flags], { env, allowFail: true });
            cases.push({ label: `端到端：${label}`, bad, passed: bad ? ok === null : ok !== null });
        };
        c("好提交", false, ["-s", "-m", "ci(test): 好提交"]);
        c("缺签名", true, ["-m", "ci(test): 无签名"]);
        c("卡号", true, ["-s", "-m", `ci(test): ${j("R", "N-01a")}`]);
        c("个人针", true, ["-s", "-m", `ci(test): ${FAKE_NEEDLE}`]);
        c("作者不是维护者", true, ["-s", "-m", "ci(test): 换人"], ["-c", "user.email=other@example.com"]);
    } finally {
        repo.cleanup();
        fs.rmSync(nf, { force: true });
    }
    return caseResult(cases);
}

function main(argv) {
    if (argv[0] === "--self-test" || argv[0] === "-SelfTest") {
        const r = selfTest();
        for (const l of r.lines) console.error(l);
        console.log(`commit-msg --self-test：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`);
        return r.ok ? 0 : 1;
    }
    const cwd = process.cwd();
    let errors;
    try {
        assertNodeVersion();
        const ident = (k) => git(["var", k], { cwd }).trim();
        errors = checkCommitMsg(fs.readFileSync(argv[0], "utf8"), {
            authorIdent: ident("GIT_AUTHOR_IDENT"),
            committerIdent: ident("GIT_COMMITTER_IDENT"),
            personal: loadPersonalNeedles({ cwd }).needles,
        });
    } catch (e) {
        errors = [e.message];
    }
    if (!errors.length) return 0;
    console.error("commit-msg 拒绝提交：");
    for (const e of errors) console.error(`  ${e}`);
    return 1;
}

if (isMain(import.meta.url)) process.exitCode = main(process.argv.slice(2));
