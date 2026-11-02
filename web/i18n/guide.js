// SPDX-License-Identifier: GPL-3.0-or-later
//
// 首启链的界面词条：规则页、两个插件的导览、恢复实例的页脚提示。键前缀有 rules.*、tour.* 和 hint.*。
// 两个插件共用的词条在 common.js（语言选择卡也在那里），键不得重名；由 web/shared/i18n.js 的 mergeDicts 合并。
// 四条规则的引导词与正文（rules.r<N>.lead / body）不在这里手写：真源是 docs/USER_GUIDE.zh-CN.md 的「使用前必读」
// 与 docs/hard-rules.i18n.json，由 gen-docs 的 hard-rules 块写进下面的生成段，改真源后运行 node scripts/gen-docs.mjs --write。
// 其余 fr 为初译，登记在 fr-review/guide.json，发布前抽检；规则的 fr 由 hard-rules.i18n.json 的 frReview 记录。
//
// 排版规则（scripts/check-i18n.mjs 检查）：
//   - zh 用全角标点，en 用弯引号和弯撇号；
//   - fr 在 ; : ? ! % 之前、« » 的内侧用 U+00A0 或 U+202F，撇号用 ’。

// <!-- BEGIN gen:hard-rules:guide -->
/** 规则页的引导词与正文（生成段）：按规则编号排列，文件下方摊成 rules.r<N>.lead / body 词条。 */
const RULE_TEXT = {
    zh: [
        {
            lead: "放置位置",
            body: "Rinne Input 插在通道插入链的第一格；Rinne Output 插在你想放进环路的最后一个插件之后、推子分隔线之上（最简单：推子前区最后一格；宿主没有推子前区时，插在插入链最后一格）。两者之间的插件，包括宿主自带的插入效果器，都在环路里，它们自身的延迟会叠加到环路延迟 D 上；Output 之后的插件、推子、声像和发送都不在环路里。",
        },
        {
            lead: "从 0 开始",
            body: "新实例的反馈量是 0，加载工程或预设时回送从 0 淡入。先调低监听音量，再从 0 慢慢往上调：回送越多越容易自激。混合在 50% 及以上、环路里没有衰减时，反馈超过 100% 一定会自激；环路里的插件带增益（EQ 提升、压缩补偿、饱和）时，更低也可能自激。限制器和失控保护默认开启，但挡不住长时间的高电平。",
        },
        {
            lead: "出事先切断",
            body: "听到啸叫或异常，先按「紧急切断」：它只切断回送，干声照常通过；恢复请点提示条里的「恢复回送」。窗口关着时，最快的办法是在宿主里旁路 Rinne Input。推子不在环路里，只拉推子只是听不见，环路仍在增长；停止播放和静音也停不住自激。",
        },
        {
            lead: "一组一对",
            body: "一个组（A–H）只放一个 Rinne Input 和一个 Rinne Output，两端组号一致才会连接。复制轨道会连组号一起复制：副本会显示「冲突」，在副本的 Rinne Input 上点「移到空闲组」，再在副本的 Rinne Output 上点「重新配对」。",
        },
    ],
    en: [
        {
            lead: "Placement",
            body: "Put Rinne Input on the first insert slot of the channel. Put Rinne Output after the last plug-in you want in the loop and above the fader divider (simplest: the last pre-fader slot; if your host has no pre-fader section, the last insert slot). Every plug-in between them — including the host’s own insert effects — is inside the loop, and its own latency adds to the loop delay D; plug-ins after Output, the fader, pan and sends are not.",
        },
        {
            lead: "Start from 0",
            body: "A new instance starts at 0% feedback, and the return fades in from 0 when a project or preset loads. Turn your monitors down, then raise feedback slowly from 0: the more return, the easier the loop self-oscillates. With Mix at 50% or more and nothing attenuating in the loop, feedback above 100% always self-oscillates; gain-adding plug-ins in the loop (EQ boosts, compressor make-up, saturation) can make it happen lower. The limiter and runaway protection are on by default, but they can’t protect you from sustained loud levels.",
        },
        {
            lead: "Cut it first",
            body: "If you hear howling or anything odd, press Panic first: it cuts only the return, and the dry signal keeps playing; restore it with Restore return in the notice bar. With the window closed, the fastest way is to bypass Rinne Input in the host. The fader isn’t in the loop — pulling it down only hides the sound while the loop keeps growing, and stopping playback or muting won’t stop self-oscillation either.",
        },
        {
            lead: "One pair per group",
            body: "A group (A–H) holds one Rinne Input and one Rinne Output, and they connect only when their groups match. Duplicating a track copies its group too: the copy shows CONFLICT — click Move to free group on the copy’s Rinne Input, then Re-pair on the copy’s Rinne Output.",
        },
    ],
    fr: [
        {
            lead: "Placement",
            body: "Placez Rinne Input dans le premier slot d’insert de la voie. Placez Rinne Output après le dernier plug-in voulu dans la boucle et au-dessus de la séparation du fader (le plus simple : dernier slot pré-fader ; si votre hôte n’a pas de section pré-fader, dernier slot d’insert). Tout plug-in situé entre les deux — y compris les effets d’insert propres à l’hôte — est dans la boucle, et sa propre latence s’ajoute au délai de boucle D ; les plug-ins après Output, le fader, le panoramique et les envois n’y sont pas.",
        },
        {
            lead: "Partir de 0",
            body: "Une nouvelle instance démarre à 0 % de réinjection, et le retour revient en fondu depuis 0 au chargement d’un projet ou d’un préréglage. Baissez le volume d’écoute, puis montez lentement la réinjection depuis 0 : plus il y a de retour, plus la boucle entre facilement en auto-oscillation. Avec le mix à 50 % ou plus et rien qui atténue dans la boucle, une réinjection au-delà de 100 % entre toujours en auto-oscillation ; des plug-ins ajoutant du gain dans la boucle (boost d’EQ, compensation de compresseur, saturation) peuvent la provoquer plus bas. Le limiteur et la protection anti-emballement sont activés par défaut, mais ils ne vous protègent pas contre des niveaux élevés prolongés.",
        },
        {
            lead: "Couper d’abord",
            body: "Si vous entendez un larsen ou quoi que ce soit d’anormal, appuyez d’abord sur Coupure d’urgence : elle ne coupe que le retour, le signal sec continue de passer ; rétablissez-le avec Rétablir le retour dans la barre d’avis. Fenêtre fermée, le plus rapide est de contourner Rinne Input dans l’hôte. Le fader n’est pas dans la boucle — le baisser ne fait que masquer le son pendant que la boucle continue de croître, et arrêter la lecture ou couper le son n’arrête pas non plus l’auto-oscillation.",
        },
        {
            lead: "Une paire par groupe",
            body: "Un groupe (A–H) contient un Rinne Input et un Rinne Output, et ils ne se connectent que si leurs groupes correspondent. Dupliquer une piste copie aussi son groupe : la copie affiche CONFLIT — cliquez sur « Vers un groupe libre » dans le Rinne Input de la copie, puis sur « Réappairer » dans son Rinne Output.",
        },
    ],
};
/* 生成内容到此为止，由 scripts/gen-docs.mjs 的 hard-rules 块写出，不要手改。
<!-- END gen:hard-rules:guide --> */

/** 生成段摊成词条：第 N 条规则的引导词与正文是 rules.rN.lead、rules.rN.body。 */
function ruleStrings(lang) {
    const out = {};
    RULE_TEXT[lang].forEach((r, i) => {
        out[`rules.r${i + 1}.lead`] = r.lead;
        out[`rules.r${i + 1}.body`] = r.body;
    });
    return out;
}

export default {
    zh: {
        ...ruleStrings("zh"),
        // 规则页：标题、按钮与脚注（四条规则的引导词与正文在上面的生成段）
        "rules.title": "使用 Rinne 前必读：下面四条决定环路能否闭合，以及会不会伤到耳朵和音箱。",
        "rules.btn.start": "我已了解，开始导览",
        "rules.btn.skip": "跳过导览",
        "rules.btn.continue": "继续导览",
        "rules.btn.ack": "我已了解",
        "rules.readFirst": "请先读到最后一条",
        "rules.footnote": "以后可点问号按钮或在「关于」里重新查看",
        // 导览：说明框的公共部分
        "tour.common.clickAnywhere": "左键点击任意处继续",
        "tour.common.skip": "跳过",
        "tour.common.prev": "上一步",
        "tour.common.next": "下一步",
        "tour.common.done": "完成",
        "tour.common.step": "{i}/{n}",
        // Input 导览 6 步
        "tour.in.step1.title": "欢迎使用 Rinne Input",
        "tour.in.step1.body":
            "这是回环的返回端，插在通道插入链的第一格：读回环路尾端送回的信号，再和干声混合。所有调节都在这一页，花半分钟认一下这几件东西。",
        "tour.in.step2.title": "组与配对",
        "tour.in.step2.body":
            "在你想放进环路的最后一个插件之后插入 Rinne Output（最简单：推子前区最后一格），它会自动加入一个组。灯变绿就是已连接；下面的轨道名与本轨一致，说明两端很可能在同一条轨上。",
        "tour.in.step3.title": "反馈量",
        "tour.in.step3.body":
            "回送的增益，新实例从 0 开始。先调低监听音量再慢慢加：回送越多越容易自激。混合在 50% 及以上、环路里没有衰减时，超过 100%（表盘琥珀段）一定会自激；环路里的插件带增益时，更低也可能自激。",
        "tour.in.step4.title": "环路延迟 D",
        "tour.in.step4.body":
            "信号绕一圈的固定时间，与缓冲大小和导出方式无关，所以播放和导出听起来一样。D 不能小于最小值（本会话见过的最大块长）；设得更小时按最小值运行，并在这里标出实际 D。",
        "tour.in.step5.title": "安全与紧急切断",
        "tour.in.step5.body":
            "回送路径始终有直流阻断和软削波，限制器和失控保护默认开启。出事先按「紧急切断」：导览和弹窗打开时也能点；恢复按钮在上方的提示条里。",
        "tour.in.step6.title": "随时重看",
        "tour.in.step6.body":
            "想再看一遍使用规则和这段导览，点右下角的问号按钮。点版本号可以查看许可、高级设置和诊断信息。",
        // Output 导览 4 步
        "tour.out.step1.title": "欢迎使用 Rinne Output",
        "tour.out.step1.body":
            "这是回环的发送端，放在你想放进环路的最后一个插件之后（最简单：推子前区最后一格），把环路尾端的信号送回 Rinne Input。它没有旋钮，全部调节都在 Rinne Input。",
        "tour.out.step2.title": "组 A–H",
        "tour.out.step2.body":
            "新插入的 Rinne Output 会自动加入最近一个未配对的 Rinne Input 所在的组（标「自动」），同名轨优先。不对就点任意组改为手动。",
        "tour.out.step3.title": "配对状态",
        "tour.out.step3.body":
            "灯变绿就是已连接。两行轨道名一致，说明两端很可能在同一条轨上；宿主不给轨道名时显示「无法核验」，不影响使用。",
        "tour.out.step4.title": "随时重看",
        "tour.out.step4.body": "想再看一遍使用规则和这段导览，点右下角的问号按钮。",
        // 恢复的实例（新机器上打开旧工程）：本机没确认过规则时的页脚提示
        "hint.firstUseRestored": "本机未确认使用规则：点版本号查看",
    },
    en: {
        ...ruleStrings("en"),
        // 规则页：标题、按钮与脚注（四条规则的引导词与正文在上面的生成段）
        "rules.title":
            "Read this before using Rinne: these four rules decide whether the loop closes — and whether it can hurt your ears or speakers.",
        "rules.btn.start": "Got it, start the tour",
        "rules.btn.skip": "Skip the tour",
        "rules.btn.continue": "Continue to the tour",
        "rules.btn.ack": "Got it",
        "rules.readFirst": "Read to the last rule first",
        "rules.footnote": "You can reread these from the “?” button or About",
        // 导览：说明框的公共部分
        "tour.common.clickAnywhere": "Left-click anywhere to continue",
        "tour.common.skip": "Skip",
        "tour.common.prev": "Back",
        "tour.common.next": "Next",
        "tour.common.done": "Done",
        "tour.common.step": "{i}/{n}",
        // Input 导览 6 步
        "tour.in.step1.title": "Welcome to Rinne Input",
        "tour.in.step1.body":
            "This is the return end of the loop. It sits on the first insert slot, reads back what the end of the loop sends, and mixes it with the dry signal. Every control is on this page — take thirty seconds to meet them.",
        "tour.in.step2.title": "Group and pairing",
        "tour.in.step2.body":
            "Insert Rinne Output after the last plug-in you want in the loop (simplest: the last pre-fader slot); it joins a group by itself. Green means connected; if the track name below matches this one, both ends are most likely on one track.",
        "tour.in.step3.title": "Feedback",
        "tour.in.step3.body":
            "The gain of the return; it starts at 0. Turn your monitors down and raise it slowly: the more return, the easier it self-oscillates. With Mix at 50% or more and no loss in the loop, above 100% (the amber arc) it always does; gain inside the loop can make it happen sooner.",
        "tour.in.step4.title": "Loop delay D",
        "tour.in.step4.body":
            "The fixed time for one trip around the loop — the same at any buffer size and in export. It can’t go below the minimum (the largest block this session); set it lower and Rinne runs at the minimum and shows the actual D.",
        "tour.in.step5.title": "Safety and Panic",
        "tour.in.step5.body":
            "The return path always has DC blocking and soft clipping; the limiter and runaway protection are on by default. If anything goes wrong, press Panic — it works even with this tour or a dialog open. Restore return is in the notice bar.",
        "tour.in.step6.title": "See it again",
        "tour.in.step6.body":
            "Click the “?” button at the bottom right to see the rules and this tour again. Click the version number for licences, advanced settings and diagnostics.",
        // Output 导览 4 步
        "tour.out.step1.title": "Welcome to Rinne Output",
        "tour.out.step1.body":
            "The send end of the loop. It sits after the last plug-in you want in the loop (simplest: the last pre-fader slot) and sends the end of the loop back to Rinne Input. It has no knobs — every control is in Rinne Input.",
        "tour.out.step2.title": "Group A–H",
        "tour.out.step2.body":
            "A new Rinne Output joins the group of the most recent unpaired Rinne Input (marked AUTO), preferring one on a track with the same name. If that’s wrong, click any group to set it by hand.",
        "tour.out.step3.title": "Pairing",
        "tour.out.step3.body":
            "A green lamp means connected. Matching track names mean both ends are most likely on one track; if the host gives no names you’ll see “Can’t verify”, which is fine.",
        "tour.out.step4.title": "See it again",
        "tour.out.step4.body": "Click the “?” button at the bottom right to see the rules and this tour again.",
        // 恢复的实例（新机器上打开旧工程）：本机没确认过规则时的页脚提示
        "hint.firstUseRestored": "Rules not yet confirmed: click the version",
    },
    fr: {
        ...ruleStrings("fr"),
        // 规则页：标题、按钮与脚注（四条规则的引导词与正文在上面的生成段）
        "rules.title":
            "À lire avant d’utiliser Rinne : ces quatre règles déterminent si la boucle se ferme — et si elle peut abîmer vos oreilles ou vos enceintes.",
        "rules.btn.start": "Compris, lancer la visite",
        "rules.btn.skip": "Passer la visite",
        "rules.btn.continue": "Continuer la visite",
        "rules.btn.ack": "Compris",
        "rules.readFirst": "Lisez d’abord jusqu’à la dernière règle",
        "rules.footnote": "Vous pourrez les relire via le bouton « ? » ou À propos",
        // 导览：说明框的公共部分
        "tour.common.clickAnywhere": "Clic gauche n’importe où pour continuer",
        "tour.common.skip": "Passer",
        "tour.common.prev": "Précédent",
        "tour.common.next": "Suivant",
        "tour.common.done": "Terminé",
        "tour.common.step": "{i}/{n}",
        // Input 导览 6 步
        "tour.in.step1.title": "Bienvenue dans Rinne Input",
        "tour.in.step1.body":
            "Voici l’extrémité retour de la boucle. Placée dans le premier slot d’insert, elle relit ce qu’envoie la fin de la boucle et le mélange au signal sec. Tous les réglages sont sur cette page — prenez trente secondes pour les découvrir.",
        "tour.in.step2.title": "Groupe et appairage",
        "tour.in.step2.body":
            "Insérez Rinne Output après le dernier plug-in voulu dans la boucle (le plus simple : dernier slot pré-fader) ; il rejoint un groupe de lui-même. Vert = connecté ; un nom de piste identique ci-dessous indique très probablement la même piste.",
        "tour.in.step3.title": "Réinjection",
        "tour.in.step3.body":
            "Le gain du retour ; il démarre à 0. Baissez l’écoute et montez-le lentement : plus de retour, plus vite l’auto-oscillation. Avec le mix à 50 % ou plus et sans perte dans la boucle, elle est certaine au-delà de 100 % (l’arc ambre) ; un gain dans la boucle l’avance.",
        "tour.in.step4.title": "Délai de boucle D",
        "tour.in.step4.body":
            "Le temps fixe d’un tour de boucle — identique quelle que soit la taille de tampon, et à l’export. Il ne peut pas descendre sous le minimum (le plus grand bloc de cette session) ; réglé plus bas, Rinne fonctionne au minimum et affiche le D réel.",
        "tour.in.step5.title": "Sécurité et coupure d’urgence",
        "tour.in.step5.body":
            "Le retour passe toujours par un blocage DC et un écrêtage doux ; limiteur et protection anti-emballement sont actifs par défaut. En cas de souci, appuyez sur Coupure d’urgence, même visite ouverte. Rétablir le retour est dans la barre d’avis.",
        "tour.in.step6.title": "À revoir à tout moment",
        "tour.in.step6.body":
            "Cliquez sur le bouton « ? » en bas à droite pour revoir les règles et cette visite. Cliquez sur le numéro de version pour les licences, les réglages avancés et les diagnostics.",
        // Output 导览 4 步
        "tour.out.step1.title": "Bienvenue dans Rinne Output",
        "tour.out.step1.body":
            "L’extrémité envoi de la boucle. Placée après le dernier plug-in voulu dans la boucle (le plus simple : dernier slot pré-fader), elle renvoie la fin de la boucle vers Rinne Input. Elle n’a aucun bouton — tous les réglages sont dans Rinne Input.",
        "tour.out.step2.title": "Groupe A–H",
        "tour.out.step2.body":
            "Un nouveau Rinne Output rejoint le groupe du dernier Rinne Input non appairé (marqué AUTO), de préférence sur une piste de même nom. Si ce n’est pas le bon, cliquez sur un groupe pour le choisir à la main.",
        "tour.out.step3.title": "Appairage",
        "tour.out.step3.body":
            "Voyant vert = connecté. Des noms de piste identiques indiquent très probablement la même piste ; sans noms de l’hôte, « Vérification impossible » s’affiche, sans conséquence.",
        "tour.out.step4.title": "À revoir à tout moment",
        "tour.out.step4.body": "Cliquez sur le bouton « ? » en bas à droite pour revoir les règles et cette visite.",
        // 恢复的实例（新机器上打开旧工程）：本机没确认过规则时的页脚提示
        "hint.firstUseRestored": "Règles à confirmer : cliquez sur la version",
    },
};
