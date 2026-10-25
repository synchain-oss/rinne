# 发版

Rinne 的发版只有一条路：维护者在 `prod` 分支上打一个 `v*` tag，`.github/workflows/release.yml` 接着跑
verify-tag → build → package → notes → publish，最后建一个**草稿** Release。维护者核对后自己发布。

本页是维护者一侧的操作手册与发版清单；许可口径见 [ADR-0007](adr/0007-licensing.md)。

## 版本号的唯一真源

版本号只写在顶层 `CMakeLists.txt` 的 `project(Rinne VERSION x.y.z)` 一行里，别的文件不写死。
发版时改它，切 CHANGELOG 的小节，把 README 的状态行改成已发布。

## Tag 规则

只接受两种形态：`vX.Y.Z`（正式版）与 `vX.Y.Z-rc.N`（预发布）。X.Y.Z 必须等于 CMake 里的版本；
rc 不改 CMakeLists。其余一律拒（`-beta.1`、`-RC.1`、无前导 `v`、带空白换行的等）。**不做演练 tag**：发布路径靠
`check-release-tag.ps1 -SelfTest`、`check-release-ready` 自测与 `package.ps1 -Preflight` 兜底。

一个 tag 要同时满足下面五条，任何一条不成立，流水线在 verify-tag 就红：

1. **形态**：上面那两种之一。
2. **版本**：X.Y.Z 等于 `project(Rinne VERSION x.y.z)`。
3. **来源**：tag 指向的提交是 `origin/prod` 的祖先。tag 只打在 prod 上。
4. **CHANGELOG**：有与本次版本对应的小节（`## [0.9.0]` 或 `## [0.9.0-rc.1]`，整行只有版本号）。
5. **源码**：正式版的源码与最后一个 rc 相同。最后一个 rc 是 `origin/prod` 上编号最大、形态严格的
   `vX.Y.Z-rc.N`，且是正式版提交的祖先；野标签与不在 prod 上的 rc 不算。比对范围是 `src`、`web`、`cmake`、
   `CMakeLists.txt`、`third_party`、`tests`，外加 `.juce-commit`、`.juce-version`；只差文档、`packaging/`、
   `scripts/` 不算。rc 不受这一条约束。

`-SelfTest` 在临时 git 仓里造 prod 与非 prod 的提交、各种 tag 和改动，把整个门禁（含取数层）走一遍。

## CHANGELOG 约定

`CHANGELOG.md` 只用英文，格式跟 Keep a Changelog 1.1.0，节标题逐字是 `## [0.9.0-rc.1]`、`## [0.9.0]`。

## 发版流水线现状

六个 job：`verify-tag`（tag 门禁与发版就绪各先自测再实判，再做打包前置检查）→ `build`（`workflow_call` 调
`build.yml` 的 release 模式：构建、ctest、pluginval、auval，以及 mac 打包与安装脚本的自测）→ `package-win` 与
`package-mac`（打成发行 zip、`.sha256` 与 summary；mac 用 ditto 压缩）→ `notes`（只读：收齐 6 件、核哈希、渲染正文）→
`publish`（不 checkout、不跑仓库脚本：建**草稿** Release 并读回核对）。

- **tag 怎么传给 build**：靠 `github` 上下文隐式传递（`github.ref`、`github.sha`）；`with:` 留空，因为 tag 是不可信文本。
- **谁持写权限**：只有 `publish`。**任一红不建草稿**：`notes` 的 `needs` 同时含两个打包 job。
- **重跑**：*Re-run failed jobs* 与 *Re-run all jobs* 都行：每处 `upload-artifact` 都带 `overwrite: true`，
  不会撞同名 artifact。已有草稿 ⇒ 覆盖上传 6 件、清掉多出来的资产、重置正文；已**发布**的 Release 一律判红。
  rc 勾 pre-release。

## Release 的六个资产

恰好 6 个，多一个少一个都红：

- `Rinne-v<X.Y.Z>-win64.zip`：2 个 `.vst3` + `INSTALL.txt` + 合规文件 + `SHA256SUMS`
- `Rinne-v<X.Y.Z>-win64.zip.sha256`：`<小写 hex>␣␣<文件名>` + LF
- `package-summary-win64.md`：版本、tag、源码 commit、对应源码指引（含 JUCE 版本与 commit、WebView2 SDK、
  构建步骤）、字节数、SHA-256、zip 内逐条目哈希
- `Rinne-v<X.Y.Z>-macos-arm64.zip`：2 个 `.vst3` + 2 个 `.component` + 安装与卸载脚本 + 同上的文件
- `Rinne-v<X.Y.Z>-macos-arm64.zip.sha256`、`package-summary-macos-arm64.md`：同上

zip 的根条目与 `packaging/manifest-*.txt` 逐项比对（集合与数量都相等）。kit 包（带 `tools/rinne_nulltest`；
没有 tag，对应源码链接钉在 commit 上）只进 Actions artifact，**不进 Release**。

## 发版清单

每次发版复制这一节进 dev→prod 的 PR 正文勾选。

- [ ] **第 0 步（首次公开发布前做一次）**：商标检索完成（USPTO、CIPO 第 9、41 类）；
      `CLAUDE_CODE_OAUTH_TOKEN` 剩余有效期 > 30 天。
- [ ] CHANGELOG 切出 `[<版本>]` 小节；改 `CMakeLists.txt` 的版本；改 README 的状态行。
- [ ] 三语审校（发版前）：AI 以中文（`docs/USER_GUIDE.zh-CN.md`）为准交叉核对 `docs/hard-rules.i18n.json` 的英法译文；
      维护者抽检后，在发版 PR 留一条确认评论。然后把 `frReview.status` 改成 `reviewed`：`reviewedBy` 填留评论的维护者的
      GitHub 用户名，`approvalUrl` 填那条评论的链接，`reviewedZhSha256` 填 `node scripts/gen-docs.mjs --print-hashes`
      的 `combined`。中文一改，记录即过期，要重做。
- [ ] 强声明有证据：`docs/claims.json` 里每条声明的 evidence 都非空且成立；
      `docs/BUILDING.md` 存在（INSTALL.txt 指向它，缺了发版就红）。
- [ ] 本地 `pwsh scripts/gates.ps1` 全量；`node scripts/check-release-ready.mjs --version <版本>`；
      NOTICES、`LICENSES/`、钉版复核；`node scripts/check-upstream.mjs --drift` 的输出贴进 PR。
- [ ] feature→dev 由维护者批准后合；dev→prod 的 PR 跑完整 gate；在 prod 上打 tag 并推 tag（**只有维护者能做**）。
- [ ] 核对草稿：恰好 6 个资产；两个 `.sha256` 用 INSTALL.txt 里教的命令自验；zip 根条目逐项对照清单。
- [ ] 维护者手动发布；发布后：官网交接、Discussions 公告、更新 `docs/daw-tiers.json`。

### 发布说明模板

`notes` 步把下面模板块渲染成 Release 正文：`{{…}}` 逐个替换成这一版的实际值，缺一个就红。第一行是两个资产直链，第二行写「不要下载 Source code」。

<!-- BEGIN release-notes -->
{{HEADER}}

## Upgrade notes

Two plug-ins, one version. Install Rinne Input and Rinne Output of the **same** version over the old ones.
A project saved with a newer version is refused by an older one, and its bytes are kept untouched.

两个插件要一起升级，版本必须相同；新的覆盖旧的。旧版拒绝载入新版保存的工程，并保持原字节不动。

The full list of changes is in `CHANGELOG.md` at the tag.

## Download and verify

| Asset | SHA-256 |
| --- | --- |
| `{{WIN_FILE}}` | `{{WIN_SHA256}}` |
| `{{MAC_FILE}}` | `{{MAC_SHA256}}` |

Windows：`Get-FileHash -Algorithm SHA256 .\{{WIN_FILE}}`；macOS：`shasum -a 256 -c {{MAC_FILE}}.sha256`。

## Unsigned on macOS

The macOS plug-ins are not signed with an Apple Developer ID and not notarized. Unlock first, then open your DAW:
run `install-macos.sh` from the zip. If macOS says it cannot verify the plug-in, do not click "Move to Trash" —
click "Done", run the installer again, retry. Steps in `INSTALL.md` at the tag.

## Hosts tested and known issues

See `docs/DAW_COMPATIBILITY.md` and `docs/KNOWN_ISSUES.md` at the tag. Rinne never claims support for all hosts.

## Corresponding source (GPLv3 §6(d))

| Item | Where |
| --- | --- |
| Rinne | [tag `{{TAG}}`](https://github.com/synchain-oss/rinne/tree/{{TAG}}) (commit `{{SOURCE_COMMIT}}`) |
| JUCE | `{{JUCE_VERSION}}`, [commit `{{JUCE_COMMIT}}`](https://github.com/juce-framework/JUCE/tree/{{JUCE_COMMIT}}) (bundles the VST3 SDK and the AudioUnitSDK) |
| WebView2 SDK | `{{WEBVIEW2_VERSION}}` (Windows only; binary only, published by Microsoft) |
| Build steps | `docs/BUILDING.md` at the tag `{{TAG}}` |

This repository does not hold the complete corresponding source of the Windows binaries: Microsoft publishes
the WebView2 loader as a binary only. macOS uses the system WKWebView.

## Rolling back

A published release is never rewritten. If this version is bad, publish a PATCH and add
"Superseded by vX.Y.Z" (the new version) on top of its notes.

## 中文摘要

- 两个插件一起升级，版本必须相同；工程文件的兼容性见 CHANGELOG。
- 下载后先校验 SHA-256：Windows `Get-FileHash -Algorithm SHA256`，macOS `shasum -a 256 -c`。
- macOS 的包没有签名也没有公证：先解锁再打开 DAW（运行 zip 里的 `install-macos.sh`）；弹出无法验证时
  不要点「移到废纸篓」，点「完成」再重跑安装脚本。
- 测试过的宿主见 `docs/DAW_COMPATIBILITY.md`，已知问题见 `docs/KNOWN_ISSUES.md`。
- 对应源码：Rinne 的 tag `{{TAG}}`（commit `{{SOURCE_COMMIT}}`）、JUCE `{{JUCE_VERSION}}`（commit `{{JUCE_COMMIT}}`）、
  WebView2 SDK `{{WEBVIEW2_VERSION}}`、构建步骤见该 tag 上的 `docs/BUILDING.md`。
<!-- END release-notes -->

## 附录 A：维护者依赖与交接

钉版文件清单见 `CLAUDE.md` 的「环境与钉版」一节，版本不在别处写死。打包与发版脚本：`scripts/package.ps1`、
`scripts/package-macos.sh`、`scripts/check-release-tag.ps1`、`scripts/check-release-ready.mjs`（也渲染正文）、
`scripts/gen-docs.d/provenance.mjs`（逐文件来源表）、`scripts/gates.d/60-package.ps1`。
交接三件事：草稿由流水线建、维护者手动发布；tag 只打在 prod 上；改到流水线与合并判据的 PR 不自合。

## 附录 B：仓库设置目标态

每条是只读回读命令加期望值（先设 `R=repos/synchain-oss/rinne`）。改设置属 GitHub 写操作，
由维护者批准后执行，再用这些命令回读。

- **dev、prod 的分支规则**：`gh api $R/rules/branches/dev --jq '[.[].type]'`（prod 同）⇒ `deletion`、
  `non_fast_forward`、`pull_request`；pull_request 的 `required_approving_review_count` 为 0，
  `required_review_thread_resolution` 为 true。
- **合并与仓库字段**：`gh api $R --jq '{allow_squash_merge,allow_merge_commit,allow_rebase_merge,squash_merge_commit_title,squash_merge_commit_message,merge_commit_title,merge_commit_message,delete_branch_on_merge,has_discussions}'`
  ⇒ squash 与 merge 开、rebase 关；squash 标题 `PR_TITLE`、正文 `COMMIT_MESSAGES`，merge 标题 `PR_TITLE`、正文 `PR_BODY`；
  `delete_branch_on_merge` 为 false；`has_discussions` 为 true。
- **密钥扫描与 push 保护**：`gh api $R --jq '.security_and_analysis|[.secret_scanning.status,.secret_scanning_push_protection.status]'`
  ⇒ 都是 `enabled`。
- **Actions 白名单**：`gh api $R/actions/permissions/selected-actions` ⇒ `github_owned_allowed` 为 true，
  `patterns_allowed` 含清单里的全部项。多出的 `qodo-ai/pr-agent@*`、`softprops/action-gh-release@*` 登记在清单的
  `org_inherited_unused`：它们在 org 级放行，仓库级改不了（PUT 返回 409）。release.yml 有意不用第三方发布 action，
  `workflows` 检查拒绝任何 workflow 引用它们。
- **钉 SHA**：`gh api $R/actions/permissions --jq .sha_pinning_required` ⇒ `true`（已开启）。
- **默认 workflow 权限**：`gh api $R/actions/permissions/workflow` ⇒ `default_workflow_permissions` 为 `read`，
  `can_approve_pull_request_reviews` 为 `false`。
- **tag ruleset**：`gh api $R/rulesets --jq '.[]|select(.target=="tag")|[.id,.name]'` 取 id，再 `gh api $R/rulesets/<id>` ⇒
  名字 `release-tags`，对象 `refs/tags/v*`，规则 `creation`、`update`、`deletion`、`non_fast_forward`；
  admin 角色（RepositoryRole 5）可绕过，其余角色不能创建、移动、删除 `v*` tag。
- **仓库简介**：`gh api $R --jq .description` ⇒ 等于 README 的 tagline。

## 附录 C：上游同步

本仓从姊妹仓 [synchain-oss/scvb](https://github.com/synchain-oss/scvb) 复制过代码，在 `upstream/<area>.json`
里登记（上游提交与每个文件的 blob），文件头恰好一行 `Derived from synchain-oss/scvb; see upstream/<area>.json`。
校验：`node scripts/run-checks.mjs --only upstream`；漂移比对（要联网）：`node scripts/check-upstream.mjs --drift`。

只在三种情况同步：上游修了安全、崩溃或实时安全问题；来源分支已合进上游主干、需要改指；首个正式版之前核对
仍指向未合并分支的来源。一次一个 PR。见 [ADR-0008](adr/0008-shared-code.md)。
