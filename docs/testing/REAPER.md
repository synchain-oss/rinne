# REAPER 本机宿主烟测

用途：在上 Cubase 之前，先在本机用 REAPER 把「两个插件放进同一条插入链、渲染、存盘重开」这一类能自动化的事跑一遍，早点挡掉明显的退化。它是预检与本机回归，**不替代 Cubase / LUNA**。

- REAPER 的数据**不计入** Cubase 的结论：这里的绿不能当作 Cubase 的绿，日志也与 Cubase 的分开存放。
- REAPER 在 DAW 分级里仍是 Tier 3（见 `docs/daw-tiers.json`），本工具不改分级。
- REAPER 的评估许可只在本机使用；评估期过后按 EULA 须购买许可，本工具不会自动点掉许可提示窗。本工具不进 CI，不挂 gates，不进 ctest。要上 CI 须先向 Cockos 确认许可条款。
- 第一段只测本机构建的 `dev` 风味（段名前缀 D），第二段只测本机构建的探针风味（`build.ps1 -Probe`，段名前缀 P）。都是插件的副本，不测装机版；两种产物不能混用，预检按这一段要的风味核对构建清单与类 ID。

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

- `-CfgDir` 必须是显式的绝对路径，放在 REAPER 的默认资源目录**之外**。`doctor.ps1 -Init` 建目录、私有插件目录、最小的 `reaper.ini`（Dummy 音频设备）和哨兵文件。`-Init` 只接受还不存在的目录、空目录，或以前 `-Init` 过的目录（有哨兵，已有的 `reaper.ini` 不动）；已有内容又没有哨兵的目录一律拒绝，什么都不写，也没有绕过的开关：那多半是一份真实的 REAPER 配置（例如便携版的目录），冒烟会改它的 `reaper.ini`、往它的插件目录里复制插件。首次用空目录启动 REAPER 会扫描系统 VST3 目录，可能很慢；可以把已有的插件缓存复制进私有目录来避免。私有目录里的插件缓存含本机装了哪些插件，**永远不进仓、不进日志**。
- `-UserTestingMarker`：维护者上机测试期间放这个标记，存在时整套 SKIPPED。路径由调用方（本机包装脚本）传入。
- `-Only <id 正则>` 只跑部分场景，`-NoNegative` 不跑反例，`-Repeat N` 连跑 N 轮。
- 结果写 `<结果目录>/<时间>-<构建 sha7>.json`：每轮的耗时、结局、每个场景的结论与说明、插件文件的 sha256 前 12 位、REAPER 启动次数、默认资源目录前后核对的结果。结果里的路径一律脱敏。
- `-SelfTest` 不启动 REAPER 的主体部分在假的 APPDATA 里做；其中少量用例会用替身进程弹出几个很快消失的对话框，来验证模态框处理，期间占 gui 锁。

## 安全规则

REAPER 对不存在的 `-cfgfile` 路径不报错，而是悄悄改用默认资源目录并写它，这会改掉使用者自己的配置。所以：

1. 永不碰默认资源目录。`run.ps1` 只接受显式的 `-CfgDir`；启动前断言：路径是盘符开头的绝对路径（`\\?\` 前缀、`\\.\` 设备路径、UNC 与指回本机的 `\\localhost\C$\` 管理共享写法一律不收）；展开 `..`、解析 junction 与符号链接之后，字符串上不在 AppData 之下；再按文件身份（卷序列号加文件 ID，取自打开的目录句柄）比一遍：配置目录自己和它的每一级上级（一直查到盘符根），只要有一个与 AppData 是同一个目录就拒绝，这样 8.3 短名、大小写、尾部分隔符、subst 盘符这些字符串认不出的写法也过不去；存在却取不到文件身份（拒绝访问等）同样拒绝，证明不了就不放行；目录存在；有 `doctor.ps1 -Init` 写的哨兵文件；`reaper.ini` 存在。任何一条不满足就拒绝启动。`-cfgfile` 的实参只由一个只读常量产生，每次启动前再校验一遍。
2. 变量名不得只靠大小写区分（PowerShell 变量名不分大小写，`$c` 会盖掉 `$C`，当初就是这样让配置路径变空的）。自测会扫描 `scripts/reaper` 与 `tests/reaper` 下所有脚本：大小写碰撞、缺 `Set-StrictMode -Version Latest`、配置参数的字面量不是恰好一处且在 `Host.psm1` 里、除 `Guard.psm1` 与自测外提到 AppData，都判红。`.ps1`、`.psm1`、`.lua`、`.json` 里还不许夹这些不可见的控制字符：U+0000 至 U+0008、U+000B、U+000C、U+000E 至 U+001F、DEL（U+007F），以及不紧挨在 LF 前的 CR（TAB 与行尾的 CRLF 允许）：它们在编辑器和 diff 里看不见，一旦被吞掉，正则或字符串的意思就变了。
3. 看门狗只按自己启动的 PID 结束进程；对话框只处理属于这个 PID 的窗口，用户自己的 REAPER 即使开着也不会被碰。本机已有别的 `reaper.exe` 在运行时整套 SKIPPED。
4. 音频一律走 Dummy 设备，不播放到真实声卡；启动参数固定带 `-newinst -nosplash`。
5. 模态框：「REAPER Query」（存盘提示）点「否」；渲染、插件扫描、峰值文件的进度窗放行；其余可见对话框（未选音频设备、Project Load Warning、REAPER Error、录音结束保存框、Lua 报错、评估许可的 About REAPER 提示窗等）持续半秒以上就判卡死，关窗并结束进程，场景为 INCOMPLETE。许可提示窗不会被自动点掉：评估期满后应购买许可证。
6. 每轮前后各取一次默认资源目录的清单（文件的大小与修改时间），变了就判红并停止后续轮次。
7. 只复制本机构建、且风味与这一段相符（第一段 `dev`，第二段探针）的两个 VST3 进私有目录；构建清单的风味不对就 SKIPPED。预检还会校验 `rinne-build.json`：它得是合法的 JSON 对象，`sha` 得是 7 到 40 位的十六进制（它会拼进结果文件名，也证明测的是哪次构建），否则整套 SKIPPED 并写明原因。工程模板里插件的类 ID 是写死的，预检读两个插件包里的 `Contents/Resources/moduleinfo.json`，与模板比对；对不上（插件身份变了，链上会是「效果不可用」的空槽，渲染照样逐位相同）就整套 SKIPPED，先去更新 `tests/reaper/Rpp.psm1`。
8. `-Only` 写错、一个场景都没选中（或只选中了反例又给了 `-NoNegative`）是用法错误：退出码 64，不启动 REAPER，也不写结果文件；零个场景的一轮不会落成 PASS。
9. 自测里用到 subst 盘符（验证文件身份层认得出指向 AppData 的盘符）：映射对整个登录会话可见，自测被硬结束时清理不会跑。所以自测开头先清上一次留下的残留，只清同时满足两条的：盘符在 Z 到 U 之间；目标是自测临时目录（固定前缀加 32 位十六进制）。别的映射一个不碰，清了哪个字母、指向哪里只写在本机的自测输出里。

## 第二段：探针构建进真宿主

第二段让「探针构建 → REAPER → 探针日志 → `scripts/probe/analyze-probe.py`」整条链真跑一遍。先用 `scripts/build.ps1 -Probe` 构建探针风味（两个探针插件的类 ID 与正式版不同），再：

```
pwsh scripts/reaper/doctor.ps1 -CfgDir <私有配置目录> -BuildDir build-probe -UserTestingMarker <标记文件路径> -Probe [-Python <解释器>]
pwsh scripts/reaper/run.ps1 -CfgDir <私有配置目录> -BuildDir build-probe -LogDir <结果目录> -UserTestingMarker <标记文件路径> -Probe [-Python <解释器>]
```

- 场景在 `scenarios.json` 的 `probe` 一节，第一段的种类不能写进去，探针的种类也不能写在第一段。工程是一条轨道 `[Probe In][Probe Out]`（In 在首、Out 在末），音源是 `rinne_nulltest make-signal` 生成的测试信号，走 Dummy 设备。两端 state 里写同一个组，feedback 保持探针默认的 0，D 是默认的 100 ms。预检另查 Python 3 解释器（没给 `-Python` 就找 `py -3`）、`analyze-probe.py` 在、文档目录取得到且列得出清单。
- 记录窗口只靠三个隐藏参数开（`auto_case`、`auto_variant`、`auto_record_s`），探针的窗口按钮 Lua 碰不到。Lua 按参数名找到它们（不假设下标），按探针规则表的首现顺序换算成整数写入，每次写完核对显示值；先写 case 与 variant，最后两端一起写记录秒数。探针自动开的窗口里 `buf` 写 `-`，`guard` 写 `na`。
- 稳态格（`probe-play`，case `CB-01`、variant `steady`）：实时播放，停走带（探针在停走带时自动 Ping 一次），等记录窗口到时，再正常退出 REAPER。不能结束进程，否则日志不收尾。导出格（`probe-export`，case `CB-04`）：同一段范围离线渲染一次（`export-offline`）、1x 渲染一次（`export-rt`），两份结果用 `rinne_nulltest compare` 对拍，写成分析器认的 `compare-*.csv` 一行。
- 每格从日志里读出并判定：In、Out 各一份，各自恰好一个测试格且采样率对；正常收尾；块数够；没丢记录（丢弃计数与块序号缺口都是 0）；块长不超过声明的最大块长，播放中恰为设定的块长；两端都配对到本进程；D 恰为 100 ms 的样本数；稳态格的 Ping 求出的延迟恰为 D（按样本）。导出格的离线那一次要有 `nonrt` 为 1 的块。
- 然后对这一格的日志目录跑 `analyze-probe.py <目录> --json --redact <用户名> --redact <主机名>`，原始输出原样存下。分析器的判据按 Cubase 的清单设计，REAPER 下多数项是数据不全，这正常：每格只把 `analyzerMustPass` 里的项计入结局（稳态格 A1、A4、A8，导出格 A3、A8），其中有 FAIL 就 FAIL，没到 PASS 就 INCOMPLETE；别的项照录、打星号、不计入。A2、A5、A6、A9 在这一段没有对应的操作（反复切换、三种格式组合、改轨道名、存盘重开），A7（hostNs）取决于宿主怎么给 VST3 的系统时间，REAPER 上的结论都不外推到 Cubase。退出码 2 是分析器拒收日志，说明日志与分析器的契约对不上：这一格 INCOMPLETE，整套停下，不改探针或分析器去凑。整轮合在一起再跑一次分析器，只作记录。
- 反例 `neg-probe-unpaired`：两端不设组，必须判 FAIL，且原因里有「没有配对」。

收日志。探针不读环境变量（发布版与探针构建都一样），日志一律写在文档目录（`[Environment]::GetFolderPath('MyDocuments')`，可能在 OneDrive 下）下的 `Rinne Probe Logs`，文件名 `probe-<I|O>-<pid>-<tok4>.csv`。维护者接着要在 Cubase 里往同一个目录写上机数据，REAPER 的日志不能留在那里混进去，所以每个 REAPER 进程退出后立即把它自己的日志搬走：

1. 只认同时满足这几条的条目：直接在这个目录里（不进子目录）；是普通文件（目录、链接一律不碰）；名字恰好是上面的格式；pid 是这次启动、并且已经退出的 REAPER；修改时间不早于这次启动。
2. 复制到 `<结果目录>/<时刻>-probe-run<k>/probe-logs/<场景>/`，核对 sha256 相同，删之前再看一眼大小与修改时间没变，然后只删那一个原文件（单个文件，不递归，绝不删目录）。哈希不同、进程还在、结果目录里已有同名文件、复制之后原文件变了，都不删并判红。
3. 每轮前后各列一次该目录的清单（名字、大小、修改时间），除本轮自己的文件外必须完全相同；变了一轮判 FAIL，并停止后续轮次。列不出清单也不跑：看不见不能当作没变。

第二段的结果写 `<结果目录>/<时间>-<构建 sha7>-probe.json`；本轮目录里另有每格分析器的原始输出 `analyze-<场景>.json`、整轮那一次的 `analyze.json` 与文档目录前后的清单。

以后再加场景（例如回声间隔），只需要往 `scenarios.json` 加条目，再在 `Scenarios.psm1` 的登记表里为新的 `kind` 加一项处理函数。条目的 `env` 字段会按原样设进 REAPER 进程的环境变量（`{work}` 换成本轮的工作目录）；探针本身不读它们。

## 常见现象

- 第一次或插件更新后启动较慢：REAPER 在重新扫描被换掉的 VST3，进度窗会被放行，超时是 90 s。
- 结果 INCOMPLETE 且说明里有「卡在对话框」：说明里列出了见到的窗口（标题里的路径已打码），按名字处理或加进 `Host.psm1` 的已知清单。
