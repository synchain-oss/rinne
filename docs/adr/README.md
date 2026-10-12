# 架构决策记录（ADR）

## Summary (EN)

Architecture decision records for Rinne. Each record is written in Chinese, starts with an English summary and stays
under 4 KB. Records marked "planned" are not written yet.

## 约定

- 一条决定一个文件 `NNNN-<slug>.md`，编号只增不复用。
- 结构：标题、`## Summary (EN)`（不超过 5 行：决定、后果、兼容性影响）、状态、背景、决定、后果、被否决的备选。正文用中文，理由写全，不引用私有文档与内部编号；每份不超过 4 KB。
- 已采纳的 ADR 不改写结论。要推翻时写一条新 ADR，并在旧 ADR 的状态里写明被哪一条取代。
- 分层、线程模型与九条安全不变量的总览在 `docs/ARCHITECTURE.md`；冻结契约在 `docs/contracts/`。

## 索引

| ADR | 决定 | 状态 |
| --- | --- | --- |
| [0001](0001-two-binaries.md) | 两个独立的插件二进制，经命名共享内存会合 | accepted |
| 0002 | 环路时序：固定环路延迟 D、流相对寻址、向宿主报 0 延迟 | planned |
| 0003 | 安全模型：安全状态机、限幅、淡变与锁存 | planned |
| 0004 | 配对：组 A–H、智能默认与对端轨道名核验 | planned |
| [0005](0005-platform-toolchain.md) | 平台、格式与工具链；macOS 不签名不公证 | accepted |
| 0006 | 冻结面与版本策略 | planned |
| [0007](0007-licensing.md) | 许可与合规口径 | accepted |
| [0008](0008-shared-code.md) | 与 SCVB 共享代码：复制并登记来源，不抽共享库 | accepted |
