# web/js/juce/

JUCE 官方的网页端辅助脚本（`index.js`、`check_native_interop.js`），取自 JUCE 8.0.8
（`modules/juce_gui_extra/native/javascript/`），内容零改动，只有行尾按本仓 `.gitattributes` 取 LF。
随 JUCE 的许可（AGPL-3.0-only），版权归 Raw Material Software Limited，不加 Synchain 的版权头，
由 `REUSE.toml` 的 `juce-web-js` 小节声明。

`web/shared/bridge.js` 在检测到真 JUCE 宿主时惰性 `import("../js/juce/index.js")` 来使用它。

不要在预览页里直接 import 这两个文件：`check_native_interop.js` 在没有 JUCE 宿主的普通浏览器里会自己造一个
占位的 `window.__JUCE__`（带空的 `initialisationData.__juce__functions` 和纯本地的 `backend`）。
`createBridge()` 按「`backend` 存在且 `__juce__functions` 非空」判定真宿主，并让显式注入的 mock 优先，
所以不会被占位对象骗过去；但页面自己直接调 `getNativeFunction()` 仍会得到一个永不 resolve 的 Promise。
预览一律走 `createBridge({ role, mockBackend })`。
