#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# arm64 原生压测与变异构建。rinne_stress 由 IPC 与环路的后续工作提供；目标不存在时打印 SKIP 并通过，存在时自动生效。
#   stress.sh <工作目录>
# 在仓根执行。core-only 配置（不需要 JUCE）：
#   1. 常规构建跑 rinne_stress，必须通过；
#   2. RINNE_STRESS_MUTATE=no_read_fence（去掉读侧 fence）与 no_intent（去掉 write_intent）各构建一次，
#      跑同一个压测，必须失败：变异通过说明压测抓不住这种内存序缺陷，判红（经 expect-red.sh 反转）。
# 常规构建在 arm64 上跑，弱内存序下的缺陷才暴露得出来；x86 的强序会掩盖它们。

set -euo pipefail

work=${1:-build-stress}
here=$(cd "$(dirname "$0")" && pwd)

configure() { # $1 构建目录 其余 额外的 cmake 参数
  local dir=$1
  shift
  cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DRINNE_CORE_ONLY=ON "$@"
}

# 目标有没有，和 tests/CMakeLists.txt 的 rinne_add_test_suite 用同一个判据：tests/stress 下有没有 .cpp
if ! find tests/stress -name '*.cpp' -print -quit 2>/dev/null | grep -q .; then
  echo "::notice title=stress::SKIP: rinne_stress not built yet, nothing to run"
  echo "- 压测与变异构建：SKIP（rinne_stress not built yet）" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
  exit 0
fi

configure "$work"
cmake --build "$work" --target rinne_stress --parallel
ctest --test-dir "$work" -R '^rinne_stress$' --output-on-failure --no-tests=error -V
echo "- 压测（arm64 原生）：通过" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"

for mutation in no_read_fence no_intent; do
  dir="$work-$mutation"
  configure "$dir" "-DRINNE_STRESS_MUTATE=$mutation"
  cmake --build "$dir" --target rinne_stress --parallel
  exe=$(find "$dir" -type f -name rinne_stress -perm -u+x -not -path '*/_deps/*' -print -quit)
  [ -n "$exe" ] || { echo "::error::变异构建 $mutation 里找不到 rinne_stress"; exit 1; }
  # --timeout 与 ctest 的 TIMEOUT 600 同值：变异挂住时单次运行就结束，不拖到 job 超时
  bash "$here/expect-red.sh" --catch2 --timeout 600 "变异 $mutation" -- "$exe"
  echo "- 变异构建 ${mutation}：压测按预期变红" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
done
