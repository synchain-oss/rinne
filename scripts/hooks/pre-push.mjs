// SPDX-License-Identifier: GPL-3.0-or-later
//
// pre-push hook 的判据（外壳是同目录的 pre-push）。推上去就公开了：refs/pull 与 fork 会永久保留，
// 删分支也收不回，所以推之前逐条把关：
//   1. 远端 ref 只放行 refs/heads/feat/<slug>（含删除）；tag 只在 RINNE_ALLOW_TAG_PUSH=1 时放行，
//      且只能指向远端已有的提交；长期分支一律拒绝。
//   2. 推到 GitHub 时 URL 必须带 DLsnows@，且 gh 的 DLsnows 凭据确实解析为 DLsnows。
//   3. 远端还没有的每个提交：作者与提交者是维护者身份；check-commits 同仓规则；
//      隐私规则与个人针（消息、新增行、新路径）。
//   4. gitleaks 扫同一批提交，版本必须等于 .gitleaks-version（RINNE_GITLEAKS 或 git config rinne.gitleaks）。
//   node scripts/hooks/pre-push.mjs <远端名> <URL>（ref 更新行从 stdin 读）
//   node scripts/hooks/pre-push.mjs --self-test

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { MAINTAINER, checkBranch, checkMessage } from "../check-commits.mjs";
import { scanCommits, formatFinding } from "../check-privacy.mjs";
import { FAKE_NEEDLE, loadPersonalNeedles } from "../lib/privacy-needles.mjs";
import { ROOT, assertNodeVersion, caseResult, git, isMain, j, localSetting, tempRepo } from "../lib/check-kit.mjs";
import { HOOKS } from "./commit-msg.mjs";

const ZERO = /^0+$/;

export function checkRef(remoteRef, env = process.env) {
    if (remoteRef.startsWith("refs/heads/")) {
        const name = remoteRef.slice("refs/heads/".length);
        return name.startsWith("feat/") ? checkBranch({ head: name }) : [`只允许推 refs/heads/feat/*：${remoteRef}`];
    }
    if (/^refs\/tags\/v\d/.test(remoteRef) && env.RINNE_ALLOW_TAG_PUSH === "1") return [];
    return [`拒绝推 ${remoteRef}`];
}

/** 非 GitHub 远端（本地路径等）不涉及公开，不查账号。 */
export function checkRemote(url, ghLogin) {
    if (!/github\.com[/:]/i.test(url)) return [];
    if (!/^https:\/\/DLsnows@github\.com\//.test(url)) return ["推到 GitHub 的远端 URL 必须带 DLsnows@"];
    return ghLogin() === "DLsnows" ? [] : ["gh 的 DLsnows 凭据没有解析为 DLsnows，拒绝推送"];
}

const run = (cmd, args, env) => spawnSync(cmd, args, { env, encoding: "utf8" });

function ghLogin(env) {
    const token = env.GH_TOKEN || run("gh", ["auth", "token", "--user", "DLsnows"], env).stdout?.trim();
    return (run("gh", ["api", "user", "--jq", ".login"], { ...env, GH_TOKEN: token }).stdout || "").trim();
}

function gitleaksCheck(revArgs, cwd, env) {
    const bin = localSetting("RINNE_GITLEAKS", "rinne.gitleaks", { env, cwd }) || "gitleaks";
    const want = fs.readFileSync(path.join(ROOT, ".gitleaks-version"), "utf8").trim();
    const have = (run(bin, ["version"], env).stdout || "").trim().replace(/^v/, "");
    if (have !== want) return [`gitleaks 版本应为 ${want}（.gitleaks-version），实际 ${have || "找不到"}`];
    const args = ["git", "--no-banner", "--no-color", "--redact", "-v", `--log-opts=${revArgs.join(" ")}`, "."];
    const r = spawnSync(bin, args, { cwd, env, encoding: "utf8" });
    if (r.status === 0) return [];
    return ["gitleaks 发现疑似凭据（已打码）：", ...`${r.stdout}${r.stderr}`.trim().split("\n").map((l) => `  ${l}`)];
}

export function prePush({ updates, remote, url, cwd, env, needles, login = () => ghLogin(env) }) {
    const errors = [];
    for (const [, localSha, remoteRef, remoteSha] of updates) {
        errors.push(...checkRef(remoteRef, env));
        if (ZERO.test(localSha)) continue; // 删除远端分支：不带任何内容
        // 只看远端还没有的提交：已在远端任一分支上的早就公开了
        const hasOld = !ZERO.test(remoteSha) && git(["cat-file", "-e", remoteSha], { cwd, allowFail: true }) !== null;
        const revArgs = [localSha, ...(hasOld ? [`^${remoteSha}`] : []), "--not", `--remotes=${remote}`];
        const shas = git(["rev-list", ...revArgs], { cwd }).split("\n").filter(Boolean);
        if (!shas.length) continue;
        if (remoteRef.startsWith("refs/tags/")) {
            errors.push("tag 只能指向远端已有的提交");
            continue;
        }
        for (const sha of shas) {
            const fields = git(["log", "-1", "--format=%an <%ae>%x00%cn <%ce>%x00%P%x00%B", sha], { cwd });
            const [author, committer, parents, message] = fields.split("\0");
            const s7 = sha.slice(0, 7);
            if (author !== MAINTAINER || committer !== MAINTAINER) errors.push(`${s7} 作者与提交者必须是 ${MAINTAINER}`);
            const merge = parents.trim().includes(" ");
            errors.push(...checkMessage(message, { merge, personal: needles }).errors.map((e) => `${s7} ${e}`));
        }
        errors.push(...scanCommits(revArgs, cwd, needles, { messages: false }).map(formatFinding));
        errors.push(...gitleaksCheck(revArgs, cwd, env));
    }
    if (updates.length) errors.push(...checkRemote(url, login));
    return errors;
}

export function selfTest() {
    const cases = [];
    const t = (label, bad, errors) => cases.push({ label, bad, passed: bad ? errors.length > 0 : errors.length === 0 });
    t("ref：feat/<slug>", false, checkRef("refs/heads/feat/check-scripts", {}));
    t("ref：dev", true, checkRef("refs/heads/dev", {}));
    t("ref：feature/v1", true, checkRef("refs/heads/feature/v1", {}));
    t("ref：分支名含卡号", true, checkRef(j("refs/heads/feat/r", "n-01a"), {}));
    t("ref：未授权的 tag", true, checkRef("refs/tags/v0.9.0-rc.1", {}));
    t("ref：授权后的 tag", false, checkRef("refs/tags/v0.9.0-rc.1", { RINNE_ALLOW_TAG_PUSH: "1" }));
    const gh = "https://DLsnows@github.com/synchain-oss/rinne.git";
    t("远端：GitHub URL 不带 DLsnows@", true, checkRemote(gh.replace("DLsnows@", ""), () => "DLsnows"));
    t("远端：凭据解析成别的账号", true, checkRemote(gh, () => "someone-else"));
    t("远端：DLsnows", false, checkRemote(gh, () => "DLsnows"));

    // 端到端：经真实的 sh 外壳推到本地裸仓。坏提交在不挂 hook 的临时仓里做出来，推送时才挂 hook。
    const gitleaks = localSetting("RINNE_GITLEAKS", "rinne.gitleaks", { cwd: ROOT }) || "gitleaks";
    if (run(gitleaks, ["version"], process.env).status !== 0) {
        console.error("pre-push --self-test：找不到 gitleaks，跳过端到端部分（设置 RINNE_GITLEAKS）");
        return caseResult(cases);
    }
    const remote = tempRepo({ bare: true });
    const repo = tempRepo();
    const nf = `${repo.dir}-needles.txt`;
    fs.writeFileSync(nf, `${FAKE_NEEDLE}\n`);
    const env = { ...repo.env, RINNE_NODE: process.execPath, RINNE_PRIVACY_LOCAL: nf, RINNE_GITLEAKS: gitleaks };
    const push = (label, bad, refspec) => {
        const args = ["-c", `core.hooksPath=${HOOKS}`, "push", "-q", "origin", refspec];
        const ok = repo.git(args, { env, allowFail: true });
        cases.push({ label: `端到端：${label}`, bad, passed: bad ? ok === null : ok !== null });
    };
    const signed = "\n\nSigned-off-by: DLsnows <noreply@synchain.ca>";
    const commit = (file, text, msg, gitOpts = []) => {
        repo.write(file, text);
        repo.git(["add", "."]);
        repo.git([...gitOpts, "commit", "-q", "-m", msg]);
    };
    const bad = (label, ...args) => {
        commit(...args);
        push(label, true, "HEAD:refs/heads/feat/ok");
        repo.git(["reset", "-q", "--hard", "HEAD~1"]);
    };
    try {
        repo.git(["remote", "add", "origin", remote.dir]);
        commit("docs/a.md", "干净\n", `docs(test): 干净${signed}`);
        push("好提交推到 feat/", false, "HEAD:refs/heads/feat/ok");
        push("推长期分支", true, "HEAD:refs/heads/dev");
        push("分支名含卡号", true, j("HEAD:refs/heads/feat/r", "n-01a"));
        bad("新增行含个人针", "docs/b.md", `${FAKE_NEEDLE}\n`, `docs(test): 夹带${signed}`);
        bad("新路径含卡号（空文件，补丁里没有 +++ 行）", j("docs/R", "N-01.md"), "", `docs(test): 路径${signed}`);
        bad("提交消息缺签名", "docs/c.md", "x\n", "docs(test): 无签名");
        bad("作者不是维护者", "docs/d.md", "x\n", `docs(test): 换人${signed}`, ["-c", "user.email=other@example.com"]);
        const leak = j("gh", "p_", "Zr8XcV2nQ7tLm4Kp9Ws6Yb3Hd5Fg1Jq0Ae2T");
        bad("gitleaks 抓到凭据", "src/e.txt", `auth = "${leak}"\n`, `docs(test): 凭据${signed}`);
        push("删除 feat/ 分支", false, ":refs/heads/feat/ok");
    } finally {
        repo.cleanup();
        remote.cleanup();
        fs.rmSync(nf, { force: true });
    }
    return caseResult(cases);
}

function main(argv) {
    if (argv[0] === "--self-test" || argv[0] === "-SelfTest") {
        const r = selfTest();
        for (const l of r.lines) console.error(l);
        console.log(`pre-push --self-test：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`);
        return r.ok ? 0 : 1;
    }
    const [remote, url = ""] = argv;
    const cwd = process.cwd();
    let errors;
    try {
        assertNodeVersion();
        const updates = fs
            .readFileSync(0, "utf8")
            .split("\n")
            .map((l) => l.trim().split(/\s+/))
            .filter((p) => p.length === 4);
        const { needles } = loadPersonalNeedles({ cwd });
        errors = prePush({ updates, remote, url, cwd, env: process.env, needles });
    } catch (e) {
        errors = [e.message];
    }
    if (!errors.length) return 0;
    console.error("pre-push 拒绝推送：");
    for (const e of errors) console.error(`  ${e}`);
    return 1;
}

if (isMain(import.meta.url)) process.exitCode = main(process.argv.slice(2));
