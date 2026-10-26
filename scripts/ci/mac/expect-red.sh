#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 「期望变红」的反转：被测命令必须失败，才算这一步通过（变异构建的压测靠它）。
#   expect-red.sh [--catch2] [--tag <标签>] [--section <名字>] [--timeout <秒>] <说明> -- <命令> [参数...]
#   expect-red.sh --self-test
# 退出码 0 是变异没被抓住，红。命令没跑起来（126、127）或被信号杀死（128 以上）也红，不能当成「抓住了」。
# --catch2：被测命令是 Catch2 v3 的测试程序时用。它的退出码是失败断言数（最多 255），而 2（没跑任何用例）、3（过滤没匹配上）、
# 4（全部 SKIP）也是小整数，光看退出码分不清「断言失败」「什么都没测」和被信号杀死；所以不看退出码的大小，
# 要求输出里有失败断言的行（`: FAILED:`）和跑完之后才会打印的汇总行（`assertions: … failed`），
# 并且不是崩溃或未捕获异常（输出里有 `fatal error condition` 或 `unexpected exception` 就不算）。
# 先出一条 FAILED 再被 SIGKILL（超时、内存不足）的运行没有汇总行，判红。
# --tag：要求这次运行确实按该标签过滤过（Catch2 会打印 `Filters: [标签]`），因此失败的断言都属于这个标签；
#        标签要带方括号，例如 [stress]。
# --section：要求至少一条失败断言落在同名 section 里（按 console reporter 的标题块判断）。
# --timeout：单次运行的上限秒数；不指定时不设上限，行为和以前一样。

set -uo pipefail

# 失败断言是否落在指定的 section：console reporter 在每个失败断言前先打印标题块
# （分隔线、test case 名、若干缩进的 section 名、分隔线），只要有一条失败断言的标题块里出现同名 section 就算通过。
section_ok() { # $1 输出文件 $2 section 名
  awk -v want="$2" '
    /^--+$/ {
      if (inhdr) { inhdr = 0 } else { inhdr = 1; n = 0 }
      next
    }
    inhdr { hdr[n++] = $0; next }
    /: FAILED:/ {
      for (i = 1; i < n; i++) {
        line = hdr[i]
        sub(/^[[:space:]]+/, "", line)
        if (line == want) ok = 1
      }
    }
    END { exit(ok ? 0 : 1) }
  ' "$1"
}

# 判退出码与输出：打印 CAUGHT、SURVIVED 或 BROKEN
classify() { # $1 退出码 $2 输出文件 $3 catch2 或 plain $4 tag $5 section
  if [ "$1" = "0" ]; then echo SURVIVED; return; fi
  if [ "$3" = "catch2" ]; then
    if grep -q ': FAILED:' "$2" && grep -qE '^assertions:.* failed' "$2" \
      && ! grep -qE 'fatal error condition|unexpected exception' "$2" \
      && { [ -z "$4" ] || grep -Fq "Filters: $4" "$2"; } \
      && { [ -z "$5" ] || section_ok "$2" "$5"; }; then echo CAUGHT; else echo BROKEN; fi
    return
  fi
  if [ "$1" -ge 126 ]; then echo BROKEN; return; fi
  echo CAUGHT
}

if [ "${1:-}" = "--self-test" ]; then
  bad=0
  check() { # $1 说明 $2 期望退出码 $3 模式开关（空或 --catch2） $4 命令；其余参数原样转给 expect-red
    local got desc=$1 want=$2 mode=$3 cmd=$4 args=()
    shift 4
    [ -z "$mode" ] || args=("$mode")
    got=$(bash "$0" ${args[@]+"${args[@]}"} "$@" "自测" -- bash -c "$cmd" 2>&1; echo "exit=$?")
    if [ "$(tail -n 1 <<< "$got")" != "exit=$want" ]; then echo "FAIL  ${desc}（期望 exit=${want}，实际 $(tail -n 1 <<< "$got")）"; printf '%s
' "$got" | sed 's/^/      | /'; bad=1; else echo "PASS  $desc"; fi
  }
  failed_line="echo 'a.cpp:1: FAILED:'; echo 'assertions: 3 | 2 passed | 1 failed'"
  check "命令失败（false）：这一步通过" 0 "" "false"
  check "命令成功（true）：变异没被抓住，这一步红" 1 "" "true"
  check "命令不存在（127）：没跑起来，这一步红" 1 "" "exit 127"
  check "命令被信号杀死（137）：这一步红" 1 "" "exit 137"
  check "Catch2：有失败断言、退出码 3：通过" 0 --catch2 "$failed_line; exit 3"
  check "Catch2：什么都没跑（退出码 2，无失败断言）：红" 1 --catch2 "exit 2"
  check "Catch2：过滤没匹配上（退出码 3，无失败断言）：红" 1 --catch2 "exit 3"
  check "Catch2：全部 SKIP（退出码 4，无失败断言）：红" 1 --catch2 "exit 4"
  check "Catch2：崩溃（有 FAILED 行但是 fatal error condition）：红" 1 --catch2 "$failed_line; echo 'due to a fatal error condition:'; exit 134"
  check "Catch2：先出 FAILED 再被 SIGKILL（没有汇总行）：红" 1 --catch2 "echo 'a.cpp:1: FAILED:'; exit 137"
  check "Catch2：失败断言很多（退出码 200，有汇总行）：通过" 0 --catch2 "$failed_line; exit 200"
  check "Catch2：通过（退出码 0）：红" 1 --catch2 "echo 'All tests passed'"
  # 失败断言必须属于指定的 tag：运行要留下 Filters 行，并且过滤的正是这个标签
  tagged="echo 'Filters: [stress]'; $failed_line; exit 3"
  check "Catch2：失败断言属于指定 tag" 0 --catch2 "$tagged" --tag "[stress]"
  check "Catch2：指定 tag 但运行没有按 tag 过滤" 1 --catch2 "$failed_line; exit 3" --tag "[stress]"
  check "Catch2：指定 tag 但运行过滤的是别的标签" 1 --catch2 "echo 'Filters: [other]'; $failed_line; exit 3" --tag "[stress]"
  # 失败断言必须属于指定的 section：标题块里的 section 名要和它一致
  sectioned="echo '---'; echo 'case'; echo '  malloc'; echo '---'; echo 'f.cpp:1'; echo '......'; echo 'f.cpp:1: FAILED:'; echo 'assertions: 3 | 2 passed | 1 failed'"
  wrong_section="echo '---'; echo 'case'; echo '  calloc'; echo '---'; echo 'f.cpp:1'; echo '......'; echo 'f.cpp:1: FAILED:'; echo 'assertions: 3 | 2 passed | 1 failed'"
  check "Catch2：失败断言在指定 section" 0 --catch2 "$sectioned; exit 3" --section malloc
  check "Catch2：失败断言不在指定 section" 1 --catch2 "$wrong_section; exit 3" --section malloc
  check "Catch2：tag 与 section 同时匹配" 0 --catch2 "echo 'Filters: [stress]'; $sectioned; exit 3" --tag "[stress]" --section malloc
  # 单次超时：上限内失败照常通过，跑不完的判红
  check "超时：命令在上限内失败，照常通过" 0 "" "false" --timeout 5
  check "超时：命令跑不完，判红" 1 "" "sleep 5" --timeout 1
  [ "$bad" = 0 ] || { echo "expect-red 自测失败"; exit 1; }
  echo "expect-red 自测通过"
  exit 0
fi

mode=plain
tag=""
section=""
timeout=0
while [ $# -gt 0 ]; do
  case "$1" in
    --catch2) mode=catch2; shift ;;
    --tag|--section|--timeout)
      [ $# -ge 2 ] || { echo "::error::$1 要一个值"; exit 2; }
      case "$1" in
        --tag) tag=$2 ;;
        --section) section=$2 ;;
        --timeout) timeout=$2 ;;
      esac
      shift 2
      ;;
    --) shift; break ;;
    *) break ;;
  esac
done
label=${1:-}
shift || true
[ "${1:-}" = "--" ] && shift

case "$timeout" in
  ''|*[!0-9]*) echo "::error::--timeout 要一个非负整数秒数"; exit 2 ;;
esac
if [ -n "$tag" ]; then
  case "$tag" in
    \[*\] ) ;;
    *) echo "::error::--tag 要写成带方括号的 Catch2 标签，例如 [stress]"; exit 2 ;;
  esac
fi
if [ "$mode" != "catch2" ] && { [ -n "$tag" ] || [ -n "$section" ]; }; then
  echo "::error::--tag 与 --section 只能和 --catch2 一起用"
  exit 2
fi

out=$(mktemp "${TMPDIR:-/tmp}/rinne.XXXXXX")
# 「走完了」标记（约定写法，selftest.sh 与本机 gate 会扫）：macOS 自带的 bash 3.2 在变量展开出错时会静默结束脚本，
# 装了 EXIT trap 时进入 trap 的 $? 常常是 0，变异构建的压测就会在半路死掉之后被当成「抓住了」。所以只有走到末尾才把标记置 1，
# trap 里退出码为 0 而标记没置位，就改成非 0。这里只拦「本脚本自己没走完」，被测命令的结果照 classify 的判法，一个字没改。
RINNE_RAN_TO_END=0
on_exit() {
  local rc=$?
  rm -f "$out" "$out.timedout"
  if [ "$rc" -eq 0 ] && [ "$RINNE_RAN_TO_END" != 1 ]; then
    echo "::error::expect-red.sh：脚本没有走完就退出了（bash 3.2 下变量展开出错会静默结束脚本，退出码是 0）：按失败处理"
    exit 1
  fi
}
trap on_exit EXIT
rc=0
if [ "$timeout" -gt 0 ]; then
  "$@" >"$out" 2>&1 &
  pid=$!
  ( sleep "$timeout"; : > "$out.timedout"; kill -TERM "$pid" 2>/dev/null; sleep 5; kill -KILL "$pid" 2>/dev/null ) </dev/null >/dev/null 2>&1 &
  watchdog=$!
  wait "$pid" || rc=$?
  kill "$watchdog" 2>/dev/null || true
  wait "$watchdog" 2>/dev/null || true
  cat "$out"
  if [ -f "$out.timedout" ]; then
    echo "::error::${label}：被测命令超过 ${timeout} 秒还没有结束，按未抓住变异处理"
    exit 1
  fi
else
  "$@" 2>&1 | tee "$out"
  rc=${PIPESTATUS[0]}
fi
case "$(classify "$rc" "$out" "$mode" "$tag" "$section")" in
  CAUGHT) echo "${label}：被测命令按预期变红（退出码 ${rc}）" ;;
  SURVIVED) echo "::error::${label}：被测命令通过了，变异没有被抓住"; exit 1 ;;
  *) echo "::error::${label}：被测命令没有按预期失败（退出码 ${rc}，输出里没有失败断言，或是崩溃），不能算抓住了变异"; exit 1 ;;
esac
RINNE_RAN_TO_END=1
exit 0
