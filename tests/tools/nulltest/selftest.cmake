# SPDX-License-Identifier: GPL-3.0-or-later
#
# rinne_nulltest 自测，用合成 WAV 跑三格：通过；D 差 1 样本 -> FAIL；两份不一致 -> FAIL。
# 期望变红的格子（FAIL）同样要检查：工具若把它们判成 PASS，这里就变红。
#   cmake -DNULLTEST=<可执行文件> -DWORK_DIR=<临时目录> -P selftest.cmake

if(NOT NULLTEST OR NOT WORK_DIR)
  message(FATAL_ERROR "需要 -DNULLTEST=<rinne_nulltest> 和 -DWORK_DIR=<临时目录>")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# run(<期望退出码> <输出里必须出现的正则> <参数...>)
function(run expect_rc expect_text)
  execute_process(COMMAND "${NULLTEST}" ${ARGN}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL expect_rc)
    message(FATAL_ERROR "期望退出码 ${expect_rc}，实际 ${rc}：rinne_nulltest ${ARGN}\n${out}${err}")
  endif()
  if(NOT out MATCHES "${expect_text}")
    message(FATAL_ERROR "输出里没有 ${expect_text}：rinne_nulltest ${ARGN}\n${out}${err}")
  endif()
endfunction()

set(sig "${WORK_DIR}/sig.wav")
set(sig2 "${WORK_DIR}/sig2.wav")

# make-signal：同参数逐字节一致；格式是 48 kHz、32 位浮点、立体声（头 44 字节 + 4 秒 * 48000 * 8 字节）
run(0 "已生成" make-signal --out "${sig}")
run(0 "已生成" make-signal --out "${sig2}")
file(SHA256 "${sig}" h1)
file(SHA256 "${sig2}" h2)
if(NOT h1 STREQUAL h2)
  message(FATAL_ERROR "同参数生成的两份文件不是逐字节一致")
endif()
file(SIZE "${sig}" size)
math(EXPR expect_size "44 + 4 * 48000 * 2 * 4")
if(NOT size EQUAL expect_size)
  message(FATAL_ERROR "test-signal 大小 ${size}，期望 ${expect_size}")
endif()
# 格式码 3（IEEE float）、2 声道、48000 Hz、字节率 384000、块对齐 8、位深 32
file(READ "${sig}" fmt_hex OFFSET 20 LIMIT 16 HEX)
if(NOT fmt_hex STREQUAL "0300020080bb000000dc050008002000")
  message(FATAL_ERROR "WAV 头不是 48 kHz 32 位浮点立体声：${fmt_hex}")
endif()
run(0 "48000 Hz，2 声道" compare --a "${sig}" --b "${sig}")

# 第 1 格：通过。同一份文件对拍逐位相同，开头 2 秒单列
run(0 "逐位相同：是.*开头 2.00 秒残差.*结果：PASS" compare --a "${sig}" --b "${sig2}" --head-seconds 2)

# 第 3 格：两份不一致 -> FAIL。种子不同的噪声，以及错位 3 样本，都必须判红并报出 lag
run(0 "已生成" make-signal --out "${WORK_DIR}/other.wav" --seed 2)
run(2 "逐位相同：否.*结果：FAIL" compare --a "${sig}" --b "${WORK_DIR}/other.wav")
run(0 "已生成" make-signal --out "${WORK_DIR}/late.wav" --offset-samples 3)
run(2 "对齐偏移（lag）：3 样本.*结果：FAIL" compare --a "${sig}" --b "${WORK_DIR}/late.wav")
# 错位是预期时加 --allow-lag 才算过（对齐后残差为 0）
run(0 "结果：PASS" compare --a "${sig}" --b "${WORK_DIR}/late.wav" --allow-lag)

# 采样率不同 -> FAIL
run(0 "已生成" make-signal --out "${WORK_DIR}/s44.wav" --sr 44100)
run(2 "采样率不同" compare --a "${sig}" --b "${WORK_DIR}/s44.wav")

# echo：等间隔冲激串，D = 2048 样本
run(0 "已生成" make-signal --out "${WORK_DIR}/train.wav" --kind impulse-train --d 2048 --count 6)
run(0 "结果：PASS" echo --in "${WORK_DIR}/train.wav" --d 2048 --sr 48000)
run(0 "结果：PASS" echo --in "${WORK_DIR}/train.wav" --d 2048samples --sr 48000)
run(0 "结果：PASS" echo --in "${WORK_DIR}/train.wav" --d 42.6667ms --sr 48000)
# 第 2 格：D 差 1 样本 -> FAIL，两个方向都试
run(2 "最大偏差：1 样本.*结果：FAIL" echo --in "${WORK_DIR}/train.wav" --d 2049 --sr 48000)
run(2 "最大偏差：1 样本.*结果：FAIL" echo --in "${WORK_DIR}/train.wav" --d 2047 --sr 48000)
# 录音里只有一个间隔不对（2047）-> FAIL
run(0 "已生成" make-signal --out "${WORK_DIR}/train2047.wav" --kind impulse-train --d 2047 --count 6)
run(2 "结果：FAIL" echo --in "${WORK_DIR}/train2047.wav" --d 2048 --sr 48000)
# 采样率对不上 -> FAIL；没有回声 -> FAIL
run(2 "对不上" echo --in "${WORK_DIR}/train.wav" --d 2048 --sr 44100)
run(0 "已生成" make-signal --out "${WORK_DIR}/one.wav" --kind impulse)
run(2 "回声峰不足" echo --in "${WORK_DIR}/one.wav" --d 2048 --sr 48000)

# 用法错误：退出码 1，且说人话
run(1 "不认识的子命令" nonsense)
run(1 "打不开文件" compare --a "${WORK_DIR}/nope.wav" --b "${sig}")
run(0 "用法" --help)
