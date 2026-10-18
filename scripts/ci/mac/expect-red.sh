#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 「期望变红」的反转：被测命令必须失败，才算这一步通过（变异构建的压测靠它）。
#   expect-red.sh [--catch2] <说明> -- <命令> [参数...]
#   expect-red.sh --self-test
# 退出码 0 是变异没被抓住，红。命令没跑起来（126、127）或被信号杀死（128 以上）也红，不能当成「抓住了」。
# --catch2：被测命令是 Catch2 v3 的测试程序时用。它的退出码是失败断言数（最多 255），而 2（没跑任何用例）、3（过滤没匹配上）、
# 4（全部 SKIP）也是小整数，光看退出码分不清「断言失败」「什么都没测」和被信号杀死；所以不看退出码的大小，
# 要求输出里有失败断言的行（`: FAILED:`）和跑完之后才会打印的汇总行（`assertions: … failed`），
# 并且不是崩溃或未捕获异常（输出里有 `fatal error condition` 或 `unexpected exception` 就不算）。
# 先出一条 FAILED 再被 SIGKILL（超时、内存不足）的运行没有汇总行，判红。

set -uo pipefail

# 判退出码与输出：打印 CAUGHT、SURVIVED 或 BROKEN
classify() { # $1 退出码 $2 输出文件 $3 catch2 或 plain
  if [ "$1" = "0" ]; then echo SURVIVED; return; fi
  if [ "$3" = "catch2" ]; then
    if grep -q ': FAILED:' "$2" && grep -qE '^assertions:.* failed' "$2" \
      && ! grep -qE 'fatal error condition|unexpected exception' "$2"; then echo CAUGHT; else echo BROKEN; fi
    return
  fi
  if [ "$1" -ge 126 ]; then echo BROKEN; return; fi
  echo CAUGHT
}

if [ "${1:-}" = "--self-test" ]; then
  bad=0
  check() { # $1 说明 $2 期望退出码 $3 模式开关（空或 --catch2） $4 命令
    local got args=()
    [ -z "$3" ] || args=("$3")
    got=$(bash "$0" ${args[@]+"${args[@]}"} "自测" -- bash -c "$4" 2>&1; echo "exit=$?")
    if [ "$(tail -n 1 <<< "$got")" != "exit=$2" ]; then echo "FAIL  ${1}（期望 exit=${2}，实际 $(tail -n 1 <<< "$got")）"; printf '%s
' "$got" | sed 's/^/      | /'; bad=1; else echo "PASS  $1"; fi
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
  [ "$bad" = 0 ] || { echo "expect-red 自测失败"; exit 1; }
  echo "expect-red 自测通过"
  exit 0
fi

mode=plain
if [ "${1:-}" = "--catch2" ]; then
  mode=catch2
  shift
fi
label=$1
shift
[ "${1:-}" = "--" ] && shift
out=$(mktemp "${TMPDIR:-/tmp}/rinne.XXXXXX")
trap 'rm -f "$out"' EXIT
rc=0
"$@" 2>&1 | tee "$out"
rc=${PIPESTATUS[0]}
case "$(classify "$rc" "$out" "$mode")" in
  CAUGHT) echo "${label}：被测命令按预期变红（退出码 ${rc}）" ;;
  SURVIVED) echo "::error::${label}：被测命令通过了，变异没有被抓住"; exit 1 ;;
  *) echo "::error::${label}：被测命令没有按预期失败（退出码 ${rc}，输出里没有失败断言，或是崩溃），不能算抓住了变异"; exit 1 ;;
esac
exit 0
