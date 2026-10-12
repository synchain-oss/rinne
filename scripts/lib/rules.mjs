// SPDX-License-Identifier: GPL-3.0-or-later
//
// 仓内规则数据的单一真源：冻结契约面、第三方模块登记、镜像文档、顶层条目白名单、体量预算。
// 这里只放数据，判据在用它的检查里：契约面归 check-commits 的 contract-guard，第三方登记归
// checks.d/modules-closure，其余由 docs 组的检查读取。改这里就是改判据，同一个 PR 里要让
// 对应检查的 --self-test 照样全绿。

/** 三份冻结契约文档。契约守卫读它们在 base 上的 `status:` 行（draft / frozen）决定是否启用。 */
export const CONTRACTS = {
    ipc: "docs/contracts/IPC_CONTRACT.md",
    params: "docs/contracts/PARAMETERS.md",
    state: "docs/contracts/STATE_SCHEMA.md",
};

// 冻结契约面：[路径, 所属契约（* = 任一契约冻结即受守）, 只看哪些改动行]。按行判的路径只看 -U0 的改动行：
// CMake 里经变量间接给身份（set(X Rnni) 再 PLUGIN_CODE ${X}）、或值写在下一行的情形这里抓不到，
// 由身份 golden 的静态检查兜底。
export const CONTRACT_SOURCES = [
    [/^docs\/contracts\/IPC_CONTRACT\.md$|^src\/core\/ipc\/[^/]*Layout[^/]*\.h$/, "ipc"],
    [/^tests\/golden\/ipc-layout\.json$/, "ipc"],
    [/^docs\/contracts\/STATE_SCHEMA\.md$|^src\/core\/state\//, "state"],
    [/^tests\/golden\/state(?:-schema\.json$|\/)/, "state"],
    [/^docs\/contracts\/PARAMETERS\.md$|^tests\/golden\/(?:params-input|identity)\.json$/, "params"],
    [/^src\/input\/ParamTable\.h$/, "params", /\bRINNE_PARAM\b/],
    [/(?:^|\/)CMakeLists\.txt$|\.cmake$/, "params", /\b(?:PLUGIN_(?:MANUFACTURER_)?CODE|BUNDLE_ID|AU_[A-Z_]+)\b/],
    [/^docs\/contracts\/|^tests\/golden\//, "*"],
];

/**
 * CMake 引入的每个第三方来源，以及它带进二进制或构建的组件。键的写法：
 *   juce::<模块>  target_link_libraries 里的 JUCE 目标；juce_add_plugin 隐含 juce::juce_audio_plugin_client
 *   nuget:<包名>  NuGet 包（NEEDS_WEBVIEW2 隐含 WebView2 的包）
 *   fetch:<名字>  FetchContent_Declare 的名字
 * table 是组件在 THIRD-PARTY-NOTICES.md 里所在的表：shipped = 随二进制分发，build = 只用于构建、测试与 CI。
 * components 与表格第一列逐字一致；只是编译选项、不带第三方代码的目标写空数组。
 * 这里预登记了 v1 计划用到的全部模块。新增 JUCE 模块或依赖时，同一个 PR 里改这里、NOTICES、
 * LICENSES/、third_party/notices/ 与 REUSE.toml；modules-closure 与 licenses-closure 检查它们是否闭合。
 */
export const THIRD_PARTY_BY_MODULE = {
    "juce::juce_core": { table: "shipped", components: ["JUCE", "zlib"] },
    "juce::juce_events": { table: "shipped", components: ["JUCE"] },
    "juce::juce_data_structures": { table: "shipped", components: ["JUCE"] },
    "juce::juce_audio_basics": { table: "shipped", components: ["JUCE"] },
    "juce::juce_audio_processors": { table: "shipped", components: ["JUCE", "VST3 SDK"] },
    "juce::juce_audio_plugin_client": { table: "shipped", components: ["JUCE", "VST3 SDK", "AudioUnitSDK"] },
    "juce::juce_graphics": {
        table: "shipped",
        components: ["JUCE", "libpng", "libjpeg", "HarfBuzz", "SheenBidi", "Unicode data tables"],
    },
    "juce::juce_gui_basics": { table: "shipped", components: ["JUCE"] },
    "juce::juce_gui_extra": { table: "shipped", components: ["JUCE", "JUCE JavaScript helper"] },
    "juce::juce_recommended_config_flags": { table: "shipped", components: [] },
    "juce::juce_recommended_lto_flags": { table: "shipped", components: [] },
    "juce::juce_recommended_warning_flags": { table: "shipped", components: [] },
    "nuget:Microsoft.Web.WebView2": { table: "shipped", components: ["WebView2 loader"] },
    "fetch:Catch2": { table: "build", components: ["Catch2"] },
};

/** 中英镜像文档：英文为规范版，zh-CN 是完整镜像（硬约束以中文为语义权威）。 */
export const MIRROR_PAIRS = [
    { en: "README.md", zh: "README.zh-CN.md" },
    { en: "docs/USER_GUIDE.md", zh: "docs/USER_GUIDE.zh-CN.md" },
];

/** 中文真源节与它渲染出的生成块（gen-docs 的块名:语言）。镜像对拍时两边按这张表对应。 */
export const SOURCE_BLOCKS = [
    { source: "docs/USER_GUIDE.zh-CN.md#使用前必读", blocks: ["safety-warning:en", "hard-rules:en"] },
];

/** 单文件双语的文档：每个二级标题下先英文后中文。 */
export const BILINGUAL_FILES = ["docs/INSTALL.md", "docs/KNOWN_ISSUES.md"];

/**
 * 仓根允许出现的条目（git 跟踪的第一级路径）。白名单不留通配：新增顶层文件或目录时在这里登记，
 * 否则 structure 检查判红，例如仓根的截图、草稿与临时导出。
 */
export const TOP_LEVEL = {
    files: [
        ".actionlint-version",
        ".clang-format",
        ".clang-format-version",
        ".editorconfig",
        ".gitattributes",
        ".gitignore",
        ".gitleaks-version",
        ".gitleaks.toml",
        ".juce-commit",
        ".juce-version",
        ".markdown-link-check.json",
        ".node-version",
        ".pluginval-version",
        ".prettier-version",
        ".prettierignore",
        ".prettierrc.json",
        ".sccache-sha256",
        ".sccache-version",
        "CHANGELOG.md",
        "CLAUDE.md",
        "CMakeLists.txt",
        "CONTRIBUTING.md",
        "LICENSE",
        "README.md",
        "README.zh-CN.md",
        "REUSE.toml",
        "SECURITY.md",
        "THIRD-PARTY-NOTICES.md",
        "requirements-tools.txt",
    ],
    dirs: [
        ".github",
        "LICENSES",
        "cmake",
        "docs",
        "packaging",
        "presets",
        "scripts",
        "src",
        "tests",
        "third_party",
        "tools",
        "upstream",
        "web",
        "web-preview",
    ],
};

const KiB = 1024;
/**
 * 体量预算，超出只报 WARN（由 docs 组的 budget 检查执行）。字节按 UTF-8 计。
 * 脚本行数只数本仓新写的部分：登记在 upstream/*.json 里的复制脚本不计。
 */
export const BUDGETS = {
    files: [
        { path: "CLAUDE.md", bytes: 16 * KiB },
        { path: "README.md", bytes: 14 * KiB },
        { path: "README.zh-CN.md", bytes: 14 * KiB },
        { path: "docs/USER_GUIDE.md", bytes: 30 * KiB },
        { path: "docs/USER_GUIDE.zh-CN.md", bytes: 30 * KiB },
        { path: "docs/RELEASE.md", bytes: 12 * KiB },
    ],
    patterns: [{ glob: "docs/adr/[0-9][0-9][0-9][0-9]-*.md", bytes: 4 * KiB }],
    changelogSectionBytes: 4 * KiB,
    publicMarkdownBytes: 200 * KiB,
    scriptLines: { total: 2500, perFile: 400, growthPerMinor: 0.15 },
};
