#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 零分配计数器的反例：计数器漏掉任何一种原语，对应用例必须变红。
#   alloc-canary.sh <宿主测试可执行文件>
#   alloc-canary.sh --self-test
# 先不致盲跑一遍，必须全绿且没有被 SKIP；再让计数器依次对 malloc、calloc、realloc、free 视而不见
# （环境变量 RINNE_TEST_ALLOC_BLIND，只有测试辅助 AllocationCounter 读它），每一遍都必须失败，
# 而且失败的 section 里要有以这个原语命名的那一个，不能是别的原因红的。

set -uo pipefail

# $1 可执行文件；返回 0 表示反例都按预期变红
run_canaries() {
  local exe=$1 out rc p fail=0 here
  here=$(cd "$(dirname "$0")" && pwd)
  out=$(mktemp "${TMPDIR:-/tmp}/rinne.XXXXXX")
  rc=0
  "$exe" "[allocation]" > "$out" 2>&1 || rc=$?
  if [ "$rc" != "0" ]; then cat "$out"; echo "::error::不致盲时计数器用例就失败了（退出码 $rc）"; rm -f "$out"; return 1; fi
  if grep -qi 'skipped' "$out"; then cat "$out"; echo "::error::计数器用例被 SKIP 了，没有真正断言"; rm -f "$out"; return 1; fi
  echo "不致盲：计数器用例全部通过"
  for p in malloc calloc realloc free; do
    rc=0
    RINNE_TEST_ALLOC_BLIND=$p "$exe" "[allocation]" > "$out" 2>&1 || rc=$?
    # Catch2 v3 的退出码是失败断言数（2、3、4 同时表示没跑用例、过滤没匹配、全部 SKIP），所以不看退出码的大小，
    # 要求输出里有失败断言，且不是崩溃或未捕获异常
    if [ "$rc" = "0" ]; then
      tail -n 40 "$out"
      echo "::error::对 $p 致盲后期望用例失败，实际通过了"
      fail=1
    elif ! grep -q ': FAILED:' "$out" || grep -qE 'fatal error condition|unexpected exception' "$out"; then
      tail -n 40 "$out"
      echo "::error::对 $p 致盲后退出码是 $rc，但输出里没有失败断言，或者是崩溃：不能算用例按预期变红"
      fail=1
    elif ! grep -qx "  $p" "$out"; then
      tail -n 40 "$out"
      echo "::error::对 $p 致盲后有用例失败，但失败的里面没有名为 $p 的 section"
      fail=1
    else
      echo "对 $p 致盲：用例按预期变红（退出码 $rc，section $p 在失败之列）"
    fi
  done
  rm -f "$out"
  return "$fail"
}

if [ "${1:-}" = "--self-test" ]; then
  tmp=$(mktemp -d "${TMPDIR:-/tmp}/rinne.XXXXXX")
  trap 'rm -rf "$tmp"' EXIT
  # 假的测试程序：good 对致盲有反应（失败并点名 section），deaf 对致盲没反应，wrong 失败但点的不是对应的 section，
  # silent 致盲后什么都没跑
  cat > "$tmp/good" <<'SH'
#!/usr/bin/env bash
if [ -n "${RINNE_TEST_ALLOC_BLIND:-}" ]; then printf 'case\n  %s\nf.cpp:1: FAILED:\n' "$RINNE_TEST_ALLOC_BLIND"; exit 2; fi
echo "All tests passed"
SH
  cat > "$tmp/deaf" <<'SH'
#!/usr/bin/env bash
echo "All tests passed"
SH
  cat > "$tmp/wrong" <<'SH'
#!/usr/bin/env bash
if [ -n "${RINNE_TEST_ALLOC_BLIND:-}" ]; then printf 'case\n  something else\nf.cpp:1: FAILED:\n'; exit 2; fi
echo "All tests passed"
SH
  cat > "$tmp/silent" <<'SH'
#!/usr/bin/env bash
if [ -n "${RINNE_TEST_ALLOC_BLIND:-}" ]; then echo "No test cases matched"; exit 2; fi
echo "All tests passed"
SH
  cat > "$tmp/skipped" <<'SH'
#!/usr/bin/env bash
echo "SKIPPED"
SH
  chmod +x "$tmp"/*
  bad=0
  expect() { # $1 说明 $2 期望退出码 $3 程序
    local rc=0
    run_canaries "$3" > /dev/null 2>&1 || rc=$?
    if [ "$rc" != "$2" ]; then echo "FAIL  $1（期望 $2，实际 $rc）"; bad=1; else echo "PASS  $1"; fi
  }
  expect "致盲后点名失败的计数器：通过" 0 "$tmp/good"
  expect "致盲后仍然全绿的计数器：红" 1 "$tmp/deaf"
  expect "失败但没点名对应 section：红" 1 "$tmp/wrong"
  expect "致盲后什么都没跑（退出码 2，无失败断言）：红" 1 "$tmp/silent"
  expect "用例被 SKIP：红" 1 "$tmp/skipped"
  [ "$bad" = 0 ] || { echo "alloc-canary 自测失败"; exit 1; }
  echo "alloc-canary 自测通过"
  exit 0
fi

run_canaries "$1"
