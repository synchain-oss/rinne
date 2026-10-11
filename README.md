**English** | [简体中文](README.zh-CN.md)

# Synchain Rinne

> An open-source plugin pair that feeds a channel back into itself, with a loop delay you set.

**Rinne** (pronounced *RIN-neh*, from the Japanese 輪廻, "cycle"; Chinese name 回环) is two plugins. **Rinne Input** goes in the first insert slot and holds every control. **Rinne Output** goes after the last plugin you want inside the loop and only handles pairing. Everything between them is fed back into Rinne Input, with a loop delay that is designed not to depend on buffer size or export mode.

> **Status: in development.** There is no release yet. Work happens on the `feature/v1` branch.

> **Warning:** feedback can build up to very loud levels. Rinne starts with feedback at zero and has a limiter and an emergency cut, but keep your monitoring low while you experiment.

Planned formats: VST3 on Windows and macOS, Audio Unit on macOS.

Licensed under [GPL-3.0-or-later](LICENSE). Made by [Synchain](https://synchain.ca).
