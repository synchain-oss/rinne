# 界面三语审校日志（fr）

## Summary (EN)

- **Scope**: every French UI string registered for review (335 keys in four pages) plus the four hard rules and their lead paragraph, checked against the Chinese and English text.
- **Result**: 32 UI strings and the lead paragraph of the hard rules were changed (wording, terminology, grammar, decimal point); the other 303 registered strings and the four rule bodies were kept as they were. No Chinese or English text was touched.
- **Length**: no string was lengthened beyond what its design box allows; `check-i18n`, `check-text-fit` and the font coverage check all pass.
- **Status**: AI cross-check finished; the maintainer spot-check is **still pending**. `frReview.status` in `docs/hard-rules.i18n.json` stays `pending`, and the four `web/i18n/fr-review/*.json` files keep their entries (status "已复核，待用户抽检") until the spot-check comment arrives.

## 范围与口径

对照中文（含 `docs/USER_GUIDE.zh-CN.md` 的「使用前必读」）和英文逐条看了四件事：

1. **意思**：与中文一致；安全相关的词一个都不弱化（切断、恢复、限制器、失控保护、从 0 开始、放置位置）。
2. **术语**：同一个状态、按钮、控件只有一种说法；界面里被引用的按钮名与它真实的词条逐字相同（如「Rétablir le retour」「Réappairer」「Vers un groupe libre」「Non vérifiable」）。
3. **排版**：`: ; ? ! %` 前与 « » 内侧用不换行空格，弯撇号 ’，负号 −，省略号单字符，数字与单位之间不换行；小数点三语统一用「.」。
4. **占位符**与数字：三语一致（`check-i18n` 机查，另外逐条核对了数字）。

登记在 `web/i18n/fr-review/*.json` 的条目共 335 条：通用 77、Input 184、Output 40、首启链与导览 34。词条表里另有 50 个键没有登记，不在抽检范围：品牌名、单位、语言名这类不翻译的，沿用 SCVB 已审的缩放确认词，以及规则页四条规则的生成段（规则的 fr 以 `docs/hard-rules.i18n.json` 为准，已在下面一并复核）。

| | 条目数 | 改动 | 未改 |
|---|---|---|---|
| 通用（common） | 77 | 4 | 73 |
| Input | 184 | 19 | 165 |
| Output | 40 | 7 | 33 |
| 首启链与导览（guide） | 34 | 2 | 32 |
| 硬约束引导词与四条规则 | 5 | 1 | 4 |
| 合计 | 340 | 33 | 307 |

没改的条目不逐条列。没有为压长度而缩短任何条目：改动都是措辞、术语、语法或小数点问题，变长的都还在各自的预算里（`check-text-fit` 通过）。

## 术语决定

| 概念 | fr 定稿 | 说明 |
|---|---|---|
| Feedback / 反馈量 | réinjection（标签 RÉINJECTION，参数名 Réinjection） | 项目术语表既定，规则页与全界面一致；业内也常直接保留 Feedback，见抽检清单第 1 点 |
| Dry / 干声 | sec（SEC，「signal sec」） | 术语表初稿写的是 Direct，但界面从初译起一直用 sec（法语音频里与 dry 对应的常规说法）；沿用 sec，全界面无一例外 |
| Return / 回送 | retour | |
| Loop / 环路，Loop delay D | boucle，délai de boucle D | |
| Panic / 紧急切断 | Coupure d’urgence（按钮 COUPURE D’URGENCE） | 规则页开头段落首次出现时写「(Panic)」 |
| Restore return / 恢复回送 | Rétablir le retour | 唯一的恢复入口，全界面逐字相同；与「Reprendre」（接管另一个实例的组）分开 |
| Bypass / 旁路 | contourner，CONTOURNÉ | |
| Track / 轨道，Channel / 通道 | piste；voie（只在放置规则里指宿主的通道） | |
| Pair / 配对 | appairer，appairage，Réappairer | |
| Preset / 预设，Factory | préréglage，USINE | |
| Limiter / soft clip / DC | limiteur / écrêtage doux / décalage DC | |
| Self-oscillation / 自激 | auto-oscillation（AUTO-OSC.） | |
| Runaway protection / 失控保护 | protection anti-emballement（ANTI-EMBALLEMENT） | |
| Fade in / 淡入 | revient en fondu，FONDU ENTR. | |
| Tour / 导览 | visite | 页脚问号的提示原来写「guide」，已改成 visite |
| 业内通用词 | plug-in、slot、insert、fader、tail、build、ping-pong 保留英文 | 项目既定口径 |
| 产品名 | Rinne Input、Rinne Output、Synchain、ASIO-Guard、Cubase 不翻译 | |

## 改动明细

### 通用（`web/i18n/common.js`）

| key | 原 fr | 定稿 fr | 理由 |
|---|---|---|---|
| `common.footer.helpTip` | Revoir les règles et le guide | Revoir les règles et la visite | 导览在 fr 里统一叫「visite」（规则页按钮、导览全文都是），这里的「guide」与它不一致 |
| `band.action.takeoverTip` | Cette instance perdra le groupe. | L’autre instance perdra ce groupe. | 「Cette instance」会被读成当前实例；zh / en 指的是被接管的那个实例 |
| `common.pair.srMismatch` | FRÉQ. INCOHÉRENTE | ÉCHANTILLONNAGE DIFFÉRENT | 「FRÉQ. INCOHÉRENTE」可被读成音频频率不一致；改成明确指采样率，与下面两条消息同词 |
| `about.adv.tailTip` | Indique à l’hôte la durée de queue de ce plug-in, pour qu’il ne le suspende pas pendant le silence et n’interrompe pas la boucle. Auto = infini en VST3, 30 s en AU. | Indique à l’hôte la durée de queue (tail) de ce plug-in, pour qu’il ne le suspende pas pendant le silence et n’interrompe pas la boucle. Auto = infini en VST3, 30 s en AU. | 选项名按业内说法保留英文 Tail，说明里补上 (tail) 让两处对得上 |

### Input（`web/i18n/input.js`）

| key | 原 fr | 定稿 fr | 理由 |
|---|---|---|---|
| `band.in.abi` | Versions différentes (In {a} / Out {b}). | Versions incompatibles (In {a} / Out {b}). | 状态灯是「VERSION INCOMPATIBLE」，消息里的「différentes」不是同一个词；对齐用词，语义也更接近「已拒绝」 |
| `band.in.srMismatch` | Fréquences différentes ({a} / {b} Hz) : retour arrêté. | Échantillonnage différent ({a} / {b} Hz) : retour arrêté. | 「Fréquences différentes」单独出现像是音频频率；改成明确指采样率 |
| `in.fb.capNote` | Réglé {s} %, effectif 100 % | Réglage {s} %, effectif 100 % | 「Réglé」缺主语、词性悬空；改成名词「Réglage」与「effectif」相配 |
| `band.in.limiterCap100` | Limiteur désactivé : réglée {s} %, effective 100 %. | Limiteur désactivé : réglage {s} %, effectif 100 %. | 「réglée … effective」阴性形容词没有对应的名词，语法错误 |
| `in.dly.noteTipD` | Pointée : 1,5 × la valeur de note | Pointée : 1.5 × la valeur de note | 小数点三语统一用「.」，原文用了「,」 |
| `in.lim.ceilingTip` | Plafond du limiteur, de −24 à 0 dBFS par pas de 0,1 ; les préréglages ne le modifient pas | Plafond du limiteur, de −24 à 0 dBFS par pas de 0.1 ; les préréglages ne le modifient pas | 小数点三语统一用「.」，原文用了「,」 |
| `in.panic.tip` | Fait disparaître aussitôt le retour jusqu’à 0 et le maintient coupé ; le signal sec continue de passer. Pour le rétablir, utilisez Rétablir le retour dans la barre d’avis ci-dessus. | Ramène aussitôt le retour à 0 en fondu et le maintient coupé ; le signal sec continue de passer. Pour le rétablir, utilisez Rétablir le retour dans la barre d’avis ci-dessus. | 「disparaître … jusqu’à 0」不通；改成「淡出到 0」的正常说法，安全含义不变 |
| `in.hint.cut` | Retour coupé — cliquez sur Rétablir le retour pour le refaire entrer en fondu | Retour coupé — cliquez sur Rétablir le retour pour le faire revenir en fondu | 「refaire entrer en fondu」不自然；与其它处的「revient en fondu」统一 |
| `in.loop.tip` | Désactivée, la boucle fait disparaître le retour en fondu sans vider la boucle ; automatisable, et assignable à un contrôleur comme coupure d’urgence matérielle. | Désactivée, la boucle fait disparaître le retour en fondu sans vider son contenu ; automatisable, et assignable à un contrôleur comme coupure d’urgence matérielle. | 连用两个「la boucle」；zh 说的是保留环路内容 |
| `band.in.stateDecodeFailed` | Réglages illisibles : valeurs par défaut (réinj. 0). | Réglages illisibles : valeurs par défaut (réinjection 0). | 去掉缩写「réinj.」，宽度够就写全，与全界面的「réinjection」一致 |
| `band.in.nextStep` | Ensuite : Rinne Output (dernier slot pré-fader), groupe {g}. | Ensuite : insérez Rinne Output (dernier slot pré-fader), groupe {g}. | 原文没有动词，zh / en 都是「插入 / add」；补上「insérez」 |
| `band.in.takeover` | Groupe {g} tenu par une instance inactive. | Groupe {g} : instance inactive. Reprendre ? | zh / en 是问句（要接管吗），原文变成了陈述；问句里的「Reprendre」就是旁边按钮的文字 |
| `band.in.crossProcess` | Processus différents : délai variable. | Autre processus : délai fixe non garanti. | zh「不保证固定」；「délai variable」成了断言；同时与状态灯「AUTRE PROCESSUS」同词 |
| `band.in.crossTrack` | Noms de piste différents : délai variable. | Noms de piste différents : délai fixe non garanti. | 同上，zh「不保证固定」 |
| `band.in.unstable` | Cadencement instable : mode tolérant, délai variable. | Cadencement instable : mode tolérant, délai fixe non garanti. | 同上，zh「不再保证固定」 |
| `band.in.exportViolation` | L’export peut différer : blocs trop grands ; augmentez D. | Dernier export peut différer : gros blocs ; relevez D. | 补回 zh / en 的「上次」。消息位是单行（约 290 px），「blocs trop grands」放不下，改成「gros blocs」，补救动作写「relevez D」（对应 en 的 raise D，两条导出提示用同一说法） |
| `band.in.exportOverrun` | L’export peut différer : D trop court ; augmentez D. | Dernier export peut différer : D trop court ; relevez D. | 补回「上次」；同上，补救动作写「relevez D」，宽度也因此放得下 |
| `band.in.exportPeer` | L’export peut différer : l’hôte d’Output déborde. | Dernier export peut différer : l’hôte d’Output déborde. | 补回「上次」；四条导出提示统一以「Dernier export peut différer」开头 |
| `band.in.exportResync` | L’export peut différer : boucle resynchronisée. | Dernier export peut différer : boucle resynchronisée. | 补回「上次」；同上 |

### Output（`web/i18n/output.js`）

| key | 原 fr | 定稿 fr | 理由 |
|---|---|---|---|
| `band.out.abi` | Versions différentes (Out {a} / In {b}). | Versions incompatibles (Out {a} / In {b}). | 同上 |
| `band.out.srMismatch` | Fréquences différentes ({a} / {b} Hz) : arrêt. | Échantillonnage différent ({a} / {b} Hz) : retour arrêté. | 同上；zh 的「回送已停止」补回，并与 Input 一侧同词 |
| `band.out.takeover` | Groupe {g} tenu par une instance inactive. | Groupe {g} : instance inactive. Reprendre ? | 同上，与 Input 一侧同词 |
| `band.out.crossProcess` | Processus différents : délai variable. | Autre processus : délai fixe non garanti. | 同上 |
| `band.out.crossTrack` | Noms de piste différents : délai variable. | Noms de piste différents : délai fixe non garanti. | 同上 |
| `band.out.ipc` | Pas de mémoire partagée : audio direct seul. | Pas de mémoire partagée : audio transmis tel quel. | 「audio direct seul」读不通；改成明确的「直通」说法 |
| `band.out.newerProject` | Projet plus récent ({a} / {b}) : défauts. | Projet plus récent ({a} / {b}) : valeurs par défaut. | 「défauts」不是规范说法，与 Input 一侧统一 |

### 首启链与导览（`web/i18n/guide.js`）

| key | 原 fr | 定稿 fr | 理由 |
|---|---|---|---|
| `tour.in.step3.body` | Le gain du retour ; il démarre à 0. Baissez l’écoute et montez-le lentement : plus de retour, plus vite l’auto-oscillation. Avec le mix à 50 % ou plus et sans perte dans la boucle, elle est certaine au-delà de 100 % (l’arc ambre) ; un gain dans la boucle l’avance. | Le gain du retour ; il démarre à 0. Baissez l’écoute et montez-le lentement : plus de retour, plus vite l’auto-oscillation. Avec le mix à 50 % ou plus et sans perte dans la boucle, elle est certaine au-delà de 100 % (l’arc ambre) ; un gain dans la boucle peut l’avancer. | 原文「l’avance」丢了「peut」（zh / en 都是「可能」），改回可能；仍在 5 行内 |
| `tour.in.step5.body` | Le retour passe toujours par un blocage DC et un écrêtage doux ; limiteur et protection anti-emballement sont actifs par défaut. En cas de souci, appuyez sur Coupure d’urgence, même visite ouverte. Rétablir le retour est dans la barre d’avis. | Le retour passe toujours par un blocage DC et un écrêtage doux ; limiteur et protection anti-emballement sont actifs par défaut. En cas de problème, appuyez sur Coupure d’urgence, même en pleine visite ou fenêtre ouverte. Rétablir le retour est dans la barre d’avis. | 补回 zh / en 的「弹窗打开时也能点」；「souci」对安全场景太轻 |

### 硬约束（`docs/hard-rules.i18n.json`）

| key | 原 fr（片段） | 定稿 fr（片段） | 理由 |
|---|---|---|---|
| `lead`（规则页总提醒） | …qu’une boucle ne larsenera jamais… | …qu’une boucle ne produira jamais de larsen… | 「larsener」是口语动词；改成「produire du larsen」，语义不变 |

四条规则的引导词与正文逐条与中文、英文核对过，没有改动，所以规则页的生成物（`web/shared/hard-rules.gen.js`、`web/i18n/guide.js` 的生成段）不需要重新生成。

## 检查结果

| 检查 | 结果 |
|---|---|
| `check-i18n --strict-unused` | 通过 |
| `check-text-fit` | 通过：0 项超出预算 |
| 字体覆盖与名称检查 | 通过：零缺字 |
| `gen-docs --check` | 通过 |

## 已知差异（未改）

- 规则页四条规则正文里，`: ;` 和 « » 旁用的是窄不换行空格 U+202F，其余条目用 U+00A0；两种都通过 `check-i18n`，字体里都有字形，所以没有动。统一成 U+00A0 要重新生成规则页的生成物，不在本次范围内。
- 四条导出不一致的消息（`band.in.export*`）没有登记文本宽度预算，`check-text-fit` 看不到它们；改动后的四条按同一消息位的预算（`band.in.exportMismatch`，302 px，一行）用同一套量宽方法手算过，都在范围内（最长一条约 283 px，上限约 290 px）；写法是按这个宽度选的（例如「blocs trop grands」放不下，用了「gros blocs」）。这四个键应补上预算条目，这样以后改动才有检查兜底。
- 工厂预设的 fr 名在 `presets/factory/*.json` 里，不在词条表中，本次没有改动，列在抽检清单末尾；其中 `f-edge-95` 的名字用了直撇号，数字与 % 之间是普通空格，不合本仓的排版规则（预设文件不在词条表的检查范围内），留待单独修正。

## 抽检清单

**怎么回**：在 PR 上评论：通过；或列出要改的 key 与建议。

★ 表示本次改过的条目。表中法语里的空格都是 U+00A0（不换行空格），看起来和普通空格一样。表共 51 行、86 个键，按风险排序：规则与安全相关 → 首启链与导览 → 本次改动较大的条目 → 其余抽样 → 请你表态的几处。

**请你先看这几处（我拿不准）**

1. **Feedback 译作 Réinjection**。术语表这样定，规则页和全界面都跟着用。法语使用者看延迟类效果器时常见的是直接写 Feedback；若想改成 Feedback，规则页正文要一起改。我的意见：保持 Réinjection，因为规则页要让不懂行的人也读得懂。
2. **Dry 译作 SEC（术语表初稿写 Direct）**。界面一直用 sec，我认为比 Direct 准确（Direct 会被读成「直通 / 现场」）。
3. **关于面板的许可短口径 `about.license` 的 fr 保持英文**（设计稿里就是逐字英文，法律措辞以英文为准）。如果想要法语，建议译为：Code source : GPL-3.0-or-later. Le binaire dans son ensemble est distribué sous GPLv3 ; sa partie JUCE relève de l’AGPLv3 (GPLv3 §13). Aucune garantie.
4. **Reprendre**（接管另一个实例的组）与 **Rétablir**（恢复回送）是两个不同的动词，故意分开，免得「恢复」与「接管」混淆。
5. **状态灯 stale 写作 LIAISON PERDUE**（术语表初稿是 PERDU），因为单写「perdu」不说什么丢了。
6. 工厂预设的 fr 名（下表）按旧稿原样，请看名字是否可以。

| id | zh | en | fr |
|---|---|---|---|
| `f-init` | Init | Init | Initialisation |
| `f-slapback-90` | Slapback 90 ms | Slapback 90 ms | Slapback 90 ms |
| `f-dub-quarter-dotted` | Dub 1/4D | Dub 1/4D | Dub 1/4D |
| `f-ping-pong-eighth` | Ping-Pong 1/8 | Ping-Pong 1/8 | Ping-pong 1/8 |
| `f-shortest-comb` | Shortest Comb | Shortest Comb | Peigne minimal |
| `f-long-hold-92` | Long Hold 92 % | Long Hold 92 % | Maintien long 92 % |
| `f-edge-95` | Edge 95 % | Edge 95 % | Bord d'oscillation 95 % |
| `f-light-mix-30` | Light Mix 30 % | Light Mix 30 % | Mélange léger 30 % |

| key | 所在界面 | zh | en | fr |
|---|---|---|---|---|
| ★ `规则页总提醒` | 规则页最上方的总提醒段落（也出现在 README 的安全警告里） | 反馈可能在一瞬间变得很响。Rinne 的用途就是把一条通道接回自己。请从反馈量 0 开始，试验时把监听音箱和耳机调小，并保持安全保护开启。Rinne 会对回送信号做限幅和异常值清理，也有紧急切断（Panic），但放在环路里的插件仍可能抬高送到音箱的电平；任何软件都无法保证环路永远不啸叫。本软件不提供任何担保（GPL-3.0 第 15–16 条）。 | Feedback can get loud — fast. Rinne deliberately feeds a channel back into itself. Start with Feedback at 0, keep monitors and headphones low while you experiment, and leave Rinne’s safety protection on. Rinne limits and sanitises the signal it feeds back and has a Panic button, but plugins you place inside the loop can still raise the level that reaches your speakers, and no software can guarantee a loop will never howl. Rinne comes with no warranty (GPL-3.0, sections 15–16). | La réinjection peut devenir très forte, très vite. Rinne renvoie volontairement une voie dans elle-même. Démarrez avec la réinjection à 0, gardez enceintes et casque à bas volume pendant vos essais et laissez la protection de Rinne activée. Rinne limite et assainit le signal renvoyé et dispose d’un bouton Coupure d’urgence (Panic), mais les plug-ins placés dans la boucle peuvent tout de même augmenter le niveau qui arrive à vos enceintes, et aucun logiciel ne peut garantir qu’une boucle ne produira jamais de larsen. Rinne est fourni sans aucune garantie (GPL-3.0, articles 15–16). |
| rules.r1.lead / .body | 规则页第 1 条：放置位置（引导词 + 正文） | 放置位置：Rinne Input 插在通道插入链的第一格；Rinne Output 插在你想放进环路的最后一个插件之后、推子分隔线之上（最简单：推子前区最后一格；宿主没有推子前区时，插在插入链最后一格）。两者之间的插件，包括宿主自带的插入效果器，都在环路里，它们自身的延迟会叠加到环路延迟 D 上；Output 之后的插件、推子、声像和发送都不在环路里。 | Placement: Put Rinne Input on the first insert slot of the channel. Put Rinne Output after the last plug-in you want in the loop and above the fader divider (simplest: the last pre-fader slot; if your host has no pre-fader section, the last insert slot). Every plug-in between them — including the host’s own insert effects — is inside the loop, and its own latency adds to the loop delay D; plug-ins after Output, the fader, pan and sends are not. | Placement : Placez Rinne Input dans le premier slot d’insert de la voie. Placez Rinne Output après le dernier plug-in voulu dans la boucle et au-dessus de la séparation du fader (le plus simple : dernier slot pré-fader ; si votre hôte n’a pas de section pré-fader, dernier slot d’insert). Tout plug-in situé entre les deux — y compris les effets d’insert propres à l’hôte — est dans la boucle, et sa propre latence s’ajoute au délai de boucle D ; les plug-ins après Output, le fader, le panoramique et les envois n’y sont pas. |
| rules.r2.lead / .body | 规则页第 2 条：从 0 开始（引导词 + 正文） | 从 0 开始：新实例的反馈量是 0，加载工程或预设时回送从 0 淡入。先调低监听音量，再从 0 慢慢往上调：回送越多越容易自激。混合在 50% 及以上、环路里没有衰减时，反馈超过 100% 一定会自激；环路里的插件带增益（EQ 提升、压缩补偿、饱和）时，更低也可能自激。限制器和失控保护默认开启，但挡不住长时间的高电平。 | Start from 0: A new instance starts at 0% feedback, and the return fades in from 0 when a project or preset loads. Turn your monitors down, then raise feedback slowly from 0: the more return, the easier the loop self-oscillates. With Mix at 50% or more and nothing attenuating in the loop, feedback above 100% always self-oscillates; gain-adding plug-ins in the loop (EQ boosts, compressor make-up, saturation) can make it happen lower. The limiter and runaway protection are on by default, but they can’t protect you from sustained loud levels. | Partir de 0 : Une nouvelle instance démarre à 0 % de réinjection, et le retour revient en fondu depuis 0 au chargement d’un projet ou d’un préréglage. Baissez le volume d’écoute, puis montez lentement la réinjection depuis 0 : plus il y a de retour, plus la boucle entre facilement en auto-oscillation. Avec le mix à 50 % ou plus et rien qui atténue dans la boucle, une réinjection au-delà de 100 % entre toujours en auto-oscillation ; des plug-ins ajoutant du gain dans la boucle (boost d’EQ, compensation de compresseur, saturation) peuvent la provoquer plus bas. Le limiteur et la protection anti-emballement sont activés par défaut, mais ils ne vous protègent pas contre des niveaux élevés prolongés. |
| rules.r3.lead / .body | 规则页第 3 条：出事先切断（引导词 + 正文） | 出事先切断：听到啸叫或异常，先按「紧急切断」：它只切断回送，干声照常通过；恢复请点提示条里的「恢复回送」。窗口关着时，最快的办法是在宿主里旁路 Rinne Input。推子不在环路里，只拉推子只是听不见，环路仍在增长；停止播放和静音也停不住自激。 | Cut it first: If you hear howling or anything odd, press Panic first: it cuts only the return, and the dry signal keeps playing; restore it with Restore return in the notice bar. With the window closed, the fastest way is to bypass Rinne Input in the host. The fader isn’t in the loop — pulling it down only hides the sound while the loop keeps growing, and stopping playback or muting won’t stop self-oscillation either. | Couper d’abord : Si vous entendez un larsen ou quoi que ce soit d’anormal, appuyez d’abord sur Coupure d’urgence : elle ne coupe que le retour, le signal sec continue de passer ; rétablissez-le avec Rétablir le retour dans la barre d’avis. Fenêtre fermée, le plus rapide est de contourner Rinne Input dans l’hôte. Le fader n’est pas dans la boucle — le baisser ne fait que masquer le son pendant que la boucle continue de croître, et arrêter la lecture ou couper le son n’arrête pas non plus l’auto-oscillation. |
| rules.r4.lead / .body | 规则页第 4 条：一组一对（引导词 + 正文） | 一组一对：一个组（A–H）只放一个 Rinne Input 和一个 Rinne Output，两端组号一致才会连接。复制轨道会连组号一起复制：副本会显示「冲突」，在副本的 Rinne Input 上点「移到空闲组」，再在副本的 Rinne Output 上点「重新配对」。 | One pair per group: A group (A–H) holds one Rinne Input and one Rinne Output, and they connect only when their groups match. Duplicating a track copies its group too: the copy shows CONFLICT — click Move to free group on the copy’s Rinne Input, then Re-pair on the copy’s Rinne Output. | Une paire par groupe : Un groupe (A–H) contient un Rinne Input et un Rinne Output, et ils ne se connectent que si leurs groupes correspondent. Dupliquer une piste copie aussi son groupe : la copie affiche CONFLIT — cliquez sur « Vers un groupe libre » dans le Rinne Input de la copie, puis sur « Réappairer » dans son Rinne Output. |
| `rules.title` | 规则页标题句 | 使用 Rinne 前必读：下面四条决定环路能否闭合，以及会不会伤到耳朵和音箱。 | Read this before using Rinne: these four rules decide whether the loop closes — and whether it can hurt your ears or speakers. | À lire avant d’utiliser Rinne : ces quatre règles déterminent si la boucle se ferme — et si elle peut abîmer vos oreilles ou vos enceintes. |
| `rules.btn.start`<br>`rules.btn.skip` | 规则页底部的两个按钮（首次打开时；重看时换成「继续导览」「我已了解」） | 我已了解，开始导览<br>跳过导览 | Got it, start the tour<br>Skip the tour | Compris, lancer la visite<br>Passer la visite |
| `rules.readFirst`<br>`rules.footnote` | 规则页：读到底之前的提示，以及脚注 | 请先读到最后一条<br>以后可点问号按钮或在「关于」里重新查看 | Read to the last rule first<br>You can reread these from the “?” button or About | Lisez d’abord jusqu’à la dernière règle<br>Vous pourrez les relire via le bouton « ? » ou À propos |
| `out.place` | Rinne Output 窗口顶部常驻的位置提示 | 放在你想放进环路的最后一个插件之后、推子分隔线之上（最简单：推子前区最后一格）。 | Put this after the last plug-in you want in the loop and above the fader divider (simplest: the last pre-fader slot). | Placez-le après le dernier plug-in voulu dans la boucle, au-dessus de la séparation du fader (le plus simple : dernier slot pré-fader). |
| `in.panic.label` | Input 的紧急切断按钮文字 | 紧急切断 | PANIC | COUPURE D’URGENCE |
| ★ `in.panic.tip` | 紧急切断按钮的悬停提示 | 立即把回送淡出到 0 并保持切断；干声照常通过。恢复请点上方提示条里的「恢复回送」。 | Fades the return to 0 at once and keeps it cut; the dry signal keeps playing. To restore, use Restore return in the notice bar above. | Ramène aussitôt le retour à 0 en fondu et le maintient coupé ; le signal sec continue de passer. Pour le rétablir, utilisez Rétablir le retour dans la barre d’avis ci-dessus. |
| `in.panic.cut`<br>`in.panic.cutTip` | 已切断后按钮上的字，以及它的悬停提示（再点不会恢复） | 已切断<br>回送已切断。再点这里不会恢复，请用上方提示条里的「恢复回送」。 | CUT<br>The return is cut. Clicking here again won’t restore it — use Restore return in the notice bar above. | COUPÉ<br>Le retour est coupé. Un nouveau clic ici ne le rétablit pas : utilisez Rétablir le retour dans la barre d’avis ci-dessus. |
| `band.in.cutPanic` | 消息位红条：手动紧急切断后 | 已紧急切断：回送已静音，干声照常通过。 | Panic on: return muted, dry plays on. | Coupure : retour coupé, sec intact. |
| `band.in.cutPanicHot` | 消息位红条：紧急切断后反馈仍高于 100% | 已紧急切断：反馈仍高于 100%，先调低。 | Panic on: lower feedback below 100%. | Coupure : réinj. > 100 %, baissez-la. |
| `band.in.cutNan` | 消息位红条：Input 内部出现 NaN/Inf，已切断 | 回送链出现 NaN/Inf：已切断并清空环路。 | NaN/Inf in return: cut, loop cleared. | NaN/Inf : retour coupé, boucle vidée. |
| `band.in.cutHuge` | 消息位红条：回送超过 +12 dBFS，已切断 | 回送超过 +12 dBFS：已切断并清空环路。 | Return > +12 dBFS: cut, loop cleared. | Retour > +12 dBFS : coupé, vidé. |
| `band.in.cutRunaway` | 消息位红条：失控保护降到 −24 dB 仍止不住，已切断 | 失控保护降到 −24 dB 仍未止住：已切断。 | Runaway not stopped at −24 dB: cut. | Emballement malgré −24 dB : coupé. |
| `band.action.restore`<br>`band.action.cooldownTip` | 红条最右的恢复按钮，以及切断后 1.5 秒冷却期内的悬停提示 | 恢复回送<br>刚切断，稍等片刻再恢复 | Restore return<br>Just cut — wait a moment before restoring | Rétablir le retour<br>Vient d’être coupé — patientez un instant avant de rétablir |
| ★ `in.hint.cut`<br>`toast.restored` | 页脚提示（切断中）与恢复成功后的轻提示 | 回送已切断，点「恢复回送」重新淡入<br>回送已恢复，从 0 淡入 | Return cut — click Restore return to fade back in<br>Return restored, fading in from 0 | Retour coupé — cliquez sur Rétablir le retour pour le faire revenir en fondu<br>Retour rétabli, il revient en fondu depuis 0 |
| `in.lim.tip`<br>`in.lim.confirm` | 限制器开关的提示、关闭前的确认框正文 | 回送限制器，随工程保存；关闭后只剩软削波兜底，反馈上限变为 100%。<br>关闭后环路只剩软削波兜底，反馈上限变为 100%；环路里的插件带增益时，声音仍可能很响。先调低监听音量。 | Return limiter, saved with the project; with it off only the soft clipper is left and feedback is capped at 100%.<br>With the limiter off only the soft clipper is left and feedback is capped at 100%; gain-adding plug-ins in the loop can still make it loud. Turn your monitors down first. | Limiteur du retour, enregistré avec le projet ; désactivé, il ne reste que l’écrêtage doux et la réinjection est plafonnée à 100 %.<br>Une fois le limiteur désactivé, il ne reste que l’écrêtage doux et la réinjection est plafonnée à 100 % ; des plug-ins ajoutant du gain dans la boucle peuvent encore produire un son très fort. Baissez d’abord le volume d’écoute. |
| `in.guard.tip` | 安全栏里失控保护开关的提示 | 回送持续过载时自动降低反馈（每秒 6 dB），平静 3 秒后回升。反馈不超过 90% 且环路不带增益时不会介入。 | When the return keeps overloading, feedback is lowered automatically (6 dB per second) and comes back after 3 s of calm. It stays out of the way at 90% feedback or less with no gain in the loop. | Si le retour reste en surcharge, la réinjection est réduite automatiquement (6 dB par seconde) et remonte après 3 s de calme. Elle n’intervient pas à 90 % de réinjection ou moins sans gain dans la boucle. |
| `band.in.guardActive`<br>`band.in.limiting` | 消息位：失控保护介入中；限制器持续压限 | 失控保护中：已自动降低反馈 {x} dB，平静 3 秒后回升。<br>反馈过强：限制器正在持续压限。 | Runaway protection: feedback down {x} dB for now.<br>Feedback hot: the limiter is working continuously. | Anti-emballement : réinjection réduite de {x} dB.<br>Réinjection forte : le limiteur travaille en continu. |
| `band.in.limiterOff`<br>★ `band.in.limiterCap100` | 消息位：限制器已关闭，反馈上限变为 100% | 限制器已关闭：反馈上限变为 100%。<br>限制器关闭：设定 {s}%，有效 100%。 | Limiter off: feedback capped at 100%.<br>Limiter off: set {s}%, effective 100%. | Limiteur désactivé : réinjection max 100 %.<br>Limiteur désactivé : réglage {s} %, effectif 100 %. |
| `in.fb.amberTip`<br>★ `in.fb.capNote` | 反馈旋钮：自激区（琥珀色段）的提示；限制器关闭时的「设定 / 有效」读数 | 回送越多越容易自激：混合在 50% 及以上、环路里没有衰减时，超过 100% 一定会自激；环路里的插件带增益时，更低也可能自激。<br>设定 {s}%，有效 100% | The more return, the easier it self-oscillates: with Mix at 50% or more and no loss in the loop, above 100% it always does; gain inside the loop can make it happen lower.<br>Set {s}%, effective 100% | Plus il y a de retour, plus l’auto-oscillation vient facilement : avec le mix à 50 % ou plus et sans atténuation dans la boucle, elle est certaine au-delà de 100 % ; un gain dans la boucle peut la provoquer plus bas.<br>Réglage {s} %, effectif 100 % |
| `toast.over100`<br>`in.hint.zero` | 反馈超过 100% 的轻提示；连接后页脚提醒从 0 开始 | 超过 100%：混合在 50% 及以上时环路会自激，由限制器压住<br>已连接。反馈量从 0 开始，慢慢往上调 | Above 100%: with Mix at 50% or more the loop self-oscillates and the limiter holds it<br>Connected. Feedback starts at 0 — raise it slowly | Au-delà de 100 % : avec le mix à 50 % ou plus, la boucle auto-oscille et le limiteur la contient<br>Connecté. La réinjection démarre à 0 — montez-la lentement |
| `tour.in.step1.title`<br>`tour.in.step1.body` | Input 导览第 1 步 | 欢迎使用 Rinne Input<br>这是回环的返回端，插在通道插入链的第一格：读回环路尾端送回的信号，再和干声混合。所有调节都在这一页，花半分钟认一下这几件东西。 | Welcome to Rinne Input<br>This is the return end of the loop. It sits on the first insert slot, reads back what the end of the loop sends, and mixes it with the dry signal. Every control is on this page — take thirty seconds to meet them. | Bienvenue dans Rinne Input<br>Voici l’extrémité retour de la boucle. Placée dans le premier slot d’insert, elle relit ce qu’envoie la fin de la boucle et le mélange au signal sec. Tous les réglages sont sur cette page — prenez trente secondes pour les découvrir. |
| `tour.in.step2.title`<br>`tour.in.step2.body` | Input 导览第 2 步：组与配对 | 组与配对<br>在你想放进环路的最后一个插件之后插入 Rinne Output（最简单：推子前区最后一格），它会自动加入一个组。灯变绿就是已连接；下面的轨道名与本轨一致，说明两端很可能在同一条轨上。 | Group and pairing<br>Insert Rinne Output after the last plug-in you want in the loop (simplest: the last pre-fader slot); it joins a group by itself. Green means connected; if the track name below matches this one, both ends are most likely on one track. | Groupe et appairage<br>Insérez Rinne Output après le dernier plug-in voulu dans la boucle (le plus simple : dernier slot pré-fader) ; il rejoint un groupe de lui-même. Vert = connecté ; un nom de piste identique ci-dessous indique très probablement la même piste. |
| `tour.in.step3.title`<br>★ `tour.in.step3.body` | Input 导览第 3 步：反馈量 | 反馈量<br>回送的增益，新实例从 0 开始。先调低监听音量再慢慢加：回送越多越容易自激。混合在 50% 及以上、环路里没有衰减时，超过 100%（表盘琥珀段）一定会自激；环路里的插件带增益时，更低也可能自激。 | Feedback<br>The gain of the return; it starts at 0. Turn your monitors down and raise it slowly: the more return, the easier it self-oscillates. With Mix at 50% or more and no loss in the loop, above 100% (the amber arc) it always does; gain inside the loop can make it happen sooner. | Réinjection<br>Le gain du retour ; il démarre à 0. Baissez l’écoute et montez-le lentement : plus de retour, plus vite l’auto-oscillation. Avec le mix à 50 % ou plus et sans perte dans la boucle, elle est certaine au-delà de 100 % (l’arc ambre) ; un gain dans la boucle peut l’avancer. |
| `tour.in.step4.title`<br>`tour.in.step4.body` | Input 导览第 4 步：环路延迟 D | 环路延迟 D<br>信号绕一圈的固定时间，与缓冲大小和导出方式无关，所以播放和导出听起来一样。D 不能小于最小值（本会话见过的最大块长）；设得更小时按最小值运行，并在这里标出实际 D。 | Loop delay D<br>The fixed time for one trip around the loop — the same at any buffer size and in export. It can’t go below the minimum (the largest block this session); set it lower and Rinne runs at the minimum and shows the actual D. | Délai de boucle D<br>Le temps fixe d’un tour de boucle — identique quelle que soit la taille de tampon, et à l’export. Il ne peut pas descendre sous le minimum (le plus grand bloc de cette session) ; réglé plus bas, Rinne fonctionne au minimum et affiche le D réel. |
| `tour.in.step5.title`<br>★ `tour.in.step5.body` | Input 导览第 5 步：安全与紧急切断 | 安全与紧急切断<br>回送路径始终有直流阻断和软削波，限制器和失控保护默认开启。出事先按「紧急切断」：导览和弹窗打开时也能点；恢复按钮在上方的提示条里。 | Safety and Panic<br>The return path always has DC blocking and soft clipping; the limiter and runaway protection are on by default. If anything goes wrong, press Panic — it works even with this tour or a dialog open. Restore return is in the notice bar. | Sécurité et coupure d’urgence<br>Le retour passe toujours par un blocage DC et un écrêtage doux ; limiteur et protection anti-emballement sont actifs par défaut. En cas de problème, appuyez sur Coupure d’urgence, même en pleine visite ou fenêtre ouverte. Rétablir le retour est dans la barre d’avis. |
| `tour.in.step6.title`<br>`tour.in.step6.body` | Input 导览第 6 步：随时重看 | 随时重看<br>想再看一遍使用规则和这段导览，点右下角的问号按钮。点版本号可以查看许可、高级设置和诊断信息。 | See it again<br>Click the “?” button at the bottom right to see the rules and this tour again. Click the version number for licences, advanced settings and diagnostics. | À revoir à tout moment<br>Cliquez sur le bouton « ? » en bas à droite pour revoir les règles et cette visite. Cliquez sur le numéro de version pour les licences, les réglages avancés et les diagnostics. |
| `tour.out.step1.title`<br>`tour.out.step1.body` | Output 导览第 1 步 | 欢迎使用 Rinne Output<br>这是回环的发送端，放在你想放进环路的最后一个插件之后（最简单：推子前区最后一格），把环路尾端的信号送回 Rinne Input。它没有旋钮，全部调节都在 Rinne Input。 | Welcome to Rinne Output<br>The send end of the loop. It sits after the last plug-in you want in the loop (simplest: the last pre-fader slot) and sends the end of the loop back to Rinne Input. It has no knobs — every control is in Rinne Input. | Bienvenue dans Rinne Output<br>L’extrémité envoi de la boucle. Placée après le dernier plug-in voulu dans la boucle (le plus simple : dernier slot pré-fader), elle renvoie la fin de la boucle vers Rinne Input. Elle n’a aucun bouton — tous les réglages sont dans Rinne Input. |
| `tour.out.step2.title`<br>`tour.out.step2.body` | Output 导览第 2 步：组 A–H | 组 A–H<br>新插入的 Rinne Output 会自动加入最近一个未配对的 Rinne Input 所在的组（标「自动」），同名轨优先。不对就点任意组改为手动。 | Group A–H<br>A new Rinne Output joins the group of the most recent unpaired Rinne Input (marked AUTO), preferring one on a track with the same name. If that’s wrong, click any group to set it by hand. | Groupe A–H<br>Un nouveau Rinne Output rejoint le groupe du dernier Rinne Input non appairé (marqué AUTO), de préférence sur une piste de même nom. Si ce n’est pas le bon, cliquez sur un groupe pour le choisir à la main. |
| `tour.out.step3.title`<br>`tour.out.step3.body` | Output 导览第 3 步：配对状态（说明框只有 3 行） | 配对状态<br>灯变绿就是已连接。两行轨道名一致，说明两端很可能在同一条轨上；宿主不给轨道名时显示「无法核验」，不影响使用。 | Pairing<br>A green lamp means connected. Matching track names mean both ends are most likely on one track; if the host gives no names you’ll see “Can’t verify”, which is fine. | Appairage<br>Voyant vert = connecté. Mêmes noms de piste : très probablement la même piste ; si l’hôte n’en donne pas, « Non vérifiable » s’affiche, sans gravité. |
| `tour.out.step4.title`<br>`tour.out.step4.body` | Output 导览第 4 步：随时重看 | 随时重看<br>想再看一遍使用规则和这段导览，点右下角的问号按钮。 | See it again<br>Click the “?” button at the bottom right to see the rules and this tour again. | À revoir à tout moment<br>Cliquez sur le bouton « ? » en bas à droite pour revoir les règles et cette visite. |
| `hint.firstUseRestored` | 恢复出来的实例上，页脚提示本机还没确认过规则 | 本机未确认使用规则：点版本号查看 | Rules not yet confirmed: click the version | Règles à confirmer : cliquez sur la version |
| ★ `common.footer.helpTip` | 页脚问号按钮的悬停提示 | 重看使用规则与引导 | Show the rules and guide again | Revoir les règles et la visite |
| ★ `band.in.takeover`<br>★ `band.action.takeoverTip` | 消息位的接管确认（Output 一侧同文），以及接管按钮的悬停提示 | 组 {g} 被一个已停用的实例占用，要接管吗？<br>接管后那个实例会失去这个组。 | Take over group {g} from an idle instance?<br>That instance will lose the group. | Groupe {g} : instance inactive. Reprendre ?<br>L’autre instance perdra ce groupe. |
| ★ `common.pair.srMismatch`<br>★ `band.in.srMismatch` | 配对状态灯：采样率不一致；Input 消息位同一情形（Output 同文） | 采样率不一致<br>采样率不一致（{a} / {b} Hz）：回送已停止。 | SAMPLE RATE MISMATCH<br>Sample rates differ ({a} / {b} Hz): return stopped. | ÉCHANTILLONNAGE DIFFÉRENT<br>Échantillonnage différent ({a} / {b} Hz) : retour arrêté. |
| ★ `band.in.abi`<br>★ `band.out.abi` | 消息位：两端版本不匹配（状态灯是 VERSION INCOMPATIBLE 的 fr） | 版本不匹配（In {a} / Out {b}），已拒绝。<br>版本不匹配（Out {a} / In {b}），已拒绝。 | Versions differ (In {a} / Out {b}).<br>Versions differ (Out {a} / In {b}). | Versions incompatibles (In {a} / Out {b}).<br>Versions incompatibles (Out {a} / In {b}). |
| ★ `band.in.crossProcess`<br>★ `band.in.crossTrack`<br>★ `band.in.unstable` | 消息位：两端不在同一进程 / 轨道名不同 / 宿主时序不稳（Output 一侧前两条同文） | 两端在不同进程：延迟不保证固定。<br>两端轨道名不同：环路延迟不保证固定。<br>宿主时序不稳定，已切到容忍模式：延迟不再保证固定。 | Different processes: delay may vary.<br>Track names differ: loop delay may vary.<br>Unstable host timing: tolerant mode, delay not fixed. | Autre processus : délai fixe non garanti.<br>Noms de piste différents : délai fixe non garanti.<br>Cadencement instable : mode tolérant, délai fixe non garanti. |
| ★ `band.in.exportViolation`<br>★ `band.in.exportPeer` | 消息位：上次导出可能与播放不同（共四种原因，另两条 exportOverrun、exportResync 只换了原因） | 上次导出可能与播放不同：宿主违约，调大 D 可避免。<br>上次导出可能与播放不同：Output 端宿主违约。 | Last export may differ: oversized blocks; raise D.<br>Last export may differ: the Output host broke its limit. | Dernier export peut différer : gros blocs ; relevez D.<br>Dernier export peut différer : l’hôte d’Output déborde. |
| ★ `band.in.nextStep`<br>★ `band.in.stateDecodeFailed` | 消息位：下一步提示（插入 Output）；工程设置无法读取 | 下一步：插入 Rinne Output（最简单：推子前区最后一格），组 {g}。<br>工程里的 Rinne 设置无法读取：已按默认值运行（反馈量 0）。 | Next: add Rinne Output (last pre-fader slot), group {g}.<br>Settings unreadable: defaults in use (feedback 0). | Ensuite : insérez Rinne Output (dernier slot pré-fader), groupe {g}.<br>Réglages illisibles : valeurs par défaut (réinjection 0). |
| ★ `band.out.ipc`<br>★ `band.out.newerProject` | Output 消息位：无法建立共享内存；工程来自更新版本 | 无法建立共享内存：环路不可用，只直通音频。<br>工程来自更新版本（{a} / {b}）：暂用默认值。 | No shared memory: loop off, pass-through.<br>Project from a newer Rinne ({a} / {b}): defaults. | Pas de mémoire partagée : audio transmis tel quel.<br>Projet plus récent ({a} / {b}) : valeurs par défaut. |
| ★ `in.loop.tip`<br>★ `in.dly.noteTipD`<br>★ `in.lim.ceilingTip` | 环路开关、附点时值、限制器上限的悬停提示 | 关闭时回送淡出，环路内容保留；可被宿主自动化，也可以映射到控制器当作硬件急停。<br>附点：时值的 1.5 倍<br>限制器上限，−24 到 0 dBFS，步进 0.1；预设不会改它 | Off fades the return out without clearing the loop; it’s automatable and can be mapped to a controller as a hardware panic.<br>Dotted: 1.5 × the note value<br>Limiter ceiling, −24 to 0 dBFS in 0.1 steps; presets don’t change it | Désactivée, la boucle fait disparaître le retour en fondu sans vider son contenu ; automatisable, et assignable à un contrôleur comme coupure d’urgence matérielle.<br>Pointée : 1.5 × la valeur de note<br>Plafond du limiteur, de −24 à 0 dBFS par pas de 0.1 ; les préréglages ne le modifient pas |
| ★ `about.adv.tailTip` | 关于面板「高级设置」里 Tail 模式的提示 | 告诉宿主本插件的尾音有多长，防止宿主在静音时挂起插件、把环路断开。自动 = VST3 无限、AU 30 秒。 | Tells the host how long this plug-in’s tail is, so it doesn’t suspend the plug-in during silence and break the loop. Auto = infinite for VST3, 30 s for AU. | Indique à l’hôte la durée de queue (tail) de ce plug-in, pour qu’il ne le suspende pas pendant le silence et n’interrompe pas la boucle. Auto = infini en VST3, 30 s en AU. |
| `common.pair.stale`<br>`common.pair.crossTrack`<br>`common.verify.unknown` | 配对状态灯上的两个短标签；「无法核验」 | 失联<br>不同轨道？<br>无法核验 | LOST<br>OTHER TRACK?<br>Can’t verify | LIAISON PERDUE<br>AUTRE PISTE ?<br>Non vérifiable |
| `in.preset.note`<br>`in.dly.minTip` | 预设面板的说明；环路延迟最小值的提示 | 预设不含环路开关、组号、限制器开关与上限。<br>最小值 = 宿主声明的最大块长与本会话实际见过的最大块长中较大的一个（{n} 采样）；随缓冲设置变化，Cubase 的 ASIO-Guard 会抬高它。 | Presets don’t include Loop, the group, or the limiter switch and ceiling.<br>Minimum = the larger of the host’s declared maximum block and the largest block seen this session ({n} samples); it changes with buffer settings, and Cubase’s ASIO-Guard raises it. | Les préréglages n’incluent pas Boucle, le groupe, ni l’interrupteur et le plafond du limiteur.<br>Minimum = la plus grande valeur entre la taille de bloc maximale déclarée par l’hôte et le plus grand bloc observé durant cette session ({n} échantillons) ; elle varie avec les réglages de tampon, et l’ASIO-Guard de Cubase la relève. |
| `out.pair.sub.inputMuted` | Output 配对卡副行：Input 已静音回送 | Rinne Input 已静音回送（紧急切断、异常值保护或环路关闭）。 | Rinne Input has muted the return (Panic, invalid-value protection or Loop off). | Rinne Input a coupé le retour (coupure d’urgence, protection contre les valeurs invalides ou boucle désactivée). |
| `about.license` | 关于面板的许可短口径（fr 保持英文，见上面的第 3 点） | 源码 GPL-3.0-or-later；二进制整体按 GPLv3 分发，其中 JUCE 部分适用 AGPLv3（GPLv3 §13）。不提供任何担保。 | Source code: GPL-3.0-or-later. The binary as a whole is distributed under GPLv3; its JUCE portion is covered by AGPLv3 (GPLv3 §13). No warranty. | Source code : GPL-3.0-or-later. The binary as a whole is distributed under GPLv3 ; its JUCE portion is covered by AGPLv3 (GPLv3 §13). No warranty. |
| `in.fb.label`<br>`in.mix.endDry`<br>`in.mix.endReturn` | 反馈旋钮名与混合推子两端：Réinjection / SEC / RETOUR（见上面的第 1、2 点） | 反馈量<br>干<br>回送 | Feedback<br>DRY<br>RETURN | Réinjection<br>SEC<br>RETOUR |
