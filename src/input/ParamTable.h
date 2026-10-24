// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 的参数表：参数冻结前的草案，冻结时由用户确认。
//
// 这是一张 X-macro 表，只有宏行，没有 include 保护（同一张表要按需展开几次）。用法：先定义 RINNE_PARAM
// （要选项文字时再定义 RINNE_CHOICE），再 #include 本文件；本文件末尾会把它们 #undef。
//
//   RINNE_PARAM(id, index, kind, lo, hi, def, step, skew, automatable, unit, name)
//     id           参数 ID（ASCII 小写加下划线），永不改
//     index        0 起的序号，与 id 同序，永不重排
//     kind         F 连续、B 布尔、C 档位
//     lo hi def    范围与默认，一律是明文值（% / ms / Hz / dB，与 state 里存的一致）；B 与 C 取 0…档数−1
//     step         步进。除 ceiling（0.1）外是草案
//     skew         lin 线性、log 对数。是草案，对数曲线的细节（如 delay_ms 的中点）冻结时定
//     automatable  1 宿主可自动化、0 不可
//     unit         none、pct、ms、hz、db
//     name         宿主里看到的英文名。是草案；中文名与界面文案归 i18n，不在这里
//   RINNE_CHOICE(id, index, bridge, label)
//     C 档位参数的第 index 档。bridge 是界面桥上这一档的取值 id（小写 ASCII 与数字），label 是宿主里看到的文字。
//     宿主的档位下标与桥取值 id 的对应以本表为准，桥文档列取值的先后不作准。
//     delay_note 的 16 档顺序冻结，按时值严格升序，只能在末尾追加。
//
// 约定：low_cut 取下限 20 Hz 表示关，high_cut 取上限 20000 Hz 表示关。
// 每个 RINNE_PARAM / RINNE_CHOICE 占一行、全是字面量、不含表达式，写成 clang-format 的规范形（单空格、不对齐）：
// 改动这些行就是改参数契约，tests/golden/params-input.json 要同步改。

#ifndef RINNE_PARAM
#error "先定义 RINNE_PARAM(id, index, kind, lo, hi, def, step, skew, automatable, unit, name) 再包含 ParamTable.h"
#endif
#ifndef RINNE_CHOICE
#define RINNE_CHOICE(id, index, bridge, label)
#endif

RINNE_PARAM(feedback, 0, F, 0.0, 150.0, 0.0, 0.1, lin, 1, pct, "Feedback")
RINNE_PARAM(loop_on, 1, B, 0, 1, 1, 1, lin, 1, none, "Loop")
RINNE_PARAM(mix, 2, F, 0.0, 100.0, 25.0, 0.1, lin, 1, pct, "Mix")
RINNE_PARAM(delay_mode, 3, C, 0, 1, 0, 1, lin, 0, none, "Delay Mode")
RINNE_PARAM(delay_ms, 4, F, 1.0, 4000.0, 100.0, 0.1, log, 0, ms, "Delay")
RINNE_PARAM(delay_note, 5, C, 0, 15, 10, 1, lin, 0, none, "Sync Note")
RINNE_PARAM(low_cut, 6, F, 20.0, 2000.0, 20.0, 1.0, log, 1, hz, "Low Cut")
RINNE_PARAM(high_cut, 7, F, 1000.0, 20000.0, 20000.0, 1.0, log, 1, hz, "High Cut")
RINNE_PARAM(polarity, 8, B, 0, 1, 0, 1, lin, 1, none, "Invert")
RINNE_PARAM(limiter, 9, B, 0, 1, 1, 1, lin, 0, none, "Return Limiter")
RINNE_PARAM(ceiling, 10, F, -24.0, 0.0, -6.0, 0.1, lin, 0, db, "Ceiling")

RINNE_CHOICE(delay_mode, 0, time, "Time")
RINNE_CHOICE(delay_mode, 1, sync, "Sync")

RINNE_CHOICE(delay_note, 0, n64, "1/64")
RINNE_CHOICE(delay_note, 1, n32t, "1/32T")
RINNE_CHOICE(delay_note, 2, n32, "1/32")
RINNE_CHOICE(delay_note, 3, n16t, "1/16T")
RINNE_CHOICE(delay_note, 4, n16, "1/16")
RINNE_CHOICE(delay_note, 5, n8t, "1/8T")
RINNE_CHOICE(delay_note, 6, n16d, "1/16D")
RINNE_CHOICE(delay_note, 7, n8, "1/8")
RINNE_CHOICE(delay_note, 8, n4t, "1/4T")
RINNE_CHOICE(delay_note, 9, n8d, "1/8D")
RINNE_CHOICE(delay_note, 10, n4, "1/4")
RINNE_CHOICE(delay_note, 11, n2t, "1/2T")
RINNE_CHOICE(delay_note, 12, n4d, "1/4D")
RINNE_CHOICE(delay_note, 13, n2, "1/2")
RINNE_CHOICE(delay_note, 14, n2d, "1/2D")
RINNE_CHOICE(delay_note, 15, n1, "1/1")

#undef RINNE_PARAM
#undef RINNE_CHOICE
