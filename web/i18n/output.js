// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Output 页面自己的界面词条：页眉、常驻位置提示、组徽标说明、配对卡、发送状态行、页脚提示、
// 消息位的 14 条消息与接管确认条。键前缀有 out.* 和 band.out.*。
// 两个插件共用的词条在 common.js，键不得重名；由 web/shared/i18n.js 的 mergeDicts 合并。
// fr 为初译，登记在 fr-review/output.json，发布前抽检。
//
// 排版规则（scripts/check-i18n.mjs 检查）：
//   - zh 用全角标点，en 用弯引号和弯撇号；
//   - fr 在 ; : ? ! % 之前、« » 的内侧用 U+00A0 或 U+202F（源码里写成转义，肉眼才看得出），撇号用 ’；
//   - 界面文字里不出现「自动配对」；中文里业内通用的英文说法（Input、Output、tail 一类）保留英文。
export default {
    zh: {
        // 页眉与常驻位置提示
        "out.brand.title": "Rinne Output",
        "out.brand.eyebrow": "SYNCHAIN · 回环 · 链尾",
        "out.place": "放在你想放进环路的最后一个插件之后、推子分隔线之上（最简单：推子前区最后一格）。",
        // 组选择的徽标说明
        "out.group.autoTip": "已自动跟随最近一个未配对的 Rinne Input（组 {g}）；点任意组改为手动。",
        "out.group.autoFallback": "没有未配对的 Rinne Input，暂用组 {g}，每秒重新查找。",
        "out.group.peerDotTip": "组 {g}：有一个未配对的 Rinne Input 在线",
        // 配对卡：状态灯、副行、两条轨道行
        "out.pair.searching": "未找到 Input",
        "out.pair.bypassed": "已旁路",
        "out.pair.eyebrow": "配对",
        "out.pair.sub.searching": "在本轨插入链第一格插入 Rinne Input，并选同一个组。",
        "out.pair.sub.connected": "正在把环路尾端的信号送回本轨第一格的 Rinne Input。",
        "out.pair.sub.connectedUnverified": "已连接，但宿主没有提供轨道名，无法核验两端在同一条轨上；不影响使用。",
        "out.pair.sub.inputMuted": "Rinne Input 已静音回送（紧急切断、异常值保护或环路关闭）。",
        "out.pair.sub.stale": "Rinne Input 没有响应：本插件照常发送。",
        "out.pair.sub.bypassed": "宿主旁路了本插件：正在发送静音，回送为零。",
        "out.pair.sub.notice": "详见下方提示。",
        "out.pair.thisTrack": "本轨",
        "out.pair.inputTrack": "Input 所在轨",
        "out.pair.noName": "（未提供轨道名）",
        // 发送状态行
        "out.send.ok": "发送中 · {ch} · {sr} kHz",
        "out.send.bypassed": "旁路：写入静音",
        "out.send.idle": "宿主未调用处理：暂停写入",
        "out.send.none": "—",
        // 页脚提示与消息位空闲灰字
        "out.hint.idle": "反馈量、延迟、音色和安全设置都在本轨的 Rinne Input 里。",
        "out.hint.unpaired": "先在本轨插入链第一格插入 Rinne Input",
        "out.hint.connected": "全部调节都在 Rinne Input",
        "out.hint.conflict": "换一个组，或删掉多出来的那个",
        // 消息位：Output 的 14 条消息与接管确认条
        "band.out.conflict": "组 {g} 已有另一个 Output：本实例只直通。",
        "band.out.unassigned": "8 个组都已占用：删掉或改组一对，空出一个组。",
        "band.out.abi": "版本不匹配（Out {a} / In {b}），已拒绝。",
        "band.out.srMismatch": "采样率不一致（{a} / {b} Hz）：回送已停止。",
        "band.out.ipc": "无法建立共享内存：环路不可用，只直通音频。",
        "band.out.ipcSandbox": "宿主沙盒禁止共享内存：暂不支持此宿主。",
        "band.out.newerProject": "工程来自更新版本（{a} / {b}）：暂用默认值。",
        "band.out.stateDecodeFailed": "工程里的 Rinne 设置无法读取：已按默认值运行。",
        "band.out.stale": "Rinne Input 超过 2 秒没有响应：本插件照常发送。",
        "band.out.crossProcess": "两端在不同进程：延迟不保证固定。",
        "band.out.crossTrack": "两端轨道名不同：延迟不保证固定。",
        "band.out.outOfOrder": "插件顺序反了：把本插件移到环路最后一个插件之后。",
        "band.out.waitingAudio": "等待宿主送来音频：播放后自动选组，也可手动选组。",
        "band.out.validator": "在插件验证工具中运行：环路关闭，只直通音频。",
        "band.out.takeover": "组 {g} 被一个已停用的实例占用，要接管吗？",
    },
    en: {
        // 页眉与常驻位置提示
        "out.brand.title": "Rinne Output",
        "out.brand.eyebrow": "SYNCHAIN · RINNE · CHAIN END",
        "out.place":
            "Put this after the last plug-in you want in the loop and above the fader divider (simplest: the last pre-fader slot).",
        // 组选择的徽标说明
        "out.group.autoTip":
            "Following the most recent unpaired Rinne Input (group {g}); click any group to choose by hand.",
        "out.group.autoFallback":
            "No unpaired Rinne Input yet; using group {g} for now and checking again every second.",
        "out.group.peerDotTip": "Group {g}: an unpaired Rinne Input is online",
        // 配对卡：状态灯、副行、两条轨道行
        "out.pair.searching": "NO INPUT",
        "out.pair.bypassed": "BYPASSED",
        "out.pair.eyebrow": "PAIRING",
        "out.pair.sub.searching":
            "Insert Rinne Input on the first insert slot of this track and choose the same group.",
        "out.pair.sub.connected": "Sending the end of the loop back to Rinne Input on this track’s first insert slot.",
        "out.pair.sub.connectedUnverified":
            "Connected, but the host gives no track names, so Rinne can’t check that both ends share a track; this doesn’t affect use.",
        "out.pair.sub.inputMuted": "Rinne Input has muted the return (Panic, invalid-value protection or Loop off).",
        "out.pair.sub.stale": "Rinne Input isn’t responding; this plug-in keeps sending.",
        "out.pair.sub.bypassed": "The host has bypassed this plug-in: it sends silence, so the return is silent.",
        "out.pair.sub.notice": "See the notice below.",
        "out.pair.thisTrack": "This track",
        "out.pair.inputTrack": "Input’s track",
        "out.pair.noName": "(no track name)",
        // 发送状态行
        "out.send.ok": "SENDING · {ch} · {sr} kHz",
        "out.send.bypassed": "BYPASSED: WRITING SILENCE",
        "out.send.idle": "HOST NOT PROCESSING: PAUSED",
        "out.send.none": "—",
        // 页脚提示与消息位空闲灰字
        "out.hint.idle": "Feedback, delay, tone and safety settings all live in the Rinne Input on this track.",
        "out.hint.unpaired": "First insert Rinne Input on this track’s first insert slot",
        "out.hint.connected": "All controls are in Rinne Input",
        "out.hint.conflict": "Pick another group, or remove the extra one",
        // 消息位：Output 的 14 条消息与接管确认条
        "band.out.conflict": "Group {g} taken by another Output.",
        "band.out.unassigned": "All 8 groups taken: remove or regroup a pair.",
        "band.out.abi": "Versions differ (Out {a} / In {b}).",
        "band.out.srMismatch": "Sample rates differ ({a} / {b} Hz): stopped.",
        "band.out.ipc": "No shared memory: loop off, pass-through.",
        "band.out.ipcSandbox": "Sandboxed host: not supported yet.",
        "band.out.newerProject": "Project from a newer Rinne ({a} / {b}): defaults.",
        "band.out.stateDecodeFailed": "Settings unreadable: defaults in use.",
        "band.out.stale": "Rinne Input silent for 2 s; still sending.",
        "band.out.crossProcess": "Different processes: delay may vary.",
        "band.out.crossTrack": "Track names differ: delay may vary.",
        "band.out.outOfOrder": "Order reversed: move this plug-in later.",
        "band.out.waitingAudio": "Waiting for host audio: play to auto-pick a group.",
        "band.out.validator": "Running in a validator: loop off, pass-through.",
        "band.out.takeover": "Take over group {g} from an idle instance?",
    },
    fr: {
        // 页眉与常驻位置提示
        "out.brand.title": "Rinne Output",
        "out.brand.eyebrow": "SYNCHAIN · RINNE · FIN CHAÎNE",
        "out.place":
            "Placez-le après le dernier plug-in voulu dans la boucle, au-dessus de la séparation du fader (le plus simple : dernier slot pré-fader).",
        // 组选择的徽标说明
        "out.group.autoTip":
            "Suit le dernier Rinne Input non appairé (groupe {g}) ; cliquez sur un groupe pour choisir à la main.",
        "out.group.autoFallback":
            "Aucun Rinne Input non appairé pour l’instant ; groupe {g} utilisé provisoirement, nouvelle recherche chaque seconde.",
        "out.group.peerDotTip": "Groupe {g} : un Rinne Input non appairé est en ligne",
        // 配对卡：状态灯、副行、两条轨道行
        "out.pair.searching": "AUCUN INPUT",
        "out.pair.bypassed": "CONTOURNÉ",
        "out.pair.eyebrow": "APPAIRAGE",
        "out.pair.sub.searching":
            "Insérez Rinne Input dans le premier slot d’insert de cette piste et choisissez le même groupe.",
        "out.pair.sub.connected":
            "Le signal de fin de boucle est renvoyé vers Rinne Input, dans le premier slot d’insert de cette piste.",
        "out.pair.sub.connectedUnverified":
            "Connecté, mais sans noms de piste de l’hôte, Rinne ne peut pas vérifier que les deux extrémités sont sur la même piste ; sans incidence.",
        "out.pair.sub.inputMuted":
            "Rinne Input a coupé le retour (coupure d’urgence, protection contre les valeurs invalides ou boucle désactivée).",
        "out.pair.sub.stale": "Rinne Input ne répond pas ; ce plug-in continue d’émettre.",
        "out.pair.sub.bypassed": "L’hôte a contourné ce plug-in : il émet du silence, le retour est donc muet.",
        "out.pair.sub.notice": "Voir l’avis ci-dessous.",
        "out.pair.thisTrack": "Cette piste",
        "out.pair.inputTrack": "Piste d’Input",
        "out.pair.noName": "(sans nom de piste)",
        // 发送状态行
        "out.send.ok": "ENVOI · {ch} · {sr} kHz",
        "out.send.bypassed": "CONTOURNÉ : ÉCRIT DU SILENCE",
        "out.send.idle": "HÔTE INACTIF : EN PAUSE",
        "out.send.none": "—",
        // 页脚提示与消息位空闲灰字
        "out.hint.idle":
            "La réinjection, le délai, le timbre et la sécurité se règlent tous dans le Rinne Input de cette piste.",
        "out.hint.unpaired": "Insérez d’abord Rinne Input en premier slot d’insert",
        "out.hint.connected": "Tous les réglages sont dans Rinne Input",
        "out.hint.conflict": "Changez de groupe, ou retirez l’instance en trop",
        // 消息位：Output 的 14 条消息与接管确认条
        "band.out.conflict": "Groupe {g} pris par un autre Output.",
        "band.out.unassigned": "Les 8 groupes sont pris : libérez-en un.",
        "band.out.abi": "Versions différentes (Out {a} / In {b}).",
        "band.out.srMismatch": "Fréquences différentes ({a} / {b} Hz) : arrêt.",
        "band.out.ipc": "Pas de mémoire partagée : audio direct seul.",
        "band.out.ipcSandbox": "Hôte en bac à sable : non pris en charge.",
        "band.out.newerProject": "Projet plus récent ({a} / {b}) : défauts.",
        "band.out.stateDecodeFailed": "Réglages illisibles : valeurs par défaut.",
        "band.out.stale": "Rinne Input muet depuis 2 s ; envoi maintenu.",
        "band.out.crossProcess": "Processus différents : délai variable.",
        "band.out.crossTrack": "Noms de piste différents : délai variable.",
        "band.out.outOfOrder": "Ordre inversé : déplacez ce plug-in plus bas.",
        "band.out.waitingAudio": "En attente d’audio : lisez ou choisissez un groupe.",
        "band.out.validator": "Outil de validation : boucle désactivée.",
        "band.out.takeover": "Groupe {g} tenu par une instance inactive.",
    },
};
