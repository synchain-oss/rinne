# SPDX-License-Identifier: GPL-3.0-or-later
#
# rinne_nulltest 自测，用合成 WAV 跑三格：通过；D 差 1 样本 -> FAIL；两份不一致 -> FAIL。
# 期望变红的格子（FAIL）同样要检查：工具若把它们判成 PASS，这里就变红。
#   cmake -DNULLTEST=<可执行文件> -DWORK_DIR=<父目录，脚本在其下新建本次运行的子目录> -P selftest.cmake

if(NOT NULLTEST OR NOT FIXTURES OR NOT WORK_DIR)
  message(FATAL_ERROR "需要 -DNULLTEST=<rinne_nulltest>、-DFIXTURES=<夹具生成器> 和 -DWORK_DIR=<父目录，脚本在其下新建本次运行的子目录>")
endif()
# WORK_DIR 从 -D 传入，只当父目录：不清空、不递归删。本次运行的 WAV 都写在它下面新建的
# run-<时间戳>-<随机> 子目录里，跑完只删这个自己新建的子目录；子目录已存在就报错，不复用。
file(MAKE_DIRECTORY "${WORK_DIR}")
string(TIMESTAMP run_stamp "%Y%m%d-%H%M%S" UTC)
string(RANDOM LENGTH 12 ALPHABET "0123456789abcdef" run_random)
set(RUN_DIR "${WORK_DIR}/run-${run_stamp}-${run_random}")
if(EXISTS "${RUN_DIR}")
  message(FATAL_ERROR "本次运行的子目录已存在，不复用也不清空：${RUN_DIR}")
endif()
file(MAKE_DIRECTORY "${RUN_DIR}")

# run(<期望退出码> <stdout 与 stderr 合起来必须出现的正则> <参数...>)
function(run expect_rc expect_text)
  execute_process(COMMAND "${NULLTEST}" ${ARGN}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
    ENCODING UTF-8)
  if(NOT rc EQUAL expect_rc)
    message(FATAL_ERROR "期望退出码 ${expect_rc}，实际 ${rc}：rinne_nulltest ${ARGN}\n${out}${err}")
  endif()
  # 工具把用法错误、文件打不开一类的提示写到 stderr，正常报告写到 stdout，两者合起来匹配
  if(NOT "${out}${err}" MATCHES "${expect_text}")
    message(FATAL_ERROR "输出里没有 ${expect_text}：rinne_nulltest ${ARGN}\n${out}${err}")
  endif()
endfunction()

# fx(<夹具名> <输出文件>)：用夹具生成器写一个手工拼的 WAV
function(fx name out)
  execute_process(COMMAND "${FIXTURES}" "${name}" "${out}" RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "夹具 ${name} 生成失败（${rc}）：${err}")
  endif()
endfunction()

set(sig "${RUN_DIR}/sig.wav")
set(sig2 "${RUN_DIR}/sig2.wav")

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
run(0 "已生成" make-signal --out "${RUN_DIR}/other.wav" --seed 2)
run(2 "逐位相同：否.*结果：FAIL" compare --a "${sig}" --b "${RUN_DIR}/other.wav")
run(0 "已生成" make-signal --out "${RUN_DIR}/late.wav" --offset-samples 3)
run(2 "对齐偏移（lag）：3 样本.*结果：FAIL" compare --a "${sig}" --b "${RUN_DIR}/late.wav")
# 错位是预期时加 --allow-lag 才算过（对齐后残差为 0）
run(0 "结果：PASS" compare --a "${sig}" --b "${RUN_DIR}/late.wav" --allow-lag)

# 其他样本格式：64 位浮点、16 / 24 / 32 位整数各生成两份，对拍必须逐位相同（噪声里有负数，覆盖符号扩展）；
# 文件大小 = 头 44 字节 + 4 秒 * 48000 * 2 声道 * 每样本字节数
foreach(fmt IN ITEMS f64 i16 i24 i32)
  string(REGEX REPLACE "^[fi]" "" fmt_bits "${fmt}")
  math(EXPR fmt_bytes "${fmt_bits} / 8")
  run(0 "已生成" make-signal --out "${RUN_DIR}/fmt-${fmt}-a.wav" --format ${fmt})
  run(0 "已生成" make-signal --out "${RUN_DIR}/fmt-${fmt}-b.wav" --format ${fmt})
  file(SIZE "${RUN_DIR}/fmt-${fmt}-a.wav" fmt_size)
  math(EXPR fmt_expect "44 + 4 * 48000 * 2 * ${fmt_bytes}")
  if(NOT fmt_size EQUAL fmt_expect)
    message(FATAL_ERROR "--format ${fmt} 的文件大小 ${fmt_size}，期望 ${fmt_expect}")
  endif()
  run(0 "逐位相同：是.*结果：PASS" compare --a "${RUN_DIR}/fmt-${fmt}-a.wav" --b "${RUN_DIR}/fmt-${fmt}-b.wav")
  run(0 "已生成" make-signal --out "${RUN_DIR}/fmt-${fmt}-c.wav" --format ${fmt} --seed 2)
  run(2 "逐位相同：否.*结果：FAIL" compare --a "${RUN_DIR}/fmt-${fmt}-a.wav" --b "${RUN_DIR}/fmt-${fmt}-c.wav")
endforeach()
run(1 "--format 只能是" make-signal --out "${RUN_DIR}/bad.wav" --format i8)
# 原始字节判据：两份 i32 文件只差冲激的 1 个整数码，解码成 float 后完全相同，必须仍报「逐位相同：否」。
# 结果仍是 PASS：解码成 float 后残差恰好为 0，判 PASS 与否看残差，逐位相同单列在报告里
run(0 "已生成" make-signal --out "${RUN_DIR}/lsb.wav" --format i32 --lsb-flip)
run(0 "逐位相同：否.*结果：PASS" compare --a "${RUN_DIR}/fmt-i32-a.wav" --b "${RUN_DIR}/lsb.wav")
run(1 "--lsb-flip 只用于" make-signal --out "${RUN_DIR}/bad.wav" --lsb-flip)
# --seconds 太短，放不下冲激或噪声突发：报错，不能静默生成残缺的文件
run(1 "放不下 0.5 s" make-signal --out "${RUN_DIR}/short.wav" --kind impulse --seconds 0.4)
run(1 "放不下 1.0 s" make-signal --out "${RUN_DIR}/short.wav" --seconds 1.0)
# 数据超过 4 GiB：分配之前就报错
run(1 "4 GiB" make-signal --out "${RUN_DIR}/huge.wav" --seconds 600 --sr 384000 --channels 8 --format f64)

# 采样率不同 -> FAIL
run(0 "已生成" make-signal --out "${RUN_DIR}/s44.wav" --sr 44100)
run(2 "采样率不同" compare --a "${sig}" --b "${RUN_DIR}/s44.wav")

# echo：等间隔冲激串，D = 2048 样本
run(0 "已生成" make-signal --out "${RUN_DIR}/train.wav" --kind impulse-train --d 2048 --count 6)
run(0 "结果：PASS" echo --in "${RUN_DIR}/train.wav" --d 2048 --sr 48000)
run(0 "结果：PASS" echo --in "${RUN_DIR}/train.wav" --d 2048samples --sr 48000)
run(0 "结果：PASS" echo --in "${RUN_DIR}/train.wav" --d 42.6667ms --sr 48000)
# 第 2 格：D 差 1 样本 -> FAIL，两个方向都试
run(2 "最大偏差：1 样本.*结果：FAIL" echo --in "${RUN_DIR}/train.wav" --d 2049 --sr 48000)
run(2 "最大偏差：1 样本.*结果：FAIL" echo --in "${RUN_DIR}/train.wav" --d 2047 --sr 48000)
# 录音里只有一个间隔不对（2047）-> FAIL
run(0 "已生成" make-signal --out "${RUN_DIR}/train2047.wav" --kind impulse-train --d 2047 --count 6)
run(2 "结果：FAIL" echo --in "${RUN_DIR}/train2047.wav" --d 2048 --sr 48000)
# 冲激串放不下（超出 --seconds）要报错，不能静默少生成
run(1 "放不下" make-signal --out "${RUN_DIR}/toolong.wav" --kind impulse-train --d 2048 --count 1000)
# 采样率对不上 -> FAIL；没有回声 -> FAIL
run(2 "对不上" echo --in "${RUN_DIR}/train.wav" --d 2048 --sr 44100)
run(0 "已生成" make-signal --out "${RUN_DIR}/one.wav" --kind impulse)
run(2 "回声峰不足" echo --in "${RUN_DIR}/one.wav" --d 2048 --sr 48000)

# echo 至少要量到 2 个间隔（3 个峰）才下结论；D 太小、非整数的 D 与 sr 是用法错误；录音含 NaN 也不下结论
run(0 "已生成" make-signal --out "${RUN_DIR}/train2.wav" --kind impulse-train --d 2048 --count 2)
run(2 "回声峰不足 3 个" echo --in "${RUN_DIR}/train2.wav" --d 2048 --sr 48000)
run(0 "共验证 5 个间隔.*结果：PASS" echo --in "${RUN_DIR}/train.wav" --d 2048 --sr 48000)
run(1 "至少要 2" echo --in "${RUN_DIR}/train.wav" --d 1 --sr 48000)
run(1 "必须是整数" echo --in "${RUN_DIR}/train.wav" --d 2048.5 --sr 48000)
run(1 "必须是整数" echo --in "${RUN_DIR}/train.wav" --d 2048 --sr 48000.5)
fx(nan "${RUN_DIR}/nan.wav")
run(2 "NaN 或 Inf" echo --in "${RUN_DIR}/nan.wav" --d 2048 --sr 48000)

# compare 要求里面真有声音：空文件、全静音、含 NaN、信号太弱（含静音对微弱噪声）一律 FAIL，不能因为"一模一样"就 PASS
fx(empty "${RUN_DIR}/empty.wav")
fx(silence "${RUN_DIR}/silence.wav")
fx(weak "${RUN_DIR}/weak.wav")
run(2 "结果：FAIL.*0 帧" compare --a "${RUN_DIR}/empty.wav" --b "${RUN_DIR}/empty.wav")
run(2 "信号太弱，无法判定" compare --a "${RUN_DIR}/silence.wav" --b "${RUN_DIR}/silence.wav")
run(2 "信号太弱，无法判定" compare --a "${RUN_DIR}/silence.wav" --b "${RUN_DIR}/weak.wav")
run(2 "信号太弱，无法判定" compare --a "${RUN_DIR}/weak.wav" --b "${RUN_DIR}/weak.wav")
run(2 "NaN 或 Inf" compare --a "${RUN_DIR}/nan.wav" --b "${RUN_DIR}/nan.wav")
# 长度不同：即使多出来的尾部是静音也 FAIL（make-signal 3 秒 vs 4 秒，前 3 秒完全相同）
run(0 "已生成" make-signal --out "${RUN_DIR}/three.wav" --seconds 3)
run(2 "两份长度不同，导出范围不一致" compare --a "${sig}" --b "${RUN_DIR}/three.wav")

# 残差门限（默认 -120 dBFS）本身：lag = 0、不逐位相同的两份，差 -118 dB 要 FAIL，差 -122 dB 要 PASS
fx(noise-a "${RUN_DIR}/noise-a.wav")
fx(noise-bm118 "${RUN_DIR}/noise-b-118.wav")
fx(noise-bm122 "${RUN_DIR}/noise-b-122.wav")
run(2 "逐位相同：否.*对齐偏移（lag）：0 样本.*结果：FAIL.*高于门限"
    compare --a "${RUN_DIR}/noise-a.wav" --b "${RUN_DIR}/noise-b-118.wav")
run(0 "逐位相同：否.*对齐偏移（lag）：0 样本.*结果：PASS" compare --a "${RUN_DIR}/noise-a.wav" --b "${RUN_DIR}/noise-b-122.wav")
# 门限可调，方向也要对：同一份 -118 dB 的差，门限放宽到 -100 就 PASS
run(0 "结果：PASS" compare --a "${RUN_DIR}/noise-a.wav" --b "${RUN_DIR}/noise-b-118.wav" --threshold-db -100)

# 读入路径：真实 DAW 导出的头形状。和 ref-f32 逐值相同的 24 位文件，用不同容器写出来，解码后必须逐位相同
fx(ref-f32 "${RUN_DIR}/ref.wav")
foreach(fix IN ITEMS plain24 ext24 data-ffff)
  fx(${fix} "${RUN_DIR}/fix-${fix}.wav")
  run(0 "逐位相同：是.*结果：PASS" compare --a "${RUN_DIR}/ref.wav" --b "${RUN_DIR}/fix-${fix}.wav")
endforeach()
# RF64 不支持、头被截断：退出码 1 并说人话，不能判通过
fx(rf64 "${RUN_DIR}/fix-rf64.wav")
fx(trunc "${RUN_DIR}/fix-trunc.wav")
run(1 "这不是 WAV 文件" compare --a "${RUN_DIR}/ref.wav" --b "${RUN_DIR}/fix-rf64.wav")
run(1 "被截断" compare --a "${RUN_DIR}/ref.wav" --b "${RUN_DIR}/fix-trunc.wav")

# 24 位、单声道、总样本数为奇数：data 长度是奇数，后面要补一个 0 字节（文件 = 44 + 12003 + 1）
run(0 "已生成" make-signal --out "${RUN_DIR}/odd24.wav" --kind impulse --format i24 --channels 1 --sr 8000 --seconds 0.5001)
file(SIZE "${RUN_DIR}/odd24.wav" odd_size)
if(NOT odd_size EQUAL 12048)
  message(FATAL_ERROR "奇数长度的 24 位 data 没有补字节：文件大小 ${odd_size}，期望 12048")
endif()
run(0 "逐位相同：是.*结果：PASS" compare --a "${RUN_DIR}/odd24.wav" --b "${RUN_DIR}/odd24.wav")

# 用法错误：退出码 1，且说人话
run(1 "不认识的子命令" nonsense)
run(1 "打不开文件" compare --a "${RUN_DIR}/nope.wav" --b "${sig}")
run(0 "用法" --help)

# 用例全部通过：只删本进程这次新建的 RUN_DIR。删之前断言它的真实路径确实是 WORK_DIR 真实路径
# 下带 run- 前缀的子目录（用的就是上面 MAKE_DIRECTORY 新建的那个变量，不重算）；断言不过就
# 报错、不删，失败的现场留着便于排查。
file(REAL_PATH "${WORK_DIR}" work_real)
file(REAL_PATH "${RUN_DIR}" run_real)
string(FIND "${run_real}" "${work_real}/run-" prefix_pos)
if(NOT prefix_pos EQUAL 0)
  message(FATAL_ERROR "拒绝删除：${run_real} 不是 ${work_real} 下本次新建的 run- 子目录")
endif()
# rinne-delete-ok: 只删本次新建的 RUN_DIR，上面已断言真实路径是 WORK_DIR 下的 run- 子目录
file(REMOVE_RECURSE "${RUN_DIR}")
