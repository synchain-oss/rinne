#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/mac-ci.json
#
# AU 的安装、auval 验证与 auval -a 唯一性。三元组一行一个（类型 子类型 厂商），由 check-bundles.mjs --triples 写出。
#   au-gate.sh install  <bundle 列表> <装好的路径列表>   把列表里的 .component 装进用户的 Components 目录并让注册器重扫
#   au-gate.sh validate <三元组文件> <日志目录>          每个三元组跑 auval -v；输出含 AU VALIDATION SUCCEEDED 且退出码 0 才算过
#   au-gate.sh unique   <三元组文件> <日志目录>          auval -a 的输出先落文件，再数每个三元组出现几行，必须恰好一行
#   au-gate.sh --self-test                              判据自测，不调 auval
# 冻结的正式三元组写死在 validate 里：check-bundles 的三元组是从构建产物与 golden 读出来的，改码时一路自洽、不会变红，
# 只有对着冻结值核，「子类型写错」才红。探针构建不走本脚本（探针身份只验 bundle，不装进系统）。

set -euo pipefail

FROZEN=$'aufx Rnni Snch\naufx Rnno Snch'

# auval 的判决：退出码 0 且输出里有成功标记，缺一不可（auval 的退出码历史上不可靠，所以两样都看）
verdict() { # $1 退出码 $2 输出文件
  if [ "$1" = "0" ] && grep -q 'AU VALIDATION SUCCEEDED' "$2"; then echo SUCCEEDED; else echo FAILED; fi
}

# 一个三元组在 auval -a 输出里占几行
count_lines() { # $1 类型 $2 子类型 $3 厂商 $4 输出文件
  local n
  n=$(grep -cE "^[[:space:]]*$1[[:space:]]+$2[[:space:]]+$3([[:space:]]|$)" "$4" || true)
  echo "${n:-0}"
}

self_test() {
  local dir bad=0
  dir=$(mktemp -d "${TMPDIR:-/tmp}/rinne.XXXXXX")
  trap 'rm -rf "$dir"' RETURN
  expect() { # $1 说明 $2 期望 $3 实际
    if [ "$2" != "$3" ]; then echo "FAIL  ${1}（期望 ${2}，实际 ${3}）"; bad=1; else echo "PASS  $1"; fi
  }
  printf 'AU VALIDATION SUCCEEDED.\n' > "$dir/ok.log"
  expect "成功标记加退出码 0 通过" SUCCEEDED "$(verdict 0 "$dir/ok.log")"
  expect "成功标记但退出码非 0 判红" FAILED "$(verdict 1 "$dir/ok.log")"
  printf 'AU VALIDATION FAILED.\n' > "$dir/fail.log"
  expect "退出码 0 但没有成功标记判红" FAILED "$(verdict 0 "$dir/fail.log")"
  : > "$dir/empty.log"
  expect "输出为空判红" FAILED "$(verdict 0 "$dir/empty.log")"

  printf '  aufx Rnni Snch  -  Synchain: Rinne Input\n  aufx Rnno Snch  -  Synchain: Rinne Output\n  aufx Abcd Appl  -  Apple: Other\n' > "$dir/a.txt"
  expect "各出现一行的三元组数得出 1" 1 "$(count_lines aufx Rnni Snch "$dir/a.txt")"
  printf '  aufx Rnni Snch  -  Synchain: Rinne Input\n  aufx Rnni Snch  -  Synchain: Rinne Input\n' > "$dir/dup.txt"
  expect "同一三元组挂了两个组件数得出 2" 2 "$(count_lines aufx Rnni Snch "$dir/dup.txt")"
  expect "没注册上数得出 0" 0 "$(count_lines aufx Rnno Snch "$dir/dup.txt")"
  printf '  aufx Rnnix Snch  -  other\n' > "$dir/prefix.txt"
  expect "子类型只是前缀相同不算" 0 "$(count_lines aufx Rnni Snch "$dir/prefix.txt")"

  [ "$bad" = 0 ] || { echo "au-gate 自测失败"; return 1; }
  echo "au-gate 自测通过"
}

install_au() {
  local list=$1 out=$2 dest b inst n
  dest="$HOME/Library/Audio/Plug-Ins/Components"
  mkdir -p "$dest"
  : > "$out"
  while IFS= read -r b; do
    case "$b" in
      *.component)
        # 先删旧的：ditto 往已有目录里是合并，不会删掉多余的文件
        inst="$dest/$(basename "$b")"
        rm -rf "$inst"
        ditto "$b" "$inst"
        printf '%s\n' "$inst" >> "$out" ;;
    esac
  done < "$list"
  n=$(grep -c . "$out" || true)
  if [ "${n:-0}" != "2" ]; then echo "::error::装进去的 .component 有 ${n:-0} 个，应当是 2 个"; exit 1; fi
  killall -9 AudioComponentRegistrar 2>/dev/null || true
  sed 's/^/installed: /' "$out"
}

validate() {
  local triples=$1 logs=$2 declared want type code mfr seen rc v fail=0
  mkdir -p "$logs"
  declared=$(LC_ALL=C sort "$triples")
  want=$(printf '%s\n' "$FROZEN" | LC_ALL=C sort)
  if [ "$declared" != "$want" ]; then
    echo "::error::构建产物声明的 AU 三元组 [$(printf '%s' "$declared" | tr '\n' ',')] 与冻结的 [$(printf '%s' "$want" | tr '\n' ',')] 不一致"
    exit 1
  fi
  while read -r type code mfr <&3; do
    [ -n "$type" ] || continue
    # 装好之后注册器要一会儿才认到；最多等 30 秒，等不到只告警，判决交给 auval -v
    seen=0
    for _ in $(seq 1 30); do
      auval -a </dev/null > "$logs/auval-list-poll.txt" 2>/dev/null || true
      if [ "$(count_lines "$type" "$code" "$mfr" "$logs/auval-list-poll.txt")" -ge 1 ]; then seen=1; break; fi
      sleep 1
    done
    if [ "$seen" != "1" ]; then echo "::warning::$type $code $mfr 30 秒内没有出现在 auval -a 里，仍然跑 auval -v"; fi
    rc=0
    auval -v "$type" "$code" "$mfr" </dev/null > "$logs/auval-$code.log" 2>&1 || rc=$?
    v=$(verdict "$rc" "$logs/auval-$code.log")
    echo "::group::auval -v $type $code ${mfr}（退出码 ${rc}，${v}）"
    cat "$logs/auval-$code.log"
    echo "::endgroup::"
    echo "- \`auval -v $type $code $mfr\`：退出码 ${rc}，$v" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
    if [ "$v" != "SUCCEEDED" ]; then echo "::error::auval -v $type $code $mfr 未通过（退出码 ${rc}）"; fail=1; fi
  done 3<<< "$FROZEN"
  exit "$fail"
}

unique() {
  local triples=$1 logs=$2 type code mfr k dups fail=0
  mkdir -p "$logs"
  dups=$(LC_ALL=C sort "$triples" | uniq -d)
  if [ -n "$dups" ]; then echo "::error::有三元组被不止一个 bundle 声明：$(printf '%s' "$dups" | tr '\n' ',')"; fail=1; fi
  # 输出先落文件再匹配：直接管道进 grep，auval 被提前关掉时会拿到半截输出
  auval -a </dev/null > "$logs/auval-list.txt" 2>&1 || true
  if [ ! -s "$logs/auval-list.txt" ]; then echo "::error::auval -a 没有输出"; exit 1; fi
  while read -r type code mfr <&3; do
    [ -n "$type" ] || continue
    k=$(count_lines "$type" "$code" "$mfr" "$logs/auval-list.txt")
    echo "$type $code ${mfr}：auval -a 里 $k 行"
    echo "- \`auval -a\` 里 \`$type $code $mfr\`：$k 行" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
    if [ "$k" != "1" ]; then echo "::error::$type $code $mfr 在 auval -a 里是 $k 行，应当恰好 1 行"; fail=1; fi
  done 3< "$triples"
  echo "::group::auval -a 里厂商码相同的行"
  awk '$3 == "Snch"' "$logs/auval-list.txt" || true
  echo "::endgroup::"
  exit "$fail"
}

case "${1:-}" in
  --self-test) self_test ;;
  install) install_au "$2" "$3" ;;
  validate) validate "$2" "$3" ;;
  unique) unique "$2" "$3" ;;
  *) echo "用法：au-gate.sh install|validate|unique ... 或 --self-test" >&2; exit 2 ;;
esac
