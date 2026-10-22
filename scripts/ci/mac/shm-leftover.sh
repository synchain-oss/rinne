#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# POSIX 共享内存残段检查：验证前后各跑一次，会合段与数据段不许有残留。
#   shm-leftover.sh <构建目录> <说明>
#   shm-leftover.sh --self-test
# mac 上 POSIX 共享内存段没有可列目录，只能由认得段名的代码去探：跨进程契约套件 rinne_ipc_tests 里带 [shm-leftover] 标签的
# 用例负责（按命名模块生成全部会合段与数据段的名字，逐个 shm_open 探，存在就失败；名字由命名模块决定，本脚本不写死）。
# SKIP 只看 tests/ipc 下有没有源码，和 tests/CMakeLists.txt 建 rinne_ipc_tests 的判据一致：没有源码就 SKIP；
# 有源码却没有构建出 rinne_ipc_tests 是构建或登记问题，判红，不能悄悄当成空检查。-w NoAssertions 让零断言的用例也算失败。

set -uo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)

# 返回 0 表示干净或按源码缺失 SKIP，1 表示失败。
probe() { # $1 仓根 $2 构建目录 $3 说明
  local repo=$1 build=$2 label=$3 exe log rc=0
  if ! find "$repo/tests/ipc" -name '*.cpp' -print -quit 2>/dev/null | grep -q .; then
    echo "::notice title=shm-leftover::SKIP: tests/ipc has no source yet, nothing to probe ($label)"
    echo "- 残段检查（${label}）：SKIP，跨进程契约套件还没有源码" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
    return 0
  fi
  exe=$(find "$build" -type f -name rinne_ipc_tests -perm -u+x -not -path '*/_deps/*' -print -quit 2>/dev/null || true)
  if [ -z "$exe" ]; then
    echo "::error::tests/ipc 有源码，但构建目录 ${build} 里没有 rinne_ipc_tests（构建没跑或目标被删？）"
    echo "- 残段检查（${label}）：**失败**，找不到 rinne_ipc_tests" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
    return 1
  fi
  log=$(mktemp "${TMPDIR:-/tmp}/rinne.XXXXXX")
  "$exe" -w NoAssertions "[shm-leftover]" </dev/null > "$log" 2>&1 || rc=$?
  cat "$log"
  if grep -q 'No test cases matched' "$log"; then
    echo "::error::rinne_ipc_tests 里没有带 [shm-leftover] 标签的用例（被改名或改了标签？）"
    rm -f "$log"
    return 1
  fi
  rm -f "$log"
  if [ "$rc" != "0" ]; then
    echo "::error::残段检查（${label}）失败，退出码 ${rc}：有共享内存段残留，或零断言的用例没有真正探测"
    echo "- 残段检查（${label}）：**有残留**" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
    return 1
  fi
  echo "残段检查（${label}）：干净"
  echo "- 残段检查（${label}）：干净" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
}

if [ "${1:-}" = "--self-test" ]; then
  tmp=$(mktemp -d "${TMPDIR:-/tmp}/rinne.XXXXXX")
  # rinne-delete-ok: 自测里 mktemp -d 新建的目录
  trap 'rm -rf "$tmp"' EXIT
  export GITHUB_STEP_SUMMARY=/dev/null
  bad=0
  mk_src() { mkdir -p "$1/tests/ipc"; printf '// 假源码\n' > "$1/tests/ipc/test_x.cpp"; }
  mk_exe() { # $1 构建目录 $2 shell 脚本正文
    mkdir -p "$1"
    printf '#!/bin/sh\n%s\n' "$2" > "$1/rinne_ipc_tests"
    chmod +x "$1/rinne_ipc_tests"
  }
  expect() { # $1 说明 $2 ok|bad|skip $3 仓根 $4 构建目录
    local got rc
    got=$(probe "$3" "$4" "自测" 2>&1)
    rc=$?
    if [ "$2" = "ok" ] && [ "$rc" = 0 ] && ! printf '%s' "$got" | grep -q 'SKIP'; then echo "PASS  $1"
    elif [ "$2" = "skip" ] && [ "$rc" = 0 ] && printf '%s' "$got" | grep -q 'SKIP'; then echo "PASS  $1"
    elif [ "$2" = "bad" ] && [ "$rc" != 0 ]; then echo "PASS  $1"
    else echo "FAIL  ${1}（期望 ${2}，退出码 ${rc}）"; printf '%s\n' "$got" | sed 's/^/      | /'; bad=1; fi
  }
  good='case " $* " in *" -w NoAssertions "*) ;; *) echo "missing -w NoAssertions"; exit 9 ;; esac
case " $* " in *" [shm-leftover] "*) ;; *) echo "missing [shm-leftover]"; exit 9 ;; esac
echo "All tests passed (1 assertion in 1 test case)"
exit 0'
  zero='case " $* " in *" -w NoAssertions "*) echo "test cases: 1 | 1 failed"; echo "assertions: 0 | 0 failed"; exit 1 ;; esac
echo "All tests passed (0 assertions in 1 test case)"
exit 0'
  # 源码缺失时即使构建目录里有可执行文件也 SKIP：SKIP 的决定只看源码
  mk_exe "$tmp/b0" "$good"
  expect "源码缺失时 SKIP" skip "$tmp/r0" "$tmp/b0"
  # 有源码却没有可执行文件：失败，不是 SKIP
  mk_src "$tmp/r1"
  expect "有源码但没有构建出 rinne_ipc_tests 判错" bad "$tmp/r1" "$tmp/b1"
  # 正常跑通；good 里会核对 -w NoAssertions 与 [shm-leftover] 两个参数
  mk_src "$tmp/r2"
  mk_exe "$tmp/b2" "$good"
  expect "有源码且参数正确时干净" ok "$tmp/r2" "$tmp/b2"
  # 零断言的用例：脚本传了 -w NoAssertions 才会红，删掉这个参数就抓不到
  mk_src "$tmp/r3"
  mk_exe "$tmp/b3" "$zero"
  expect "零断言的用例判错" bad "$tmp/r3" "$tmp/b3"
  mk_src "$tmp/r4"
  mk_exe "$tmp/b4" 'echo "No test cases matched"; exit 3'
  expect "没有匹配 [shm-leftover] 的用例判错" bad "$tmp/r4" "$tmp/b4"
  mk_src "$tmp/r5"
  mk_exe "$tmp/b5" 'echo "a.cpp:1: FAILED:"; echo "assertions: 1 | 0 passed | 1 failed"; exit 1'
  expect "有用例失败时判错" bad "$tmp/r5" "$tmp/b5"
  [ "$bad" = 0 ] || { echo "shm-leftover 自测失败"; exit 1; }
  echo "shm-leftover 自测通过"
  exit 0
fi

probe "$root" "$1" "$2" || exit 1
