#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/mac-ci.json
#
# AU 的安装、auval 验证与 auval -a 唯一性。三元组一行一个（类型 子类型 厂商），由 check-bundles.mjs --triples 写出。
#   au-gate.sh install  <bundle 列表> <装好的路径列表>   把列表里的 .component 装进用户的 Components 目录并让注册器重扫
#   au-gate.sh validate <三元组文件> <日志目录>          每个三元组跑 auval -v；输出含 AU VALIDATION SUCCEEDED 且退出码 0 才算过
#   au-gate.sh unique   <三元组文件> <日志目录>          auval -a 的输出先落文件，再数每个冻结三元组出现几行，必须恰好一行
#   au-gate.sh --self-test                              判据自测，不调 auval
# 冻结的正式三元组写死在本脚本里（validate 与 unique 都对着它核，不信三元组文件）：check-bundles 的三元组是从构建产物与
# golden 读出来的，改码时一路自洽、不会变红，只有对着冻结值核，「子类型写错」才红。
# 探针构建不走本脚本（探针身份只验 bundle，不装进系统）。

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

# 构建产物声明的三元组必须正好等于冻结值。0 = 一致；1 = 不一致（原因打印出来，由调用方决定怎么退出）
assert_frozen() { # $1 三元组文件
  local declared want
  declared=$(LC_ALL=C sort "$1")
  want=$(printf '%s\n' "$FROZEN" | LC_ALL=C sort)
  if [ "$declared" != "$want" ]; then
    echo "::error::构建产物声明的 AU 三元组 [$(printf '%s' "$declared" | tr '\n' ',')] 与冻结的 [$(printf '%s' "$want" | tr '\n' ',')] 不一致"
    return 1
  fi
}

# 遍历的是冻结值，不是三元组文件：每个冻结三元组在 auval -a 输出里必须恰好 1 行。0 = 都是 1 行；1 = 有不是的
unique_check() { # $1 auval -a 的输出文件 $2 摘要文件（可省）
  local type code mfr k fail=0 sum=${2:-/dev/null}
  while read -r type code mfr <&3; do
    [ -n "$type" ] || continue
    k=$(count_lines "$type" "$code" "$mfr" "$1")
    echo "$type $code ${mfr}：auval -a 里 $k 行"
    echo "- \`auval -a\` 里 \`$type $code $mfr\`：$k 行" >> "$sum"
    if [ "$k" != "1" ]; then echo "::error::$type $code $mfr 在 auval -a 里是 $k 行，应当恰好 1 行"; fail=1; fi
  done 3<<< "$FROZEN"
  return "$fail"
}

self_test() {
  local dir bad=0 rc out
  dir=$(mktemp -d "${TMPDIR:-/tmp}/rinne.XXXXXX")
  # rinne-delete-ok: 自测里 mktemp -d 新建的目录
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

  # unique 遍历的是冻结值：两个冻结三元组各恰好一行才过；三元组文件里写什么不影响它
  rc=0; out=$(unique_check "$dir/a.txt") || rc=$?
  expect "两个冻结三元组各一行通过" 0 "$rc"
  rc=0; out=$(unique_check "$dir/dup.txt") || rc=$?
  expect "一个挂了两个组件、一个没注册上判红" 1 "$rc"
  case "$out" in
    *"aufx Rnni Snch 在 auval -a 里是 2 行"*"aufx Rnno Snch 在 auval -a 里是 0 行"*) expect "红的原因点名两个三元组和行数" ok ok ;;
    *) expect "红的原因点名两个三元组和行数" ok "$out" ;;
  esac
  printf '  aufx Rnni Snch  -  a\n  aufx Rnno Snch  -  b\n  aufx Rnno Snch  -  c\n' > "$dir/one-dup.txt"
  rc=0; out=$(unique_check "$dir/one-dup.txt") || rc=$?
  expect "只有 Output 重复也判红" 1 "$rc"
  printf '  aufx Rnni Snch  -  a\n' > "$dir/one-missing.txt"
  rc=0; out=$(unique_check "$dir/one-missing.txt") || rc=$?
  expect "只缺 Output 判红" 1 "$rc"
  printf '  aufx Rnni Snch  -  a\n  aufx Rnno Snch  -  b\n  aufx Zzzz Snch  -  other\n  aufx Zzzz Snch  -  other\n' > "$dir/extra.txt"
  rc=0; out=$(unique_check "$dir/extra.txt") || rc=$?
  expect "冻结值之外的三元组不参与计数" 0 "$rc"
  rc=0; out=$(unique_check "$dir/a.txt" "$dir/summary.md") || rc=$?
  expect "给了摘要文件就追加两行" 2 "$(grep -c . "$dir/summary.md")"

  # 三元组文件必须等于冻结值：多一个、少一个、子类型写错都不行
  printf 'aufx Rnni Snch\naufx Rnno Snch\n' > "$dir/t-ok.txt"
  rc=0; out=$(assert_frozen "$dir/t-ok.txt") || rc=$?
  expect "声明的三元组等于冻结值通过" 0 "$rc"
  printf 'aufx Rnno Snch\naufx Rnni Snch\n' > "$dir/t-order.txt"
  rc=0; out=$(assert_frozen "$dir/t-order.txt") || rc=$?
  expect "顺序不同不影响" 0 "$rc"
  printf 'aufx Rnni Snch\n' > "$dir/t-less.txt"
  rc=0; out=$(assert_frozen "$dir/t-less.txt") || rc=$?
  expect "少一个判红" 1 "$rc"
  printf 'aufx Rnni Snch\naufx Rnno Snch\naufx Rnxx Snch\n' > "$dir/t-more.txt"
  rc=0; out=$(assert_frozen "$dir/t-more.txt") || rc=$?
  expect "多一个判红" 1 "$rc"
  printf 'aufx Rnni Snch\naufx Rnnp Snch\n' > "$dir/t-wrong.txt"
  rc=0; out=$(assert_frozen "$dir/t-wrong.txt") || rc=$?
  expect "子类型写错判红" 1 "$rc"

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
        case "$inst" in
          "$HOME/Library/Audio/Plug-Ins/Components/"?*.component) ;;
          *) echo "::error::拒绝删除 ${inst}：不是已装目录下一层的 .component"; exit 1 ;;
        esac
        # case 里的 ?* 也匹配 /，所以再核对一次父目录正好是 Components
        if [ "$(dirname -- "$inst")" != "$dest" ]; then
          echo "::error::拒绝删除 ${inst}：不是已装目录下一层"; exit 1
        fi
        # rinne-delete-ok: 上面核对过：只删 Components 目录下一层、名字以 .component 结尾的旧包
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
  local triples=$1 logs=$2 type code mfr seen rc v fail=0
  mkdir -p "$logs"
  assert_frozen "$triples" || exit 1
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
  local triples=$1 logs=$2 dups fail=0
  mkdir -p "$logs"
  assert_frozen "$triples" || fail=1
  dups=$(LC_ALL=C sort "$triples" | uniq -d)
  if [ -n "$dups" ]; then echo "::error::有三元组被不止一个 bundle 声明：$(printf '%s' "$dups" | tr '\n' ',')"; fail=1; fi
  # 输出先落文件再匹配：直接管道进 grep，auval 被提前关掉时会拿到半截输出
  auval -a </dev/null > "$logs/auval-list.txt" 2>&1 || true
  if [ ! -s "$logs/auval-list.txt" ]; then echo "::error::auval -a 没有输出"; exit 1; fi
  unique_check "$logs/auval-list.txt" "${GITHUB_STEP_SUMMARY:-/dev/null}" || fail=1
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
