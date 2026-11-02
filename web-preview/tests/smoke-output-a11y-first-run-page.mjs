// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 页面的可访问性冒烟（无头浏览器）· 首启部分：首启的 6 个场景（语言卡、规则页、导览 4 步）加规则页放行后的画面 × 中 / 英 / 法。
// 从 smoke-output-a11y-page.mjs 拆出来，为的是两个文件各自跑在 run-all 单项 300 秒的上限之内（合在一起实测 317 秒，超了），判据一条没动。
// 每个首启画面：先断言叠层真的摆出来了（种类、导览步号、规则页的放行状态），再查
//   名称：页上每个可交互件都有非空的可访问名称，<html lang> 等于界面语言；
//   叠层：叠层是带名称的模态对话框（role=dialog、aria-modal、aria-labelledby 指到有字的节点）、
//      叠层打开时内容层 inert、键盘焦点在叠层之内；叠层里每颗按钮有名称，规则页未放行时两颗按钮真禁用并带 title、放行后可用，
//      导览第 1 步的「上一步」禁用；规则页正文区能用键盘到达；Tab 走一圈每一站只落在叠层里、且有看得见的焦点环。
//      Output 没有紧急切断，所以没有「叠层之上的紧急切断」这一项（Input 那边另有）。已在 smoke-first-run-page 断言过的不重复：
//      Esc 的去留、按钮放行的时机、红字。
//      坏样例：去掉叠层名称、去掉内容层 inert、把焦点移出叠层、把规则页按钮改成可用、关掉叠层里的焦点环，各项必须判红。
// 浏览器缺失时整套跳过（退出码 2）。
//
// 用法：node web-preview/tests/smoke-output-a11y-first-run-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import {
    LANGS,
    NAME_AUDIT,
    enableFocus,
    frameTag,
    isFirstRunScene,
    loadDesign,
    loadOutputScenes,
    openOutputFrame,
    outputFrames,
    overlayFailures,
    overlayState,
    tabWalk,
} from "../fixtures/output/lib.mjs";
import { overlayProblems } from "../fixtures/first-run/measure.mjs";

const ROOT = repoRoot();
const run = createRun("Output 页面可访问性（首启）");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const box = await loadDesign(ROOT);
const scenes = await loadOutputScenes(ROOT);
const frames = outputFrames(scenes);
const firstRunFrames = frames.filter((f) => isFirstRunScene(f.scene));
/** 导览共几步（场景表里 out-tour-N 的个数）：最后一步的按钮是「完成」而不是「下一步」。 */
const TOUR_STEPS = scenes.filter((s) => /^out-tour-[0-9]+$/.test(s.id)).length;
const server = await startStaticServer(ROOT);
const browser = await launchBrowser();

try {
    // 每个语言用一张新页：Tab 走一圈会把键盘焦点留在奇怪的地方，后面的键盘事件会落空
    const errors = [];
    let page = null;
    const freshPage = async () => {
        if (page) {
            errors.push(...page.errors);
            await page.close();
        }
        page = await browser.newPage();
        await enableFocus(page);
    };

    let overlayChecked = 0;
    for (const lang of LANGS) {
        await freshPage();
        for (const frame of firstRunFrames) {
            const ftag = frameTag(lang, frame);
            await openOutputFrame(page, server, box, lang, frame);
            const audit = await page.eval(NAME_AUDIT);
            run.check(audit.interactive >= 12, `${ftag}：应有足够多的可交互件（防空转），实得 ${audit.interactive}`);
            run.eq(audit.missing, [], `${ftag}：这些可交互件没有可访问名称`);
            run.eq(await page.eval("document.documentElement.lang"), lang, `${ftag}：<html lang>`);
            run.eq(await overlayFailures(page, frame), [], `${ftag}：首启叠层应摆出来`);
            run.eq(overlayProblems(await overlayState(page), frame, TOUR_STEPS), [], `${ftag}：叠层的无障碍问题`);
            const walk = await tabWalk(page);
            run.check(
                walk.some((s) => s.overlay),
                `${ftag}：Tab 一圈应进到叠层里，实得 ${JSON.stringify(walk.map((s) => s.id))}`,
            );
            run.eq(
                walk.filter((s) => !s.overlay).map((s) => s.id),
                [],
                `${ftag}：Tab 一圈只该落在叠层里（内容层 inert）`,
            );
            run.eq(
                walk.filter((s) => !s.ring).map((s) => s.id),
                [],
                `${ftag}：这些 Tab 停靠点没有看得见的焦点环`,
            );
            overlayChecked++;
        }
    }
    run.eq(overlayChecked, 7 * LANGS.length, "首启画面：6 个场景加规则页放行后一个，每种语言各查一遍");
    run.eq(frames.length, scenes.length + 1, "每个场景一个画面，规则页多一个放行后的画面（防新增场景漏测）");

    // ---- ⑤ 首启叠层的坏样例：量尺本身可信，每一条改坏之后对应的判据必须报出来 ----
    await freshPage();
    const frameOf = (id, sub = "") => ({ scene: scenes.find((s) => s.id === id), sub });
    const bad5 = async (label, id, sub, mutate, re) => {
        const frame = frameOf(id, sub);
        await openOutputFrame(page, server, box, "zh", frame);
        run.eq(
            overlayProblems(await overlayState(page), frame, TOUR_STEPS),
            [],
            `坏样例前提（${label}）：改坏之前应没有问题`,
        );
        await page.eval(mutate);
        const got = overlayProblems(await overlayState(page), frame, TOUR_STEPS);
        run.check(
            got.some((m) => re.test(m)),
            `坏样例：${label}，应报出 ${re}，实得 ${JSON.stringify(got)}`,
        );
    };
    await bad5(
        "导览去掉名称",
        "out-tour-2",
        "",
        `document.querySelector('[data-gb="output.tour"]').removeAttribute("aria-labelledby")`,
        /没有可访问名称/,
    );
    await bad5(
        "语言卡去掉名称",
        "out-lang",
        "",
        `document.querySelector('[data-gb="output.langStart"]').removeAttribute("aria-labelledby")`,
        /没有可访问名称/,
    );
    await bad5(
        "去掉内容层 inert",
        "out-rules",
        "",
        `document.querySelector('[data-gb="output.shell"] > [data-part="content"]').removeAttribute("inert")`,
        /内容层应是 inert/,
    );
    await bad5(
        "焦点移出叠层",
        "out-tour-3",
        "",
        `document.activeElement && document.activeElement.blur()`,
        /焦点应在叠层之内/,
    );
    await bad5(
        "规则页按钮在未放行时可用",
        "out-rules",
        "",
        `document.querySelector('[data-part="rules.start"]').removeAttribute("disabled")`,
        /rules\.start 在未放行应禁用/,
    );
    await bad5(
        "规则页按钮放行后仍禁用",
        "out-rules",
        "ready",
        `document.querySelector('[data-part="rules.skip"]').setAttribute("disabled", "")`,
        /rules\.skip 在放行后应可用/,
    );
    await bad5(
        "语言按钮没有名称",
        "out-lang",
        "",
        `document.querySelector('[data-part="langStart.en"]').textContent = ""`,
        /langStart\.en 没有名称/,
    );
    // 内容层不 inert：Tab 一圈会走进内容层，「只该落在叠层里」要报红
    await openOutputFrame(page, server, box, "zh", frameOf("out-tour-2"));
    await page.eval(
        `document.querySelector('[data-gb="output.shell"] > [data-part="content"]').removeAttribute("inert")`,
    );
    {
        const walk = await tabWalk(page);
        run.check(
            walk.filter((s) => !s.overlay).length >= 5,
            `坏样例：去掉内容层 inert 后 Tab 一圈应走进内容层，实得 ${JSON.stringify(walk.map((s) => s.id))}`,
        );
    }
    // 关掉焦点环：叠层里的停靠点要报出没有环
    await freshPage();
    await openOutputFrame(page, server, box, "zh", frameOf("out-tour-2"));
    await page.eval(
        `(() => { const s = document.createElement("style"); s.textContent = ".rn-shell *, .rn-shell *:focus-visible { outline: none !important; }"; document.head.append(s); })()`,
    );
    {
        const walk = await tabWalk(page);
        run.check(
            walk.filter((s) => s.overlay && !s.ring).length >= 2,
            `坏样例：关掉焦点环后叠层里应报出没有环的停靠点，实得 ${JSON.stringify(walk)}`,
        );
    }

    errors.push(...page.errors);
    run.check(errors.length === 0, `页面不应有错误：${errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
