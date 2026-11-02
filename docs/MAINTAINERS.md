# 维护者手册

## Summary (EN)

- Rinne has one maintainer and is supported on a best-effort basis. The only external channel is contact@synchain.ca; security reports go through GitHub Private Vulnerability Reporting.
- Response targets: first reply to a security report within 3 business days, to a report of dangerous behaviour in normal use (howling, a runaway loop) as the highest-priority triage, to everything else within 7 days.
- A host report becomes a pull request that adds a record under `docs/validation/` and updates `docs/daw-tiers.json`; tiers move only with a record.
- Handover: building and releasing need no repository secret beyond the built-in token; repository settings can be rebuilt from the read-back commands in `docs/RELEASE.md` appendix B; `CLAUDE.md` holds the standing rules.
- If maintenance stops: say so in the README status line and in a pinned issue, keep the last release and tags, then archive the repository.

## 角色与联系

Rinne 由一位维护者维护，尽力而为。对外联络只有 contact@synchain.ca；安全问题走 GitHub 的 Private Vulnerability Reporting（范围与时限见 [SECURITY](../SECURITY.md)）。外部贡献走 fork 到 `dev` 的 PR，规则见 [CONTRIBUTING](../CONTRIBUTING.md)；维护者一侧的常驻规则在仓根的 `CLAUDE.md`。

## 响应目标

| 来源 | 目标 |
|---|---|
| 安全报告（PVR 或邮件） | 3 个工作日内首次回应；高危 14 天、中危 30 天内修复或缓解 |
| 正常使用下的危险行为（啸叫、环路失控、限制器失效） | 最高优先级分诊，目标 48 小时内首次回应 |
| 其他缺陷、宿主报告、功能建议 | 目标 7 天内首次回应；每周集中分诊一次 |

这些是目标，不是承诺；单人维护，遇到休假或积压会晚。

## 宿主报告怎么落地

1. 报告来自 Host report 议题表单或邮件。能复现的问题另开缺陷报告，宿主报告只记结果。
2. 有人愿意补记录时，请他们照 [`validation/TEMPLATE.md`](validation/TEMPLATE.md) 写一份记录，文件名 `<日期>-<宿主>[-版本]-<win|macos|linux>.md`。
3. 同一个 PR 里更新 [`daw-tiers.json`](daw-tiers.json)，并重跑 `node scripts/gen-docs.mjs --write`。只有存在对应记录，等级才能写 Tier 1 或 Tier 2；`tiers` 检查会核对。
4. 宿主相关的强声明（「在某宿主里……」）要么有记录做证据，要么在 [`claims.json`](claims.json) 里保持待办并带着含糊写法，不要提前改硬。

## 发版

发版流程、tag 规则、资产清单与发布说明模板都在 [RELEASE](RELEASE.md)：tag 只打在 `prod` 上，流水线只建**草稿** Release，由维护者核对后手动发布。`dev`、`prod` 只由维护者本人合并。改到流水线、维护者脚本、hooks、gates、检查脚本与合并判据本身的 PR 不自合，交维护者批准。

## 交接（bus factor）

下一位维护者只需要下面这些：

- **构建与发版不需要仓库密钥。** 构建、打包与发版的 workflow 只用内置的 token；macOS 包不签名、不公证，所以没有证书、没有 Apple 开发者账号要交接（见 [ADR-0005](adr/0005-platform-toolchain.md)）。仓库里配置的密钥只服务于评审机器人与隐私检查，具体名字写在 `.github/workflows/` 里。
- **仓库设置可复现。** [RELEASE](RELEASE.md) 附录 B 给出每项设置的只读回读命令与期望值；改设置是 GitHub 写操作，由新维护者批准后执行，再用同一组命令回读。
- **钉版与上游。** 工具和 JUCE 的版本钉在仓根文件里，见 [BUILDING](BUILDING.md)；从姊妹仓复制的代码登记在 `upstream/`，同步规则见 [RELEASE](RELEASE.md) 附录 C。
- **决策记录。** 为什么这样设计，看 [adr/](adr/README.md) 和 [ARCHITECTURE](ARCHITECTURE.md)；每份顶部都有英文摘要。

## 停止维护

如果维护者不能再维护：

1. 在 README 的状态行和一个置顶议题里写明，给出最后一个可用的版本。
2. 保留最后一个 Release 和全部 tag，不删除任何已发布的资产。
3. 把仓库设为归档（archive）。源码按 GPL-3.0-or-later 授权，欢迎任何人 fork 继续。
