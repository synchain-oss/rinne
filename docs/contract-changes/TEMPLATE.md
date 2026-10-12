# 契约变更：<一句话标题>

<!--
用法：复制本文件为 docs/contract-changes/<YYYYMMDD>-<slug>.md（日期为开 PR 当天，slug 只用小写字母、数字与连字符），
与改动放在同一个 PR 里新增。branch-gate 的契约守卫只认本 PR 新增的这种文件名，改名或修改旧文件不算。
确实不影响契约的改动（例如只改注释）不必写本文件，在 PR 正文写一行 contract-neutral: <理由> 即可。
不适用的栏目写「不涉及」，不要删标题。
-->

## Summary (EN)

- **Contract**: <which contract document, which section>
- **Change**: <what changes, in one or two sentences>
- **Compatibility**: <effect on existing projects, recorded automation, mixed old and new versions>

## 改了什么、为什么

- 契约：`docs/contracts/<文档>.md` 的哪一节；涉及的 golden 与源码路径。
- 改动：改前 → 改后。
- 理由：为什么必须改，考虑过哪些不改契约的做法、为什么不行。

## 兼容性

- 旧工程（已保存的 state）：
- 已录的自动化：
- 新旧版本混装（一端新、一端旧；同一台机器上的两个宿主）：

## 版本位

按 SemVer 表应升哪一位，理由。

## 迁移与测试

- 迁移函数或回退策略：
- 新增或修改的测试（golden、单测、loop_rig 场景）：

## CHANGELOG 的 Compatibility 条目

```
- <一行英文，≤200 字符，末尾带 (#PR)>
```

## 批准

首个公开 rc 之后必填：维护者确认这次变更的评论链接。此前写「草案阶段，不需要」。
