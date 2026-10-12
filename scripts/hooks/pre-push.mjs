// SPDX-License-Identifier: GPL-3.0-or-later
//
// pre-push hook 的判据（外壳是同目录的 pre-push）。推上去就公开了：refs/pull 与 fork 会永久保留，
// 删分支也收不回，所以推之前逐条把关：
//   1. 远端 ref 只放行 refs/heads/feat/<slug>（含删除），分支名过私有编号与个人针；已推送的分支只许快进。
//      tag 只在 RINNE_ALLOW_TAG_PUSH=1 时放行，只能指向远端已有的提交，附注 tag 的 tagger 与消息也查。
//      长期分支一律拒绝。
//   2. 推到 GitHub 时 URL 必须带 DLsnows@，且 gh 的 DLsnows 凭据确实解析为 DLsnows。
//   3. 远端还没有的每个提交：作者与提交者是维护者身份；check-commits 同仓规则；
//      隐私规则与个人针（消息、新增内容、新路径）。
//   4. gitleaks 扫同一批提交，版本必须等于 .gitleaks-version（RINNE_GITLEAKS 或 git config rinne.gitleaks）。
//   node scripts/hooks/pre-push.mjs <远端名> <URL>（ref 更新行从 stdin 读）
//   node scripts/hooks/pre-push.mjs --self-test

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { MAINTAINER, checkBranch, checkMessage } from "../check-commits.mjs";
import { scanCommits, formatFinding } from "../check-privacy.mjs";
import { FAKE_NEEDLE, loadPersonalNeedles, mask, redact, scan } from "../lib/privacy-needles.mjs";
import * as kit from "../lib/check-kit.mjs";
import { HOOKS } from "./commit-msg.mjs";

const { ROOT, caseResult, git, j, localSetting, tempRepo } = kit;
const ZERO = /^0+$/;

/** stdin 的每一行是「本地 ref 本地 sha 远端 ref 远端 sha」；格式不对的行不能悄悄丢掉。 */
export function parseUpdates(stdin) {
    return stdin
        .split(/\r?\n/)
        .filter((l) => l.trim())
        .map((l) => {
            const p = l.trim().split(/\s+/);
            const sha = /^[0-9a-f]{40,64}$/;
            if (p.length !== 4 || !sha.test(p[1]) || !sha.test(p[3])) throw new Error("pre-push 的输入行格式不对");
            return p;
        });
}

export function checkRef(remoteRef, { env = process.env, personal = [] } = {}) {
    if (remoteRef.startsWith("refs/heads/")) {
        const name = remoteRef.slice("refs/heads/".length);
        if (name.startsWith("feat/")) return checkBranch({ head: name, personal });
        return [`只允许推 refs/heads/feat/*：${mask(remoteRef, personal)}`];
    }
    if (/^refs\/tags\/v\d/.test(remoteRef) && env.RINNE_ALLOW_TAG_PUSH === "1") return [];
    return [`拒绝推 ${mask(remoteRef, personal)}`];
}

/** 非 GitHub 远端（本地路径等）不涉及公开，不查账号。 */
export function checkRemote(url, ghLogin) {
    if (!/github\.com[/:]/i.test(url)) return [];
    if (!/^https:\/\/DLsnows@github\.com\//.test(url)) return ["推到 GitHub 的远端 URL 必须带 DLsnows@"];
    return ghLogin() === "DLsnows" ? [] : ["gh 的 DLsnows 凭据没有解析为 DLsnows，拒绝推送"];
}

/** 附注 tag 自带 tagger 与消息，与它指向的提交无关，要单独查；轻量 tag 没有这些。 */
function checkTag(sha, cwd, personal) {
    if (git(["cat-file", "-t", sha], { cwd }).trim() !== "tag") return [];
    const raw = git(["cat-file", "tag", sha], { cwd });
    const cut = raw.indexOf("\n\n");
    const tagger = /^tagger (.+?) \d+ [+-]\d{4}$/m.exec(cut < 0 ? raw : raw.slice(0, cut))?.[1];
    const errors = tagger === MAINTAINER ? [] : [`tag 的 tagger 必须是 ${MAINTAINER}`];
    const message = cut < 0 ? "" : raw.slice(cut + 2);
    for (const f of scan(message, {}, { personal })) errors.push(`tag 消息第 ${f.line} 行 [${f.rule.id}] ${redact(f)}`);
    return errors;
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
    // gitleaks 自己跑 git log -p；本机的外部 diff 与 textconv 驱动会改写补丁，一并关掉
    const logOpts = [...revArgs, "--no-ext-diff", "--no-textconv"].join(" ");
    const args = ["git", "--no-banner", "--no-color", "--redact", "-v", `--log-opts=${logOpts}`, "."];
    const r = spawnSync(bin, args, { cwd, env, encoding: "utf8" });
    if (r.status === 0) return [];
    return [
        "gitleaks 发现疑似凭据（已打码）：",
        ...`${r.stdout}${r.stderr}`
            .trim()
            .split("\n")
            .map((l) => `  ${l}`),
    ];
}

export function prePush({ updates, remote, url, cwd, env, needles, login = () => ghLogin(env) }) {
    const errors = [];
    for (const [, localSha, remoteRef, remoteSha] of updates) {
        errors.push(...checkRef(remoteRef, { env, personal: needles }));
        if (ZERO.test(localSha)) continue; // 删除远端分支：不带任何内容
        const hasOld = !ZERO.test(remoteSha) && git(["cat-file", "-e", remoteSha], { cwd, allowFail: true }) !== null;
        if (remoteRef.startsWith("refs/heads/") && !ZERO.test(remoteSha)) {
            // 已推送的分支只许快进，不改写已经公开的历史
            if (!hasOld) errors.push("远端分支的当前提交本机没有：先 fetch 再推");
            else if (git(["merge-base", "--is-ancestor", remoteSha, localSha], { cwd, allowFail: true }) === null) {
                errors.push("不允许非快进推送：已推送的历史不能改写");
            }
        }
        // 只看远端还没有的提交：已在远端任一分支上的早就公开了
        const revArgs = [localSha, ...(hasOld ? [`^${remoteSha}`] : []), "--not", `--remotes=${remote}`];
        const shas = git(["rev-list", ...revArgs], { cwd })
            .split("\n")
            .filter(Boolean);
        if (remoteRef.startsWith("refs/tags/")) {
            if (shas.length) errors.push("tag 只能指向远端已有的提交");
            errors.push(...checkTag(localSha, cwd, needles));
            continue;
        }
        if (!shas.length) continue;
        for (const sha of shas) {
            const fields = git(["log", "-1", "--format=%an <%ae>%x00%cn <%ce>%x00%P%x00%B", sha], { cwd });
            const [author, committer, parents, message] = fields.split("\0");
            const s7 = sha.slice(0, 7);
            if (author !== MAINTAINER || committer !== MAINTAINER)
                errors.push(`${s7} 作者与提交者必须是 ${MAINTAINER}`);
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
    const ref = (r, env = {}) => checkRef(r, { env, personal: [FAKE_NEEDLE] });
    t("ref：feat/<slug>", false, ref("refs/heads/feat/check-scripts"));
    t("ref：dev", true, ref("refs/heads/dev"));
    t("ref：feature/v1", true, ref("refs/heads/feature/v1"));
    t("ref：分支名含卡号", true, ref(j("refs/heads/feat/r", "n-01a")));
    t("ref：分支名含个人针", true, ref(`refs/heads/feat/${FAKE_NEEDLE}`));
    t("ref：未授权的 tag", true, ref("refs/tags/v0.9.0-rc.1"));
    t("ref：授权后的 tag", false, ref("refs/tags/v0.9.0-rc.1", { RINNE_ALLOW_TAG_PUSH: "1" }));
    const gh = "https://DLsnows@github.com/synchain-oss/rinne.git";
    t(
        "远端：GitHub URL 不带 DLsnows@",
        true,
        checkRemote(gh.replace("DLsnows@", ""), () => "DLsnows"),
    );
    t(
        "远端：凭据解析成别的账号",
        true,
        checkRemote(gh, () => "someone-else"),
    );
    t(
        "远端：DLsnows",
        false,
        checkRemote(gh, () => "DLsnows"),
    );
    const z = "0".repeat(40);
    t(
        "输入：四段格式",
        false,
        parseUpdates(`refs/heads/feat/x ${"a".repeat(40)} refs/heads/feat/x ${z}\n`).length ? [] : ["x"],
    );
    let threw = false;
    try {
        parseUpdates("refs/heads/feat/x deadbeef\n");
    } catch {
        threw = true;
    }
    cases.push({ label: "输入：格式不对的行判红", bad: true, passed: threw });

    // 端到端：经真实的 sh 外壳推到本地裸仓。坏提交在不挂 hook 的临时仓里做出来，推送时才挂 hook。
    const gitleaks = localSetting("RINNE_GITLEAKS", "rinne.gitleaks", { cwd: ROOT }) || "gitleaks";
    if (run(gitleaks, ["version"], process.env).status !== 0) {
        const note = "找不到 gitleaks，端到端部分没跑（设置 RINNE_GITLEAKS 或 git config rinne.gitleaks）";
        // CI 上必须跑全：缺 gitleaks 直接判红；本机只记一条说明
        if (kit.inCI()) cases.push({ label: note, bad: true, passed: false });
        const r = caseResult(cases);
        if (!kit.inCI()) r.lines.push(note);
        return r;
    }
    const remote = tempRepo({ bare: true });
    const repo = tempRepo();
    const nf = `${repo.dir}-needles.txt`;
    fs.writeFileSync(nf, `${FAKE_NEEDLE}\n`);
    const env = { ...repo.env, RINNE_NODE: process.execPath, RINNE_PRIVACY_LOCAL: nf, RINNE_GITLEAKS: gitleaks };
    const push = (label, bad, refspec, extra = [], more = {}) => {
        const args = ["-c", `core.hooksPath=${HOOKS}`, "push", "-q", ...extra, "origin", refspec];
        const ok = repo.git(args, { env: { ...env, ...more }, allowFail: true });
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
    const other = ["-c", "user.name=Other", "-c", "user.email=other@example.com"];
    try {
        repo.git(["remote", "add", "origin", remote.dir]);
        commit("docs/a.md", "干净\n", `docs(test): 干净${signed}`);
        push("好提交推到 feat/", false, "HEAD:refs/heads/feat/ok");
        commit("docs/b.md", "干净\n", `docs(test): 再加一个${signed}`);
        push("快进推送", false, "HEAD:refs/heads/feat/ok");
        push("推长期分支", true, "HEAD:refs/heads/dev");
        push("分支名含卡号", true, j("HEAD:refs/heads/feat/r", "n-01a"));
        push("分支名含个人针", true, `HEAD:refs/heads/feat/${FAKE_NEEDLE}`);
        const tip = repo.git(["rev-parse", "HEAD"]).trim();
        repo.git(["reset", "-q", "--hard", "HEAD~1"]);
        commit("docs/c.md", "干净\n", `docs(test): 改写历史${signed}`);
        push("强推改写已推送的历史", true, "HEAD:refs/heads/feat/ok", ["--force"]);
        repo.git(["reset", "-q", "--hard", tip]);
        bad("新增内容含个人针", "docs/d.md", `${FAKE_NEEDLE}\n`, `docs(test): 夹带${signed}`);
        bad("新路径含卡号（空文件，补丁里没有 +++ 行）", j("docs/R", "N-01.md"), "", `docs(test): 路径${signed}`);
        bad("提交消息缺签名", "docs/e.md", "x\n", "docs(test): 无签名");
        bad("作者不是维护者", "docs/f.md", "x\n", `docs(test): 换人${signed}`, other);
        const leak = j("gh", "p_", "Zr8XcV2nQ7tLm4Kp9Ws6Yb3Hd5Fg1Jq0Ae2T");
        bad("gitleaks 抓到凭据", "src/g.txt", `auth = "${leak}"\n`, `docs(test): 凭据${signed}`);
        const allow = { RINNE_ALLOW_TAG_PUSH: "1" };
        repo.git(["tag", "-a", "v0.0.1", "-m", "v0.0.1"]);
        push("授权后的附注 tag", false, "refs/tags/v0.0.1", [], allow);
        repo.git([...other, "tag", "-a", "v0.0.2", "-m", `发布 ${FAKE_NEEDLE}`]);
        push("附注 tag 的 tagger 与消息不合规", true, "refs/tags/v0.0.2", [], allow);
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
        kit.assertNodeVersion();
        const updates = parseUpdates(fs.readFileSync(0, "utf8"));
        const { needles } = loadPersonalNeedles({ cwd, mode: "local" });
        errors = prePush({ updates, remote, url, cwd, env: process.env, needles });
    } catch (e) {
        errors = [e.message];
    }
    if (!errors.length) return 0;
    console.error("pre-push 拒绝推送：");
    for (const e of errors) console.error(`  ${e}`);
    return 1;
}

if (kit.isMain(import.meta.url)) process.exitCode = main(process.argv.slice(2));
