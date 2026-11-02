# ADR-0006：冻结面与版本策略

## Summary (EN)

- **Decision**: contract documents carry a `status:` line (draft or frozen); a frozen surface changes only with a contract-change document or a `contract-neutral:` line in the PR. Plugin identity and the 11 parameter IDs never change. Versions follow SemVer with an audio-specific table, and a release candidate and its final release share the same source.
- **Consequences**: users can upgrade without losing saved projects or automation; early design mistakes after the freeze are fixed by migration or addition, not by rewriting.
- **Compatibility**: from the first public release candidate the freeze is permanent.

## 状态

已采纳（2026-11-02）。

## 背景

宿主按参数 ID 保存自动化，工程里存着 state，两端靠共享内存布局会合。这些东西一旦随版本发出就改不了，所以要先说清楚：什么冻结、何时冻结、怎么改、版本号怎么升。

## 决定

- **冻结面**：`docs/contracts/**`、`tests/golden/**`，以及 `scripts/check-commits.mjs` 登记的源码（IPC 布局头、`src/core/state/**`、`ParamTable.h` 的 `RINNE_PARAM` 行、CMake 里的插件身份行）。
- **阶段**：每份契约有一行 `status: draft` 或 `status: frozen`。参数与 state 已冻结；IPC 布局在真实宿主探测完成前保持 draft。首个公开 rc 起永久冻结。守卫读 base 上的状态，同一个 PR 把 frozen 改回 draft 放行不了自己。
- **改冻结面**：同一个 PR 新增 `docs/contract-changes/<YYYYMMDD>-<slug>.md`，或在正文写一行 `contract-neutral: <理由>`。永久冻结后另加 `status/frozen-contract` 标签，变更文档的「批准」栏填维护者确认评论的链接。
- **永不改**：插件身份（插件码、bundle id、AU 三元组）、11 个参数 ID、会合段名不带 abi，以及会合段头与 state 头前 16 字节的位置和含义。
- **版本号**：唯一真源是 `project(Rinne VERSION …)`，别处不写死。当前是 0.9.0，首个公开版本按 rc 先出。tag 只在维护者明确授权后打。
- **正式版与 rc 同源**：`vX.Y.Z` 在 `src`、`web`、`cmake`、`CMakeLists.txt`、`third_party`、`tests` 上必须与最后一个 rc 相同，由 verify-tag 比对。

SemVer 对音频产品的具体化：

| 变更 | 版本位 |
| --- | --- |
| IPC abi 升级 | MAJOR |
| 插件身份、参数 ID | 不允许 |
| 新增参数 | MINOR |
| state abi 升级（带迁移） | MINOR，CHANGELOG 写 Compatibility |
| 默认值、安全默认值、可听的 DSP 行为、实际环路时间的变化 | MINOR，CHANGELOG 写 Compatibility |
| 修复、文档、构建 | PATCH |

## 后果

- 升级不会让已存的工程和自动化失效；旧版遇到更高的 state abi 会拒载并原样回写字节。
- 代价是冻结之后只能靠迁移或新增来改，契约变更文档写起来慢。这是有意的。
- 改安全默认值、限幅上界或淡变时长算兼容性事件，要写 CHANGELOG，评审按红旗审。

## 被否决的备选

- **只靠 golden 测试守冻结**：子 PR 不编译，漂移会晚发现；所以守卫另读源码里的字面量。
- **1.0 之前不冻结**：参数 ID 一旦被工程保存就不能改，预发布版保存的工程也要能打开。
- **正式版另出一份源码**：与 rc 不同源，等于发出去的东西没被测过。
