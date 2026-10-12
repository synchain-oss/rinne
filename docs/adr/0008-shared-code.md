# ADR-0008：与 SCVB 共享代码：复制并登记来源，不抽共享库

## Summary (EN)

- **Decision**: code reused from synchain-oss/scvb is copied into Rinne and registered in `upstream/<area>.json` (upstream commit and file blob). No shared library or submodule in v1.
- **Consequences**: each copied file has a one-line provenance header; drift is found per blob; fixes are ported by hand.
- **Compatibility**: none for users. Revisit when a third product needs the same modules.

## 状态

已采纳（2026-10-12）。

## 背景

SCVB 已有 Rinne 需要的 IPC 后端、state 容器、WebView 外壳、字体流水线与检查脚本。SCVB 的 IPC 已冻结，mac 后端还在未合并的分支上；消费者只有两个，维护者只有一人。

## 决定

- **复制并按区域登记**：复制当时先 fetch 参考仓，再 rev-parse 取 40 位 sha；把 sha 与每个文件的 `fromBlob`、`mode` 写进这次复制自己的 `upstream/<area>.json`。并行的工作各写各的片段，互不冲突，`check-upstream` 合并后校验。
- **两种 mode**：`adapted`（只改名、改命名空间、改写注释，跟踪上游）与 `forked`（大幅改写，不逐行跟踪）。没有逐字副本：参考仓的注释带内部编号，复制时必须改写。
- **文件头**只写一行 `Derived from synchain-oss/scvb; see upstream/<area>.json`，不写 sha，避免出现第二个真源。逐文件来源表在打包时渲染进 zip 里的 NOTICES。
- **注释清洗**：内部编号逐处人工改写成自含的理由，不做自动替换；隐私规则判红兜底。
- **漂移**：`check-upstream` 离线校验登记；`--drift` 在线按 blob 比对上游，blob 相同就是没有漂移。只在三种情况同步：上游修了安全、崩溃或实时安全问题；来源分支已合进上游主干、需要改指；首个正式版之前核对仍指向未合并分支的来源。一次同步一个 PR。
- **反向同步**：Rinne 发现的上游问题，经维护者同意后到 SCVB 开 issue，不直接提 PR。
- **不复制** SCVB 专有的分析、导出与曲线模块，以及只服务 SCVB 的 web 模块。

## 后果

- 同一个修复可能要在两个仓各做一次，靠漂移比对提醒。
- 每个复制文件都能追溯到上游的确切版本，离线即可校验。
- Rinne 的发布不依赖 SCVB 的发布节奏。

## 被否决的备选

- **现在就抽共享库**（subtree、submodule 或包）：IPC 已冻结，mac 后端是移动靶，两个消费者、单人维护，多一个仓就多一条发布线。改为抽库的触发条件：出现第三个复用同一套模块的产品，或同一个修复一个季度内在两个仓各做三次以上；届时用 subtree。
- **逐字复制模式**：参考仓的注释必须改写，逐字副本实际做不到。
