#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# POSIX 共享内存残段检查：验证前后各跑一次，会合段与数据段不许有残留。
#   shm-leftover.sh <构建目录> <说明>
# mac 上 POSIX 共享内存段没有可列目录，只能由认得段名的代码去探：跨进程契约套件 rinne_ipc_tests 里带 [shm-leftover] 标签的
# 用例负责（按命名模块生成全部会合段与数据段的名字，逐个 shm_open 探，存在就失败；名字由命名模块决定，本脚本不写死）。套件还没有时打印 SKIP 注记并通过；
# 套件有了却没有这个标签的用例，是契约被改丢了，判红，不悄悄变成空检查。

set -uo pipefail

build=$1
label=$2

exe=$(find "$build" -type f -name rinne_ipc_tests -perm -u+x -not -path '*/_deps/*' -print -quit 2>/dev/null || true)
if [ -z "$exe" ]; then
  echo "::notice title=shm-leftover::SKIP: rinne_ipc_tests not built yet, nothing to probe ($label)"
  echo "- 残段检查（$label）：SKIP，rinne_ipc_tests 还没有" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
  exit 0
fi

log=$(mktemp "${TMPDIR:-/tmp}/rinne.XXXXXX")
rc=0
"$exe" "[shm-leftover]" </dev/null > "$log" 2>&1 || rc=$?
cat "$log"
if grep -q 'No test cases matched' "$log"; then
  echo "::error::rinne_ipc_tests 里没有带 [shm-leftover] 标签的用例（被改名或改了标签？）"
  rm -f "$log"
  exit 1
fi
rm -f "$log"
if [ "$rc" != "0" ]; then
  echo "::error::残段检查（$label）失败，退出码 $rc：有共享内存段残留，列表见上"
  echo "- 残段检查（$label）：**有残留**" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
  exit 1
fi
echo "残段检查（$label）：干净"
echo "- 残段检查（$label）：干净" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
