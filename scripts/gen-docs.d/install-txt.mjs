// SPDX-License-Identifier: GPL-3.0-or-later
//
// zip 里的 INSTALL.txt：对应源码指引 + 安全警告导语 + 标了 installTxt 的规则 + 平台安装步骤 + 商标署名，只有中英两种语言。
// 步骤取自 docs/INSTALL.md 的标记块 install:<win|mac>:<en|zh>，警告与规则取自硬约束的同一份数据。
// 它不落在仓内文件里，所以没有落地面，只经 `--emit install-txt` 输出；validate 在每次检查时用假变量渲染两个平台，
// 渲染失败或缺警告、缺必选规则当场判红，不必等到打包。

import {
    I18N_FILE,
    INSTALL_FILE,
    SOURCE_FILE,
    findRegion,
    fill,
    loadI18n,
    loadRuleSet,
    loadSource,
    readRepoText,
    sourceProblems,
} from "../lib/docs-kit.mjs";

const PLATFORMS = {
    win: {
        label: "Windows x64",
        vars: ["VERSION", "TAG", "COMMIT", "JUCE_VERSION", "JUCE_COMMIT", "WEBVIEW2_VERSION", "ZIP"],
    },
    mac: { label: "macOS arm64", vars: ["VERSION", "TAG", "COMMIT", "JUCE_VERSION", "JUCE_COMMIT", "ZIP"] },
};

const FAKE_VARS = {
    VERSION: "0.0.0",
    TAG: "v0.0.0",
    COMMIT: "0123456789abcdef0123456789abcdef01234567",
    JUCE_VERSION: "0.0.0",
    JUCE_COMMIT: "fedcba9876543210fedcba9876543210fedcba98",
    WEBVIEW2_VERSION: "0.0.0",
    ZIP: "Rinne-v0.0.0-fake.zip",
};

const TRADEMARK = {
    en: "VST® is a registered trademark of Steinberg Media Technologies GmbH. Cubase, LUNA, Logic Pro, GarageBand, macOS, Audio Units, Windows, WebView2 and other trademarks belong to their respective owners.",
    zh: "VST® 是 Steinberg Media Technologies GmbH 的注册商标。Cubase、LUNA、Logic Pro、GarageBand、macOS、Audio Units、Windows、WebView2 及其他商标归各自所有者所有。",
};

/** Markdown 转纯文本：去掉加粗，代码围栏改成缩进。 */
function plain(md) {
    let fence = false;
    return md
        .split("\n")
        .flatMap((line) => {
            if (/^\s*```/.test(line)) {
                fence = !fence;
                return [];
            }
            return [fence ? `    ${line}` : line.replace(/\*\*/g, "")];
        })
        .join("\n")
        .replace(/\n{3,}/g, "\n\n")
        .trim();
}

function render(ctx, platform, vars) {
    const spec = PLATFORMS[platform];
    if (!spec) throw new Error(`--platform 应为 ${Object.keys(PLATFORMS).join(" 或 ")}`);
    for (const k of spec.vars) if (!vars[k]) throw new Error(`缺变量 ${k}`);
    for (const k of ["COMMIT", "JUCE_COMMIT"])
        if (!/^[0-9a-f]{40}$/.test(vars[k])) throw new Error(`${k} 应为 40 位小写十六进制`);

    const set = loadRuleSet(ctx.root);
    const install = readRepoText(ctx.root, INSTALL_FILE);
    if (install === null) throw new Error(`缺 ${INSTALL_FILE}`);
    const steps = (lang) => {
        const r = findRegion(install, `install:${platform}:${lang}`);
        if (!r) throw new Error(`${INSTALL_FILE} 缺标记块 install:${platform}:${lang}`);
        return plain(fill(r.body, vars));
    };
    const rules = set.rules.filter((r) => r.installTxt);

    const source = [
        "Corresponding Source for this build (GPLv3 §6(d)):",
        `  Rinne  ${vars.VERSION}: https://github.com/synchain-oss/rinne/tree/${vars.TAG}  (commit ${vars.COMMIT})`,
        `  JUCE   ${vars.JUCE_VERSION} (bundles the VST3 SDK and AudioUnitSDK): https://github.com/juce-framework/JUCE/tree/${vars.JUCE_COMMIT}`,
        ...(platform === "win"
            ? [`  WebView2 SDK ${vars.WEBVIEW2_VERSION} (Windows only; NuGet package Microsoft.Web.WebView2)`]
            : []),
        "  Build steps: docs/BUILDING.md at the tag above",
    ];
    const parts = [
        `Rinne ${vars.VERSION} - ${spec.label}`,
        source.join("\n"),
        `READ THIS FIRST / 使用前必读\n\n${set.lead.en}\n\n${set.lead.zh}`,
        ...rules.map((r, i) => `${i + 1}. ${r.en.lead}: ${r.en.body}\n\n${r.zh.lead}：${r.zh.body}`),
        `INSTALL / 安装\n\n${steps("en")}\n\n${steps("zh")}`,
        `${TRADEMARK.en}\n${TRADEMARK.zh}`,
    ];
    return parts.join("\n\n") + "\n";
}

export default {
    name: "install-txt",
    sources: [SOURCE_FILE, I18N_FILE, INSTALL_FILE],
    surfaces: [],
    render: (ctx) => render(ctx, "win", FAKE_VARS),
    emit: (ctx, { platform, vars }) => render(ctx, platform, vars),
    validate(ctx) {
        const base = sourceProblems(loadSource(ctx.root), loadI18n(ctx.root));
        if (base.length) return []; // 真源本身的问题由硬约束块报告，这里不重复
        const set = loadRuleSet(ctx.root);
        const out = [];
        for (const platform of Object.keys(PLATFORMS)) {
            let text;
            try {
                text = render(ctx, platform, FAKE_VARS);
            } catch (e) {
                out.push(`假变量渲染 ${platform} 版 INSTALL.txt 失败：${e.message}`);
                continue;
            }
            const need = [
                set.lead.en,
                set.lead.zh,
                FAKE_VARS.JUCE_COMMIT,
                ...set.rules.filter((r) => r.installTxt).flatMap((r) => [r.en.body, r.zh.body]),
            ];
            for (const n of need) {
                if (!text.includes(n)) out.push(`${platform} 版 INSTALL.txt 缺少应收入的内容：${n.slice(0, 24)}…`);
            }
            if (/\{\{|\}\}/.test(text)) out.push(`${platform} 版 INSTALL.txt 里还有未替换的占位符`);
        }
        return out;
    },
};
