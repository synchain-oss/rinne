# web/fonts/

界面用的四个离线字体子集。插件完全离线，`web/` 一律经本地 `@font-face` 引用它们，不得出现任何在线字体服务的域名。
许可：四款都是 SIL Open Font License 1.1，逐个版权行见 `REUSE.toml`，说明与改名依据见 `THIRD-PARTY-NOTICES.md`。

| 文件 | 来源 | 字形数 | 字节数 | 用途 |
| --- | --- | --- | --- | --- |
| `SpaceGrotesk.woff2` | Space Grotesk，固定到 wght=600 | 354 | 19200 | 标题、大数字、主按钮 |
| `RinneSans.woff2` | IBM Plex Sans，固定到 wght=400、wdth=100，改名 | 355 | 27792 | 正文 |
| `RinneMono.woff2` | IBM Plex Mono Regular，改名 | 349 | 18664 | 标签、读数、状态灯 |
| `NotoSansSC.woff2` | Noto Sans SC，字重轴收窄到 300 到 700 | 4178 | 1072200 | 汉字与全角标点，三条字体栈共同的内嵌回退 |

字形数是 cmap 里的码位数；字节数是 `scripts/fetch_fonts.py` 当前产出的大小，重跑后在同一个 PR 里更新这张表。

## 命名

IBM Plex 的保留字体名是 "Plex"。子集化属于 Modified Version，按 OFL-1.1 第 3 条不得再用这个名字，
所以 `name` 表的 nameID 1、3、4、6（以及有则 16、17）、文件名和 CSS 族名都是 Rinne Sans 与 Rinne Mono。
版权、商标声明、许可证声明与网址（nameID 0、7、13、14）原样保留，按第 2 条随分发物带走。
`scripts/check-font-names.py` 检查这些，包括 `name` 表里除署名外不得再出现 "Plex"，以及 nameID 0 与 `REUSE.toml` 的登记一致。

## 字符集

字符集宽覆盖，让页面实现时不必因为缺字回头改字体。拉丁三款拿到全部非汉字字符，Noto Sans SC 拿到全集：

1. `web/` 下 js、mjs、html、css 去掉注释后出现的全部非 ASCII 字符（上屏文本的真实来源）；
2. `charset-handoff.txt`：设计交接包三语词条用到的全部字符；
3. GB 2312 一级汉字 3755 字，覆盖通用规范汉字一级 3500 字的绝大部分常用字；
4. ASCII 可打印字符、Latin-1 补充、拉丁扩展 A；
5. 兜底字符：弯引号、破折号、省略号、窄不换行空格、箭头、数学符号、中日韩标点、全角形式。

汉字只覆盖上面的集合：GB 2312 一级汉字是通用规范汉字一级 3500 字的近似（3755 字，覆盖绝大部分常用字），
不是它的精确超集。集合之外的字（例如用户的轨道名或预设名里的生僻字）不会出方块：三条字体栈在 Noto Sans SC 之后
依次回退到 `-apple-system`、`"PingFang SC"`、`system-ui`、`"Segoe UI"`、`"Microsoft YaHei"`，即宿主系统自带的中文字体
（Rinne Mono 一栈在 Noto 之后是 `ui-monospace`、`"Cascadia Code"`、`Consolas`、`monospace`，等宽字体里没有的汉字由系统再回退），
所以回退显示的是系统字体的字形，字重和字形风格与内嵌字体略有差异。

Noto Sans SC 自己没有的字符（例如拉丁扩展 A 里的一部分）由前面的拉丁字体提供，三条字体栈都是「拉丁族、Noto Sans SC、系统字体」。

法语排版用窄不换行空格 U+202F。Space Grotesk、Rinne Mono 与 Noto Sans SC 没有这个字形，
`fetch_fonts.py` 把它指向细空格（没有就指向不换行空格）的字形，这样宽度在整条字体栈里是确定的。

## 字重

字重合同只有三条：Space Grotesk 600；Rinne Sans 与 Rinne Mono 400；Noto Sans SC 300 到 700。
`tokens.css` 里 `@font-face` 声明的区间比实际内嵌的字重宽，是为了让区间内的字重请求落到内嵌字形而不是整族回退，
配合 `font-synthesis: none`，缺字重时宁可走字体栈，也不合成伪粗体。拉丁文需要加重时用 Space Grotesk 600，不要请求 Rinne Sans 的 500、600。

## 重新生成

只在字符覆盖检查（`scripts/check-font-coverage.py`）报红时，在能联网的开发机上运行；禁止手改 woff2：

```
python scripts/fetch_fonts.py
```

需要 `fontTools` 与 `brotli`。脚本从 google/fonts 的同一个钉死的提交下载上游字体并校验 sha256，同样的输入产出逐字节相同的文件。
已经下载好源文件时可以用 `--source-dir 目录` 离线重跑。网络不通又必须改词条时，先让词条用现有字符集里有的字，
网络恢复后再重跑本脚本。
