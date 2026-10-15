# <宿主与平台> 实测记录：<日期>

<!--
用法：复制本文件为 docs/validation/<YYYY-MM-DD>-<host>-<os>.md，例如 2026-10-20-cubase15-win.md。
日期是上机当天，host 与 os 只用小写字母、数字与连字符。原始 CSV 含轨道名，留在私有位置，不入库；
本文只贴 analyze-probe.py 的输出（已脱敏）和人工记录。不适用的栏目写「不涉及」，不要删栏目。
-->

## Summary (EN)

- **Host / OS**: <host and version, OS and architecture>
- **Build**: <probe build commit, first 8 characters>
- **Verdict**: <A1-A9 results in one line; whether freeze conditions F1-F4 are met>

## 环境

| 项 | 记录 |
|---|---|
| 宿主与版本 | |
| 系统与架构 | |
| 采样率 | |
| 探针构建 | 提交前 8 位： |
| 上机日期与时长 | |
| 分析命令 | `python scripts/probe/analyze-probe.py <日志目录>` |

## A1-A9 结论

把分析器输出的结论表整段粘贴在下面，再逐项补人工说明。结论只有「通过」「不通过」「数据不全」三种。

| 项 | 内容 | 结论 | 依据与说明 |
|---|---|---|---|
| A1 | 稳态对齐（不通过即停工上报） | | |
| A2 | 切换切片 | | |
| A3 | 导出一致 | | |
| A4 | 块长 | | |
| A5 | 进程与后端 | | |
| A6 | 轨道信息 | | |
| A7 | hostNs | | |
| A8 | 稳定性 | | |
| A9 | 工程恢复 | | |

## 冻结条件 F1-F4

| 条件 | 内容 | 满足 | 说明 |
|---|---|---|---|
| F1 | 必需的格（CB-01、CB-02、CB-04、CB-05、LU-02、LU-03、LU-04）数据齐全无缺口，A1-A8 都有结论 | | |
| F2 | 没有发现需要改布局的情形（保留区能吸收的小改动不算） | | |
| F3 | Win 与 mac 的 IPC 测试、x64 与 arm64 的环压测（含变异必须变红）、TSan 档全绿 | | |
| F4 | 本记录与 IPC 布局文件已经维护者过目 | | |

## 实测数据

- 各模式最大块长：
- 宿主窗框尺寸：
- Render in Place 与 Freeze 是否克隆实例：
- 其余只记录不判定的项（Ping 延迟一致性、setState 时序、删除实例的去向、多工程时的 release 行为）：

## 未解决与后续

- 
