# ADR-0005：平台、格式与工具链

## Summary (EN)

- **Decision**: VST3 on Windows x64 and macOS, Audio Unit on macOS; macOS releases are Apple Silicon only (macOS 11+) and are not signed or notarized; JUCE 8.0.8 is pinned to an exact commit.
- **Consequences**: macOS users unlock the plugins once; Intel Macs are not supported; builds follow pin files.
- **Compatibility**: moving to JUCE 8.0.11 or later is a separate decision, evaluated for v1.1.

## 状态

已采纳（2026-10-12）。

## 决定

- **格式与平台**：Windows x64 的 VST3；macOS 的 VST3 与 AU（`aufx`）。不做 AAX；Linux 只用于 core 的 sanitizer CI，不出插件。
- **macOS 架构**：正式包只出 arm64，部署目标 macOS 11.0，要求系统 WebKit 15.4 及以上（WebKit 低于 26.4 时界面缩放固定为 100%）。探针构建出 universal，便于在借来的机器上实测。
- **界面**：WebView。Windows 静态链接 WebView2 loader（NuGet 包版本由 CMake 的 `WEBVIEW2_VERSION` 给出），用户机器需要 WebView2 Runtime；macOS 用系统 WKWebView。
- **JUCE**：8.0.8，`.juce-commit` 钉 40 位 commit，只经 `-DJUCE_PATH` 传入；CMake 守卫断言检出的 HEAD 等于它，版本行作第二道检查。v1 不给 JUCE 打补丁。
- **工具链**：MSVC 2022（静态 CRT）、Xcode、CMake ≥ 3.22、Ninja；core 为 C++17，第一方代码零 warning；Catch2 做单测，pluginval 与 auval 做验证，clang-format 与 prettier 管格式，仓内脚本用 Node 22。版本全部来自钉版文件。
- **不签名、不公证**：v1 的 macOS 包没有 Developer ID 签名，也未经 Apple 公证。包内附安装与卸载脚本，INSTALL 第一屏写明「先解锁再开 DAW」；CI 用 `codesign --verify` 核对包内的 ad-hoc 签名完整。

## 后果

- macOS 用户首次安装要多一步解锁；文档与安装脚本要把这一步讲清楚，不能让用户误点「移到废纸篓」。
- Intel Mac 不支持，Logic Pro 未经测试；对外文案不得暗示支持。
- 同一组钉版文件在本机、CI 与发布流水线上给出同一套工具，构建可以复现。

## 被否决的备选

- **签名与公证**：需要付费的 Apple Developer ID，证书与公证凭据要放进 CI secret 并长期维护。单人维护的 v1 不承担这些，代价是首次安装多一步解锁。将来引入时凭据只走 secret。
- **JUCE 8.0.11 及以上或 9.x**：内置的 VST3 SDK 换成 3.8.0（MIT），音频处理模块拆分，从 SCVB 复制的 CMake 片段与封装细节都要重新核对。放到 v1.1 评估；升级同时是许可变更（ADR-0007）。
- **universal 正式包**：Intel 上无法实测，发出去等于未经验证的承诺。
