# 发版

Rinne 的发版只有一条路：维护者在 `prod` 分支上打一个 `v*` tag，`.github/workflows/release.yml` 接着跑
verify-tag → build → package → publish，最后建一个**草稿** Release。维护者核对后自己发布。

本页是维护者一侧的操作手册与发版清单。版本号的唯一真源是 `CMakeLists.txt` 的
`project(Rinne VERSION …)`；资产名在 release.yml 与 `packaging/manifest-*.txt` 里；
许可口径见 [ADR-0007](adr/0007-licensing.md)。

## 版本号的唯一真源

版本号写在顶层 `CMakeLists.txt` 的 `project(Rinne VERSION x.y.z)` 一行里，别的文件不写死版本号。
`scripts/lib/release-version.ps1` 只认这一行（`cmake_minimum_required(VERSION 3.22)` 也含 VERSION，
「取第一条含 VERSION 的行」会拿到 3.22；行首锚定，被 `#` 注释掉的 `project(...)` 不算）。
发版时改这一行，切 CHANGELOG 的小节，README 的状态行改成已发布。

## Tag 规则

只接受两种形态：`vX.Y.Z`（正式版）与 `vX.Y.Z-rc.N`（预发布）。X.Y.Z 必须等于 CMake 里的版本；
rc 不改 CMakeLists。

其余一律拒，例如 `vX.Y.Z-rc`、`vX.Y.Z-rc.1.2`、`vX.Y.Z-RC.1`、`vX.Y.Z-beta.1`、`vX.Y.Z-alpha.1`、
`v0.0.0-test`、`X.Y.Z`（无前导 `v`）、`vX.Y`、带尾随空格或换行的、多行的与空串。**不做演练 tag**：
发布路径靠 `check-release-tag.ps1 -SelfTest`、`check-release-ready` 自测与 `package.ps1 -Preflight` 兜底。

一个 tag 要同时满足下面五条，任何一条不成立，流水线在 verify-tag 就红：

1. **形态**：上面那两种之一。
2. **版本**：X.Y.Z 等于 `project(Rinne VERSION x.y.z)`。
3. **来源**：tag 指向的提交是 `origin/prod` 的祖先。tag 只打在 prod 上。
4. **CHANGELOG**：有与本次版本对应的小节（`## [0.9.0]` 或 `## [0.9.0-rc.1]`，整行只有版本号）。
5. **源码**：正式版的源码与最后一个 rc 相同。比对范围是 `src`、`web`、`cmake`、`CMakeLists.txt`、
   `third_party`、`tests`；只差文档不算。rc 不受这一条约束。

判据是纯函数，不联网、不碰真实仓库；`pwsh scripts/check-release-tag.ps1 -SelfTest` 跑判据自测。

## CHANGELOG 约定

`CHANGELOG.md` 只用英文，格式跟 Keep a Changelog 1.1.0，版本跟 SemVer。节标题逐字是
`## [0.9.0-rc.1]`、`## [0.9.0]`；`## [Unreleased]` 是还没切出来的那节。

## 发版流水线现状

五个 job：`verify-tag`（tag 门禁自测与实判、发版就绪检查、打包前置检查）→ `build`（以
`workflow_call` 调用 `build.yml` 的 release 模式：构建、ctest、pluginval、auval）→ `package-win` 与
`package-mac`（取回产物，打成发行 zip、`.sha256` 与 summary；mac 用 ditto 压缩，保留可执行位与符号链接）
→ `publish`（收齐 6 件、核哈希、建**草稿** Release）。前四个 job 只有 `contents: read`。

- **tag 怎么传给 build**：靠 `github` 上下文隐式传递。`release.yml` 由 `push` tag 触发，被调用方拿到的
  `github.ref` 就是 `refs/tags/<tag>`、`github.ref_name` 就是 tag 名、`github.sha` 就是被 tag 的提交；
  `with:` 留空，因为 tag 是不可信文本，检查禁止在 `with:` 里插值表达式。
- **谁持写权限**：只有 `publish`。**任一红不建草稿**：`publish` 的 `needs` 同时含两个打包 job。
- **重跑**：已有草稿 ⇒ 覆盖上传 6 件并把正文重置为本次渲染的结果；已**发布**的 Release 一律判红。
  rc 勾 pre-release，正式版不勾。**node 版本**：用到 node 的 job 都在 runner 上断言主版本 ≥ 22。

## Release 的六个资产

恰好 6 个，多一个少一个都红：

- `Rinne-v<X.Y.Z>-win64.zip`：2 个 `.vst3` + `INSTALL.txt` + `LICENSE` + `LICENSES/` +
  `THIRD-PARTY-NOTICES.md` + `third_party/notices/` + `SHA256SUMS`
- `Rinne-v<X.Y.Z>-win64.zip.sha256`：`<小写 hex>␣␣<文件名>` + LF
- `package-summary-win64.md`：版本、tag、40 位源码 commit、juceCommit、字节数、SHA-256、zip 内逐条目哈希
- `Rinne-v<X.Y.Z>-macos-arm64.zip`：2 个 `.vst3` + 2 个 `.component` + `install-macos.sh` +
  `uninstall-macos.sh` + 上面那批合规文件 + `SHA256SUMS`
- `Rinne-v<X.Y.Z>-macos-arm64.zip.sha256`：同上格式
- `package-summary-macos-arm64.md`：同上，另含 auval 的结果

zip 的根条目与 `packaging/manifest-*.txt` 逐项比对（集合相等且数量相等）。kit 包（带
`tools/rinne_nulltest` 的那套）只进 Actions artifact，**不进 Release**。

## 发版清单

每次发版复制这一节进 dev→prod 的 PR 正文勾选。

- [ ] **第 0 步（首次公开发布前做一次）**：商标检索完成（USPTO、CIPO 第 9、41 类）；
      `CLAUDE_CODE_OAUTH_TOKEN` 剩余有效期 > 30 天。
- [ ] CHANGELOG 切出 `[<版本>]` 小节；改 `CMakeLists.txt` 的版本；改 README 的状态行。
- [ ] 三语审校：`docs/hard-rules.i18n.json` 的 `frReview.status` 改成 `reviewed`，填上 `reviewedBy`、
      `reviewedZhSha256`（当前中文的合并哈希）与 `approvalUrl`（确认链接）。
- [ ] 强声明有证据：`docs/claims.json` 里每条声明的 evidence 都非空且成立（为空即红）；
      `docs/BUILDING.md` 存在（INSTALL.txt 指向它的构建步骤，缺了发版就红）。
- [ ] 本地 `pwsh scripts/gates.ps1` 全量；`node scripts/check-release-ready.mjs --version <版本>`；
      NOTICES、`LICENSES/`、钉版复核；`node scripts/check-upstream.mjs --drift` 的输出贴进 PR。
- [ ] feature→dev 由维护者批准后合；dev→prod 的 PR 跑完整 gate。
- [ ] 在 prod 上打 tag 并推 tag。**只有维护者能做这一步。**
- [ ] 核对草稿：恰好 6 个资产；两个 `.sha256` 用 INSTALL.txt 里教给用户的命令自验（Windows
      `Get-FileHash -Algorithm SHA256`，macOS `shasum -a 256 -c`）；zip 根条目逐项对照清单。
- [ ] 维护者手动发布；发布后：官网交接、Discussions 公告、更新 `docs/daw-tiers.json`。

### 发布说明模板

发布流水线的 `publish` 步把下面模板块渲染成 Release 正文：`{{…}}` 逐个替换成这一版的实际值。第一行是两个资产直链，第二行写「不要下载 Source code」。

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
| Rinne | tag `{{TAG}}` (commit `{{SOURCE_COMMIT}}`) |
| JUCE | commit `{{JUCE_COMMIT}}` (bundles the VST3 SDK and the AudioUnitSDK) |
| WebView2 SDK | `{{WEBVIEW2_VERSION}}` (Windows only; binary only, published by Microsoft) |
| Build steps | `docs/BUILDING.md` at the tag `{{TAG}}` |

This repository does not hold the complete corresponding source of the Windows binaries: Microsoft publishes
the WebView2 loader as a binary only. macOS uses the system WKWebView.

## Rolling back

A published release is never rewritten. If this version is bad, publish a PATCH and add
"Superseded by v<版本>" on top of its notes.

## 中文摘要

- 两个插件一起升级，版本必须相同；工程文件的兼容性见 CHANGELOG。
- 下载后先校验 SHA-256：Windows `Get-FileHash -Algorithm SHA256`，macOS `shasum -a 256 -c`。
- macOS 的包没有签名也没有公证：先解锁再打开 DAW（运行 zip 里的 `install-macos.sh`）；弹出无法验证时
  不要点「移到废纸篓」，点「完成」再重跑安装脚本。
- 测试过的宿主见 `docs/DAW_COMPATIBILITY.md`，已知问题见 `docs/KNOWN_ISSUES.md`。
- 对应源码：Rinne 的 tag `{{TAG}}`（commit `{{SOURCE_COMMIT}}`）、JUCE 的 commit `{{JUCE_COMMIT}}`、
  WebView2 SDK `{{WEBVIEW2_VERSION}}`、构建步骤见该 tag 上的 `docs/BUILDING.md`。
<!-- END release-notes -->

## 附录 A：维护者依赖与交接

发版要用的工具与钉版真源（版本一律从钉版文件读，不写死版本号字面量）：Node、actionlint、clang-format、
prettier、gitleaks、pluginval、sccache（各自的 `-version` 文件）、JUCE（`.juce-version` +
`.juce-commit`，CMake 守卫断言检出就是那个 commit）。

打包与发布用的脚本：`scripts/package.ps1`（Windows 发行包；`-Preflight` 只跑不依赖构建产物的检查）、
`scripts/package-macos.sh`（macOS 发行包，ditto 压缩）、`scripts/check-release-tag.ps1`（tag 门禁）、
`scripts/check-release-ready.mjs`（发版就绪检查，也渲染正文）、`scripts/gen-docs.d/provenance.mjs`
（逐文件来源表）、`scripts/gates.d/60-package.ps1`（Full 档打包 gate）。

交接时说清三件事：草稿由流水线建、由维护者手动发布；tag 只打在 prod 上；改到流水线与合并判据的 PR
不自合，要维护者批准。

## 附录 B：仓库设置目标态

每项给出回读命令与期望值。实际值以 `gh api` 回读为准。仓库是 `synchain-oss/rinne`，命令里省略它。

| 项 | 回读命令 | 期望值 |
| --- | --- | --- |
| 分支保护 | `gh api branches/dev/protection` | required checks 无 paths 过滤；`required_approving_review_count` 为 0；对话必须解决 |
| 合并方式 | `gh api --jq '{squash_merge_commit_title,merge_commit_title,rebase_merge_commit}'` | squash 与 merge 提交开，rebase 关 |
| 合并消息 | `gh api --jq '{squash_merge_commit_title,squash_merge_commit_message,merge_commit_title,merge_commit_message}'` | 四项分别是 `PR_TITLE`、`PR_BODY`、`PR_TITLE`、`PR_BODY` |
| 合并不删分支 | `gh api --jq .delete_branch_on_merge` | `false` |
| 密钥扫描与 push 保护 | `gh api --jq '{secret_scanning,push_protection}'` | 都开 |
| Discussions | `gh api --jq .has_discussions` | `true` |
| Actions 白名单 | `gh api actions/permissions/workflow` | 与 `.github/actions-allowlist.json` 对得上 |
| 仓库简介 | `gh api --jq .description` | 等于 README 的 tagline |

`tag ruleset`（`protect-release-tags-v`）保护发布 tag：谁都不能改写已发布的 tag。

## 附录 C：上游同步

本仓从姊妹仓 [synchain-oss/scvb](https://github.com/synchain-oss/scvb) 复制过代码。复制来的文件在
`upstream/<area>.json` 里登记（上游提交与每个文件的 blob），文件头恰好一行
`Derived from synchain-oss/scvb; see upstream/<area>.json`。判定与漂移比对：

```sh
node scripts/check-upstream.mjs --self-test   # 判据自测
node scripts/run-checks.mjs --only upstream   # 合并校验全部片段
node scripts/check-upstream.mjs --drift       # 在线比对上游（要能联网）
```

只在三种情况同步：上游修了安全、崩溃或实时安全问题；来源分支已合进上游主干、需要改指；首个正式版
之前核对仍指向未合并分支的来源。一次同步一个 PR。见 [ADR-0008](adr/0008-shared-code.md)。
