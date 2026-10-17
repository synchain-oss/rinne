#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 常驻的 sanitizer 金丝雀：把三个故意有问题的小程序按 sanitizer 配置编译并运行，每个都必须以非 0 退出，
# 并且输出里有对应的报告。哪个不报，这一步就失败：说明那个配置没有真的在检查，core-sanitizers 的绿就不可信。
# 程序之间、与 rinne_core 之间互相独立，不进 ctest 的正式套件。
#   bash tests/sanitizer-canary/run.sh asan_ubsan | tsan
set -uo pipefail

config="${1:-}"
cxx="${CXX:-clang++-18}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

case "$config" in
  asan_ubsan) cases=("heap_overflow|-fsanitize=address|AddressSanitizer: heap-buffer-overflow"
                     "signed_overflow|-fsanitize=undefined|runtime error:") ;;
  tsan)       cases=("race|-fsanitize=thread|ThreadSanitizer: data race") ;;
  *) echo "用法：run.sh asan_ubsan | tsan" >&2; exit 2 ;;
esac

failed=0
for c in "${cases[@]}"; do
  IFS='|' read -r name flag pattern <<<"$c"
  if ! "$cxx" -std=c++17 -O0 -g -pthread "$flag" -fno-sanitize-recover=all "$here/$name.cpp" -o "$out/$name"; then
    echo "金丝雀 $name：编译失败"; failed=1; continue
  fi
  "$out/$name" >"$out/$name.log" 2>&1
  status=$?
  if [ "$status" -eq 0 ]; then
    echo "金丝雀 $name：退出码 0，sanitizer 没有拦住"; failed=1
  elif ! grep -q "$pattern" "$out/$name.log"; then
    echo "金丝雀 $name：退出码 $status，但输出里没有「$pattern」"; sed 's/^/    /' "$out/$name.log" | head -20; failed=1
  else
    echo "金丝雀 $name：已报告「$pattern」，退出码 $status"
  fi
done
exit "$failed"
