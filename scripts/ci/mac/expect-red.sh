#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 「期望变红」的反转：被测命令必须失败，才算这一步通过（变异构建的压测、致盲计数器的用例都靠它）。
#   expect-red.sh <说明> -- <命令> [参数...]
#   expect-red.sh --self-test
# 只有 Catch2 式的失败才算「红得对」：退出码 1 到 99（Catch2 的退出码是失败用例数）。退出码 0 是变异没被抓住，红；
# 127、126、被信号杀死（128 以上）、超时这类说明命令根本没跑起来，也红，不能当成「抓住了」。

set -uo pipefail

# 判退出码：打印 CAUGHT、SURVIVED 或 BROKEN
classify() {
  if [ "$1" = "0" ]; then echo SURVIVED
  elif [ "$1" -ge 1 ] && [ "$1" -le 99 ]; then echo CAUGHT
  else echo BROKEN
  fi
}

if [ "${1:-}" = "--self-test" ]; then
  bad=0
  check() { # $1 说明 $2 期望 $3 命令
    local got
    got=$(bash "$0" "自测" -- bash -c "$3" 2>/dev/null; echo "exit=$?")
    if [ "$(tail -n 1 <<< "$got")" != "exit=$2" ]; then echo "FAIL  $1（期望 exit=$2，实际 $(tail -n 1 <<< "$got")）"; bad=1; else echo "PASS  $1"; fi
  }
  check "命令失败（false）：这一步通过" 0 "false"
  check "命令成功（true）：变异没被抓住，这一步红" 1 "true"
  check "命令不存在（127）：没跑起来，这一步红" 1 "exit 127"
  check "命令被信号杀死（137）：这一步红" 1 "exit 137"
  check "Catch2 式的失败（退出码 3）：这一步通过" 0 "exit 3"
  [ "$bad" = 0 ] || { echo "expect-red 自测失败"; exit 1; }
  echo "expect-red 自测通过"
  exit 0
fi

label=$1
shift
[ "${1:-}" = "--" ] && shift
rc=0
"$@" || rc=$?
case "$(classify "$rc")" in
  CAUGHT) echo "$label：被测命令按预期变红（退出码 $rc）" ;;
  SURVIVED) echo "::error::$label：被测命令通过了，变异没有被抓住"; exit 1 ;;
  *) echo "::error::$label：被测命令没有正常跑完（退出码 $rc），不能算抓住了变异"; exit 1 ;;
esac
