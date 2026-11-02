// <!-- BEGIN gen:hard-rules:web -->
// SPDX-License-Identifier: GPL-3.0-or-later
//
// 硬约束的界面数据（生成物）：真源是 docs/USER_GUIDE.zh-CN.md 的「使用前必读」与 docs/hard-rules.i18n.json，
// 由 scripts/gen-docs.mjs 的 hard-rules 块整份写出；改真源后运行 node scripts/gen-docs.mjs --write。
// 这个文件的任何一个字节被手改，gen-docs --check 都会判红。

/** 规则页的版本：用户确认过的版本号存本机全局位，与它不等时新实例重弹规则页。 */
export const RULES_VERSION = 1;

/** 规则按编号排列：id 与三语的引导词、正文。 */
export const RULES = [
    {
        id: "placement",
        zh: {
            lead: "放置位置",
            body: "Rinne Input 插在通道插入链的第一格；Rinne Output 插在你想放进环路的最后一个插件之后、推子分隔线之上（最简单：推子前区最后一格；宿主没有推子前区时，插在插入链最后一格）。两者之间的插件，包括宿主自带的插入效果器，都在环路里，它们自身的延迟会叠加到环路延迟 D 上；Output 之后的插件、推子、声像和发送都不在环路里。",
        },
        en: {
            lead: "Placement",
            body: "Put Rinne Input on the first insert slot of the channel. Put Rinne Output after the last plug-in you want in the loop and above the fader divider (simplest: the last pre-fader slot; if your host has no pre-fader section, the last insert slot). Every plug-in between them — including the host’s own insert effects — is inside the loop, and its own latency adds to the loop delay D; plug-ins after Output, the fader, pan and sends are not.",
        },
        fr: {
            lead: "Placement",
            body: "Placez Rinne Input dans le premier slot d’insert de la voie. Placez Rinne Output après le dernier plug-in voulu dans la boucle et au-dessus de la séparation du fader (le plus simple : dernier slot pré-fader ; si votre hôte n’a pas de section pré-fader, dernier slot d’insert). Tout plug-in situé entre les deux — y compris les effets d’insert propres à l’hôte — est dans la boucle, et sa propre latence s’ajoute au délai de boucle D ; les plug-ins après Output, le fader, le panoramique et les envois n’y sont pas.",
        },
    },
    {
        id: "zero",
        zh: {
            lead: "从 0 开始",
            body: "新实例的反馈量是 0，加载工程或预设时回送从 0 淡入。先调低监听音量，再从 0 慢慢往上调：回送越多越容易自激。混合在 50% 及以上、环路里没有衰减时，反馈超过 100% 一定会自激；环路里的插件带增益（EQ 提升、压缩补偿、饱和）时，更低也可能自激。限制器和失控保护默认开启，但挡不住长时间的高电平。",
        },
        en: {
            lead: "Start from 0",
            body: "A new instance starts at 0% feedback, and the return fades in from 0 when a project or preset loads. Turn your monitors down, then raise feedback slowly from 0: the more return, the easier the loop self-oscillates. With Mix at 50% or more and nothing attenuating in the loop, feedback above 100% always self-oscillates; gain-adding plug-ins in the loop (EQ boosts, compressor make-up, saturation) can make it happen lower. The limiter and runaway protection are on by default, but they can’t protect you from sustained loud levels.",
        },
        fr: {
            lead: "Partir de 0",
            body: "Une nouvelle instance démarre à 0 % de réinjection, et le retour revient en fondu depuis 0 au chargement d’un projet ou d’un préréglage. Baissez le volume d’écoute, puis montez lentement la réinjection depuis 0 : plus il y a de retour, plus la boucle entre facilement en auto-oscillation. Avec le mix à 50 % ou plus et rien qui atténue dans la boucle, une réinjection au-delà de 100 % entre toujours en auto-oscillation ; des plug-ins ajoutant du gain dans la boucle (boost d’EQ, compensation de compresseur, saturation) peuvent la provoquer plus bas. Le limiteur et la protection anti-emballement sont activés par défaut, mais ils ne vous protègent pas contre des niveaux élevés prolongés.",
        },
    },
    {
        id: "panic",
        zh: {
            lead: "出事先切断",
            body: "听到啸叫或异常，先按「紧急切断」：它只切断回送，干声照常通过；恢复请点提示条里的「恢复回送」。窗口关着时，最快的办法是在宿主里旁路 Rinne Input。推子不在环路里，只拉推子只是听不见，环路仍在增长；停止播放和静音也停不住自激。",
        },
        en: {
            lead: "Cut it first",
            body: "If you hear howling or anything odd, press Panic first: it cuts only the return, and the dry signal keeps playing; restore it with Restore return in the notice bar. With the window closed, the fastest way is to bypass Rinne Input in the host. The fader isn’t in the loop — pulling it down only hides the sound while the loop keeps growing, and stopping playback or muting won’t stop self-oscillation either.",
        },
        fr: {
            lead: "Couper d’abord",
            body: "Si vous entendez un larsen ou quoi que ce soit d’anormal, appuyez d’abord sur Coupure d’urgence : elle ne coupe que le retour, le signal sec continue de passer ; rétablissez-le avec Rétablir le retour dans la barre d’avis. Fenêtre fermée, le plus rapide est de contourner Rinne Input dans l’hôte. Le fader n’est pas dans la boucle — le baisser ne fait que masquer le son pendant que la boucle continue de croître, et arrêter la lecture ou couper le son n’arrête pas non plus l’auto-oscillation.",
        },
    },
    {
        id: "pairing",
        zh: {
            lead: "一组一对",
            body: "一个组（A–H）只放一个 Rinne Input 和一个 Rinne Output，两端组号一致才会连接。复制轨道会连组号一起复制：副本会显示「冲突」，在副本的 Rinne Input 上点「移到空闲组」，再在副本的 Rinne Output 上点「重新配对」。",
        },
        en: {
            lead: "One pair per group",
            body: "A group (A–H) holds one Rinne Input and one Rinne Output, and they connect only when their groups match. Duplicating a track copies its group too: the copy shows CONFLICT — click Move to free group on the copy’s Rinne Input, then Re-pair on the copy’s Rinne Output.",
        },
        fr: {
            lead: "Une paire par groupe",
            body: "Un groupe (A–H) contient un Rinne Input et un Rinne Output, et ils ne se connectent que si leurs groupes correspondent. Dupliquer une piste copie aussi son groupe : la copie affiche CONFLIT — cliquez sur « Vers un groupe libre » dans le Rinne Input de la copie, puis sur « Réappairer » dans son Rinne Output.",
        },
    },
];
/* 生成内容到此为止，由 scripts/gen-docs.mjs 的 hard-rules 块写出，不要手改。
<!-- END gen:hard-rules:web --> */
