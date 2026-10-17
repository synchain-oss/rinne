#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 进程外 AU 自测：构建 tests/oop 的宿主，对每个已装好的 AU 用 AUHostingService 做进程外实例化并渲染 1 秒。
#   oop-run.sh <三元组文件> <构建目录>
# 在仓根执行；AU 必须已经装进用户的 Components 目录（au-gate.sh install）。先跑宿主自己的自测。
# 这一步是 build-mac 里的非门禁步骤（continue-on-error），红了只在 summary 里留痕，不挡合并。

set -euo pipefail

triples=$1
dir=$2

cmake -S tests/oop -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$dir" --parallel
host="$dir/rinne_auhost"
"$host" --self-test

fail=0
while read -r type code mfr <&3; do
  [ -n "$type" ] || continue
  rc=0
  "$host" --type "$type" --subtype "$code" --manufacturer "$mfr" --seconds 1 || rc=$?
  if [ "$rc" = "0" ]; then
    echo "- 进程外 \`$type $code $mfr\`：通过" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
  else
    echo "::error::进程外自测 $type $code $mfr 失败，退出码 $rc"
    echo "- 进程外 \`$type $code $mfr\`：**失败**（退出码 ${rc}）" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
    fail=1
  fi
done 3< "$triples"
exit "$fail"
