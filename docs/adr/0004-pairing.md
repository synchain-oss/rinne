# ADR-0004：配对：组 A–H、智能默认与对端轨道名核验

## Summary (EN)

- **Decision**: eight pairing groups (A–H); a pure-function smart default picks a group once per instance; a manual choice is never overridden and a duplicated track never migrates automatically, it offers a one-click repair instead.
- **Consequences**: the default is testable as a table; conflicts are visible and fixable by the user, never silently rewritten.
- **Compatibility**: none for users; group numbers in saved projects always win over the default.

## 状态

已采纳（2026-10-20）。

## 背景

两个插件靠会合段里的组号配对。用户大多不想手选组：新建一对 Input / Output 时应当自己连上。但自动选组一旦迁移已有的组，就可能把一个正在工作的环路拆开，所以规则必须保守、可预测、可测试。

## 决定

- **纯函数**：`PairingPolicy` 只看调用方交来的一份会合段快照，不读段、不读时钟、不分配，表驱动单测。
- **触发**：首次 prepare 之后，累计至少 8 个实时块，距 prepare 至少 300 ms；工程里没有组号；本会话用户没有手选过组。五项同时满足才做一次决定。
- **作用域**：只看同一进程的实例。别的进程里的端点一律当作不存在，不参与匹配，也不当作空位。
- **新 Output**：同名轨上唯一的未配对 Input（两端名字都已知、哈希相等、恰好一个）；否则最近认领的未配对 Input；否则进编号最小的空组等待，每 1 s 重评一次；所在组里已有同进程的活 Input 时留在原组，重评不迁组。
- **新 Input**：先加入有未配对 Output 在等的组（最近认领的优先）；否则进编号最小的空组。并列一律取编号小的，结果与扫描顺序无关。
- **组全满**：不分配，状态显示为未分组。
- **provisional**：智能认领带 provisional 标记，工程 state 的恢复可以抢占它；用户手选之后永不自动迁组。转正（`confirmClaim`）取最早的一个时刻：认领后宿主第一次 getState、认领后第一次 setState 处理完、实例定型；不设计时器。复制轨道时宿主先对原实例 getState，原实例先转正，复制体的恢复抢不动它。
- **复制轨道**造成的冲突不自动迁组。先占着的原实例照常工作，只有后来者显示冲突，并给一键修复：Input「移到空闲组」，Output「重新配对」（按智能默认重选）。目标组里本角色的槽是休眠的，需要用户确认才接管；Output 的休眠回退还要求对面的 Input 端为空或同进程。界面文案按用语纪律，不把这个动作叫作自动的。
- **状态模型**：`PairStateModel` 把会合状态、对端快照与宿主事实折成 15 个状态（红 5、琥珀 3、灰 4、绿 3），判定顺序固定、先到先得；另给组位图、对端名与颜色、名字核验结果（相同 / 不同 / 未知）、占组者名、声道失配和两端版本串。
- **名字核验**：两端哈希都已知且相等才算相同；任一端未知就是「未核验」，不当作不同。

## 后果

- 规则全在一处，表驱动用例逐格覆盖，改规则必须改用例。
- 保守带来的代价：同名但已有人占着的轨道不会被抢，需要用户自己处理。
- 同名匹配只靠名字哈希；两条同名轨会退回「最近认领」，由用户在界面上确认。

## 被否决的备选

- **复制体判定后自动迁组**：会在用户没察觉时改变声音路径，也可能与工程恢复竞争；改为冲突可见加一键修复。
- **按「认领早于 3 秒」判复制体**：依赖加载顺序的时间窗，不同宿主表现不一；不用时间判据。
