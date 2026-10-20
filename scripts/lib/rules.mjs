// SPDX-License-Identifier: GPL-3.0-or-later
//
// 仓内规则数据：冻结契约面、第三方模块登记、镜像文档、顶层条目白名单、体量预算。
// 这里只放数据，判据在用它的检查里：第三方登记归 checks.d/modules-closure，其余由 docs 组的检查读取。
// 改这里就是改判据，同一个 PR 里要让对应检查的 --self-test 照样全绿。

// 冻结契约文档与冻结契约面的真源留在 check-commits.mjs：它是基础设施路径，改动要维护者批准，
// 契约守卫的判据因此不会被普通 PR 顺手放宽。这里只转出，供需要读取的检查使用。
export { CONTRACTS, GUARDED as CONTRACT_SOURCES } from "../check-commits.mjs";

/**
 * JUCE 各模块直接依赖的模块，逐字取自该版本每个 `modules/<模块>/<模块>.h` 模块声明里的 `dependencies:` 行。
 * 链接一个模块就会编进它依赖闭包里的所有模块，modules-closure 据此要求整个闭包都已登记。
 * 升级 JUCE 时按新版本的模块头重抄这张表（`.juce-version` 与 version 不一致时 modules-closure 判红）。
 */
export const JUCE_MODULE_REQUIRES = {
    version: "8.0.8",
    modules: {
        juce_analytics: ["juce_gui_basics"],
        juce_animation: ["juce_gui_basics"],
        juce_audio_basics: ["juce_core"],
        juce_audio_devices: ["juce_audio_basics", "juce_events"],
        juce_audio_formats: ["juce_audio_basics"],
        juce_audio_plugin_client: ["juce_audio_processors"],
        juce_audio_processors: ["juce_gui_extra", "juce_audio_basics"],
        juce_audio_utils: ["juce_audio_processors", "juce_audio_formats", "juce_audio_devices"],
        juce_box2d: ["juce_graphics"],
        juce_core: [],
        juce_cryptography: ["juce_core"],
        juce_data_structures: ["juce_events"],
        juce_dsp: ["juce_audio_formats"],
        juce_events: ["juce_core"],
        juce_graphics: ["juce_events"],
        juce_gui_basics: ["juce_graphics", "juce_data_structures"],
        juce_gui_extra: ["juce_gui_basics"],
        juce_javascript: ["juce_core"],
        juce_midi_ci: ["juce_audio_basics"],
        juce_opengl: ["juce_gui_extra"],
        juce_osc: ["juce_events"],
        juce_product_unlocking: ["juce_cryptography"],
        juce_video: ["juce_gui_extra"],
    },
};

/**
 * CMake 引入的每个第三方来源，以及它带进二进制或构建的组件。键的写法：
 *   juce::<模块>  JUCE 的目标，CMake 里写 juce::juce_x 或不带命名空间的 juce_x 都算；
 *                 juce_add_plugin 隐含 juce::juce_audio_plugin_client；JUCE 模块按 JUCE_MODULE_REQUIRES 展开依赖
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
 * 体量预算。ADR 超限判红（CLAUDE.md 把 ADR 4 KB 定为硬上限），其余条目超限只报 WARN，
 * 由 docs-budget 检查执行。字节按 UTF-8 计。
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
