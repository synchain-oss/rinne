# ADR-0001：两个独立的插件二进制

## Summary (EN)

- **Decision**: Rinne ships as two separate plugins, Rinne Input and Rinne Output, each in its own bundle. They find each other through named shared memory.
- **Consequences**: a frozen IPC contract; one core library and one version number for both; IPC is off in validators.
- **Compatibility**: plugin identities are frozen once the first public build is out.

## 状态

已采纳（2026-10-12）。

## 背景

Cubase 等宿主会拦下一切让信号回送自身的路由。想在通道内做反馈，只能在插入链的末尾取信号、在开头送回去，这是插入链上的两个位置，本来就需要两个插件实例。

JUCE 8.0.8 的 VST3 工厂是固定数组，一个模块只导出一个插件类；两个插件各自是独立加载的模块，彼此的 static 变量看不见，只能经命名共享内存会合。

## 决定

- 两个独立插件：**Rinne Input**（返回端，放插入链第一格，承载全部调节）与 **Rinne Output**（发送端，放推子前区最后一格，只做分组与配对）。
- 身份：厂商码 `Snch`；PLUGIN_CODE `Rnni` / `Rnno`；BUNDLE_ID `com.synchain.rinne.input` / `com.synchain.rinne.output`；AU 为 `aufx Rnni Snch` / `aufx Rnno Snch`；VST3 类别 `Fx Delay`。探针构建用另一套代码（`Rnpi` / `Rnpo`）和另一个共享内存前缀，与正式版互不干扰。
- 会合：每个用户会话一个会合段（组 A–H 的端点槽与标签槽），每组一个数据段（带单调写头的传输环）。会合段名永不带 abi，所以 abi 不同的两个版本仍能互相看见并报告不兼容，而不是都停在「搜索中」。
- 分层：`rinne_core` 不链 JUCE，承载 IPC、环路、DSP、安全状态机与 state；两个插件是只做映射的薄壳，互不 include。
- 两个插件共用一个版本号，同一次发布。

## 后果

- 需要一份冻结的 IPC 契约（段名、头部、布局、内存序），布局改动必须升 abi。
- 用户要插两个插件并让它们配对，所以配对状态、错序与跨轨提示是界面的一等公民（配对规则见 ADR-0004）。
- 在 pluginval、auval 等验证宿主里 IPC 整体关闭、只出干声，验证工具之间互不干扰。
- 身份在首个公开构建发出后永久冻结；要改只能发新插件。

## 被否决的备选

- **单插件双模式**（一个插件用开关选择 Input 或 Output）：两个位置各插一个实例时开关容易配错；两端的界面需求完全不同，合在一起两边都变复杂。
- **一个 bundle 导出两个插件类**：JUCE 8.0.8 的 VST3 工厂不支持。
- **借用宿主的发送或侧链**：宿主拦下的正是这类回送路由，这也是本项目存在的原因。
