# REAPER 本机宿主烟测

用途：在上 Cubase 之前，先在本机用 REAPER 把「两个插件放进同一条插入链、渲染、存盘重开」这一类能自动化的事跑一遍，早点挡掉明显的退化。它是预检与本机回归，**不替代 Cubase / LUNA**。

- REAPER 的数据**不计入** Cubase 的结论：这里的绿不能当作 Cubase 的绿，日志也与 Cubase 的分开存放。
- REAPER 在 DAW 分级里仍是 Tier 3（见 `docs/daw-tiers.json`），本工具不改分级。
- REAPER 的评估许可只在本机使用；评估期过后按 EULA 须购买许可，本工具不会自动点掉许可提示窗。本工具不进 CI，不挂 gates，不进 ctest。要上 CI 须先向 Cockos 确认许可条款。
- 只测本机构建的 `dev` 风味（段名前缀 D）插件的副本，不测装机版。

## 测什么（第一段）

当前两个插件是逐位直通的壳，所以第一段只验证「宿主怎么放、怎么渲染都不改一个样本」。场景表是 `tests/reaper/scenarios.json`，每一项的判据都是：渲染结果与同参数的无插件渲染用 `rinne_nulltest compare` 对拍，逐位相同、lag 为 0、前 2 s 的残差为 −inf。

| 场景 | 内容 |
|---|---|
| `null-<模式>-<采样率>-<块长>` | Input 在首、Output 在末；离线渲染与 1x 在线渲染；44.1 k / 48 k；块长 64 / 512 / 1024 |
| `device-<采样率>-<块长>` | 读回设备块长与采样率与设定一致；两个插件的 `pdc` 与 `chain_pdc_actual` 都是 0 |
| `mono-offline` | 单声道信号与单声道渲染 |
| `bypass-input` / `bypass-output` / `bypass-both` | 插件旁路 |
| `save-reopen` | 存盘后重开，旁路状态与延迟上报还在，重开后的工程再渲染仍逐位相同 |
| `instances-25` | 一条轨道上反复加减两个插件 25 次，REAPER 不崩，句柄与私有内存的增长有上界 |

反例（`negative` 一栏）必须按预期变红，工具才算可信：

| 反例 | 期望 |
|---|---|
| `neg-offset-1-sample`：渲染结果偏 1 个样本 | FAIL |
| `neg-track-minus-6db`：轨道推子 −6 dB | FAIL |
| `neg-timeout` / `neg-killed`：驱动卡住，超时或到点被结束 | INCOMPLETE，原因分别是超时、到点被结束，进程已结束，gui 锁已释放（gui 锁等不到、卡对话框、REAPER 自己退出都不算验证过） |
| `neg-user-testing`：放上 `USER_TESTING` 标记 | SKIPPED，一次 REAPER 都不启动 |
| `neg-sentinel-missing`：配置目录缺哨兵文件 | 拒绝启动 |
| `neg-default-dir`：配置目录指向 REAPER 的默认资源目录 | 拒绝启动 |

## 结局词汇

每个场景一个结局：`PASS`、`FAIL`、`SKIPPED`、`INCOMPLETE`。**INCOMPLETE 不当 PASS**：超时、被结束、卡在对话框、没产出文件、等不到 gui 锁，都是「没有得出结论」。反例的 `status` 要同时满足两条才是 PASS：观察到的结局等于期望，并且原因对得上（说明里匹配 `scenarios.json` 里这条反例的 `expectDetail`），原始结局写在 `observed`。只比结局会被「没启动、没执行」空过：gui 锁等不到时 REAPER 根本没有启动，看门狗反例照样是 INCOMPLETE，却没验证到看门狗。所以结局对、原因不对，以及反例没能执行（观察到 INCOMPLETE 而期望不是它），这一行都是 INCOMPLETE（反例没能验证），不是 PASS 也不是 FAIL。每条反例必须写 `expectDetail`，缺了或不是合法正则，自测判红。整套的结局取最差的一个。退出码：0 全 PASS、1 有 FAIL、2 有 INCOMPLETE、3 整套 SKIPPED、64 被护栏拒绝或用法错误。

## 怎么跑

REAPER 没有无头模式，需要一个真实桌面，并且占用 gui 锁（同一时刻最多一个 GUI 任务）。先用本机 gates 构建出 `dev` 风味的两个插件（构建目录里要有 `rinne-build.json` 与 `rinne_nulltest`），再：

```
pwsh scripts/reaper/doctor.ps1 -CfgDir <私有配置目录> -Init
pwsh scripts/reaper/doctor.ps1 -CfgDir <私有配置目录> -BuildDir build-wf -UserTestingMarker <标记文件路径>
pwsh scripts/reaper/run.ps1 -CfgDir <私有配置目录> -BuildDir build-wf -LogDir <结果目录> -UserTestingMarker <标记文件路径> -Repeat 5
pwsh scripts/reaper/run.ps1 -SelfTest
```

- `-CfgDir` 必须是显式的绝对路径，放在 REAPER 的默认资源目录**之外**。`doctor.ps1 -Init` 建目录、私有插件目录、最小的 `reaper.ini`（Dummy 音频设备）和哨兵文件。首次用空目录启动 REAPER 会扫描系统 VST3 目录，可能很慢；可以把已有的插件缓存复制进私有目录来避免。私有目录里的插件缓存含本机装了哪些插件，**永远不进仓、不进日志**。
- `-UserTestingMarker`：维护者上机测试期间放这个标记，存在时整套 SKIPPED。路径由调用方（本机包装脚本）传入。
- `-Only <id 正则>` 只跑部分场景，`-NoNegative` 不跑反例，`-Repeat N` 连跑 N 轮。
- 结果写 `<结果目录>/<时间>-<构建 sha7>.json`：每轮的耗时、结局、每个场景的结论与说明、插件文件的 sha256 前 12 位、REAPER 启动次数、默认资源目录前后核对的结果。结果里的路径一律脱敏。
- `-SelfTest` 不启动 REAPER 的主体部分在假的 APPDATA 里做；其中少量用例会用替身进程弹出几个很快消失的对话框，来验证模态框处理，期间占 gui 锁。

## 安全规则

REAPER 对不存在的 `-cfgfile` 路径不报错，而是悄悄改用默认资源目录并写它，这会改掉使用者自己的配置。所以：

1. 永不碰默认资源目录。`run.ps1` 只接受显式的 `-CfgDir`；启动前断言：路径是盘符开头的绝对路径（`\\?\` 前缀、`\\.\` 设备路径、UNC 与指回本机的 `\\localhost\C$\` 管理共享写法一律不收）；展开 `..`、解析 junction 与符号链接之后，字符串上不在 AppData 之下；再按文件身份（卷序列号加文件 ID，取自打开的目录句柄）比一遍：配置目录自己和它的每一级上级（一直查到盘符根），只要有一个与 AppData 是同一个目录就拒绝，这样 8.3 短名、大小写、尾部分隔符、subst 盘符这些字符串认不出的写法也过不去；存在却取不到文件身份（拒绝访问等）同样拒绝，证明不了就不放行；目录存在；有 `doctor.ps1 -Init` 写的哨兵文件；`reaper.ini` 存在。任何一条不满足就拒绝启动。`-cfgfile` 的实参只由一个只读常量产生，每次启动前再校验一遍。
2. 变量名不得只靠大小写区分（PowerShell 变量名不分大小写，`$c` 会盖掉 `$C`，当初就是这样让配置路径变空的）。自测会扫描 `scripts/reaper` 与 `tests/reaper` 下所有脚本：大小写碰撞、缺 `Set-StrictMode -Version Latest`、配置参数的字面量不是恰好一处且在 `Host.psm1` 里、除 `Guard.psm1` 与自测外提到 AppData，都判红。`.ps1`、`.psm1`、`.lua`、`.json` 里还不许夹不可见的控制字符（不在行尾的 CR 和 TAB、LF 以外的 C0 控制字符）：它们在编辑器和 diff 里看不见，一旦被吞掉，正则或字符串的意思就变了。
3. 看门狗只按自己启动的 PID 结束进程；对话框只处理属于这个 PID 的窗口，用户自己的 REAPER 即使开着也不会被碰。本机已有别的 `reaper.exe` 在运行时整套 SKIPPED。
4. 音频一律走 Dummy 设备，不播放到真实声卡；启动参数固定带 `-newinst -nosplash`。
5. 模态框：「REAPER Query」（存盘提示）点「否」；渲染、插件扫描、峰值文件的进度窗放行；其余可见对话框（未选音频设备、Project Load Warning、REAPER Error、录音结束保存框、Lua 报错、评估许可的 About REAPER 提示窗等）持续半秒以上就判卡死，关窗并结束进程，场景为 INCOMPLETE。许可提示窗不会被自动点掉：评估期满后应购买许可证。
6. 每轮前后各取一次默认资源目录的清单（文件的大小与修改时间），变了就判红并停止后续轮次。
7. 只复制本机 `dev` 构建的两个 VST3 进私有目录；构建清单不是 `dev` 风味就 SKIPPED。预检还会校验 `rinne-build.json`：它得是合法的 JSON 对象，`sha` 得是 7 到 40 位的十六进制（它会拼进结果文件名，也证明测的是哪次构建），否则整套 SKIPPED 并写明原因。工程模板里插件的类 ID 是写死的，预检读两个插件包里的 `Contents/Resources/moduleinfo.json`，与模板比对；对不上（插件身份变了，链上会是「效果不可用」的空槽，渲染照样逐位相同）就整套 SKIPPED，先去更新 `tests/reaper/Rpp.psm1`。
8. `-Only` 写错、一个场景都没选中（或只选中了反例又给了 `-NoNegative`）是用法错误：退出码 64，不启动 REAPER，也不写结果文件；零个场景的一轮不会落成 PASS。
9. 自测里用到 subst 盘符（验证文件身份层认得出指向 AppData 的盘符）：映射对整个登录会话可见，自测被硬结束时清理不会跑。所以自测开头先清上一次留下的残留，只清同时满足两条的：盘符在 Z 到 U 之间；目标是自测临时目录（固定前缀加 32 位十六进制）。别的映射一个不碰，清了哪个字母、指向哪里只写在本机的自测输出里。

## 第二段（探针接好后再做）

回声间隔（`rinne_nulltest echo`）、经 `TrackFX_SetParam` 设探针的隐藏参数、录一段输出再交给探针分析器，都只需要往 `scenarios.json` 加条目，再在 `Scenarios.psm1` 的登记表里为新的 `kind` 加一项处理函数。条目的 `env` 字段会按原样设进 REAPER 进程的环境变量（`{work}` 换成本轮的工作目录）。

探针不读环境变量（发布版与探针构建都一样），日志一律写在默认的「文档目录下的 `Rinne Probe Logs`」，文件名是 `probe-<role>-<pid>-<tok4>.csv`。runner 启动 REAPER 时不设任何日志目录变量，每轮结束后按文件名里的 REAPER 进程号收取本轮自己的日志：只读、只拷贝，不删也不动别的进程留下的日志。

## 常见现象

- 第一次或插件更新后启动较慢：REAPER 在重新扫描被换掉的 VST3，进度窗会被放行，超时是 90 s。
- 结果 INCOMPLETE 且说明里有「卡在对话框」：说明里列出了见到的窗口（标题里的路径已打码），按名字处理或加进 `Host.psm1` 的已知清单。
