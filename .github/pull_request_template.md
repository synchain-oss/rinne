<!--
PR 标题与 commit 同格式：type(scope): 中文描述，不写卡号或任何内部编号。
不适用的栏目写「不涉及」，不要删掉标题。正文里不写 AI 署名（Generated with … 一类）。
最后一行必须是与 commit 作者一致的 Signed-off-by（branch-gate 会核对），把占位的姓名与邮箱换成你自己的。
-->

## 做了什么


## 来源 SHA
<!-- 复制自参考仓的代码：upstream/<area>.json 里登记的 sha 与范围；没有复制就写「不涉及」 -->


## 冻结契约自查
- [ ] 参数 ID / 顺序 / 范围 / 默认值
- [ ] 共享段名 / 布局（abi）
- [ ] state schema
- [ ] 桥契约
- [ ] D 合约

<!-- 改了已冻结项：附 docs/contract-changes/<YYYYMMDD>-<slug>.md；确实不影响：单独写一行「contract-neutral: 理由」 -->

## 实时 / 安全自查
- [ ] processBlock 无分配 / 锁 / I/O / 异常
- [ ] 欠载输出零、NaN / Inf 清洗
- [ ] 默认安全未被削弱

## 验证
本地 gates：<档> @<sha7>：PASS n / FAIL 0 / SKIP n / FLAKY n（EXIT=0）
<!-- 另列新增或修改的测试；需要分支 dispatch 时附 run 链接 -->


## 宿主实测（可选）
| 宿主 | 实时 | 离线导出 | 备注 |
|---|---|---|---|
| 免 DAW 测试台 |  |  |  |
| Cubase 15 |  |  |  |
| LUNA |  |  |  |

## 三语截图（UI 改动）
<!-- 与设计稿同名场景并排：zh / en / fr -->


## DCO
- [ ] 所有 commit 均用 `git commit -s` 签名，且无 AI 署名

Signed-off-by: 姓名 <邮箱>
