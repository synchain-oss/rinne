#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/mac-ci.json
#
# mac 上的 pluginval 门：2 个 .vst3（构建树里的那份）加 2 个已装的 .component（按安装后的路径验），strictness 5，不开 GUI。
#   pluginval-gate.sh <bundle 列表> <已装 AU 列表> <日志目录>
#   pluginval-gate.sh --self-test                              判据自测，不下载、不跑 pluginval
# 版本只从仓根 .pluginval-version 读；包的 sha256 登记在同目录的 pluginval-macos.sha256（首次下载的值，上游不发校验文件），
# 版本与登记不一致时停下，不凑合。每格判据：退出码 0，输出里有单独一行 SUCCESS。四格都跑完再统一判。

set -euo pipefail

# 一格的判决：退出码 0，且输出里有单独一行 SUCCESS，缺一不可。打印 PASS 或 FAIL
cell_verdict() { # $1 退出码 $2 输出文件
  if [ "$1" = "0" ] && grep -qx 'SUCCESS' "$2"; then echo PASS; else echo FAIL; fi
}

# 版本号要作为一个完整的词出现在 --version 的输出里（子串匹配会让 1.0.40 或 1.0.4-rc1 冒充 1.0.4）。0 = 对得上
version_matches() { # $1 钉版标签（带不带 v 都行） $2 --version 的输出
  local want escaped pattern
  want=${1#v}
  escaped=$(printf '%s' "$want" | sed 's/\./\\./g')
  pattern="(^|[^0-9A-Za-z.-])v?${escaped}([^0-9A-Za-z.-]|$)"
  grep -Eq "$pattern" <<< "$2"
}

self_test() {
  local dir bad=0 rc
  dir=$(mktemp -d "${TMPDIR:-/tmp}/rinne.XXXXXX")
  # rinne-delete-ok: 自测里 mktemp -d 新建的目录
  trap 'rm -rf "$dir"' RETURN
  expect() { # $1 说明 $2 期望 $3 实际
    if [ "$2" != "$3" ]; then echo "FAIL  ${1}（期望 ${2}，实际 ${3}）"; bad=1; else echo "PASS  $1"; fi
  }
  # 每个用例：说明、退出码、输出内容（printf 格式）、期望的判决。SUCCESS 必须独占一行
  cell() { # $1 说明 $2 退出码 $3 输出 $4 期望
    printf "$3" > "$dir/cell.log"
    expect "$1" "$4" "$(cell_verdict "$2" "$dir/cell.log")"
  }
  cell "退出码 0 加单独一行 SUCCESS 通过" 0 'SUCCESS\n' PASS
  cell "SUCCESS 夹在别的输出中间也通过" 0 'Starting tests\nSUCCESS\nDone\n' PASS
  cell "最后一行 SUCCESS 没有换行结尾也通过" 0 'SUCCESS' PASS
  cell "有 SUCCESS 行但退出码 1 判红" 1 'SUCCESS\n' FAIL
  cell "有 SUCCESS 行但崩溃的退出码 139 判红" 139 'SUCCESS\n' FAIL
  cell "退出码 0 但输出里没有 SUCCESS 判红" 0 'FAILURE\nTests FAILED\n' FAIL
  cell "退出码 0 但输出为空判红" 0 '' FAIL
  cell "SUCCESS 前面有字不算单独一行" 0 'NOT SUCCESS\n' FAIL
  cell "SUCCESS 后面有字不算单独一行" 0 'SUCCESS rate: 0%%\n' FAIL
  cell "SUCCESS 前面有空格不算单独一行" 0 '  SUCCESS\n' FAIL
  cell "小写 success 不算" 0 'success\n' FAIL

  version_matches v1.0.4 'pluginval 1.0.4' && rc=0 || rc=$?
  expect "自报版本前面有别的词对得上" 0 "$rc"
  version_matches v1.0.4 '1.0.4' && rc=0 || rc=$?
  expect "自报版本就是版本号对得上" 0 "$rc"
  version_matches v1.0.4 'pluginval v1.0.4 (c) Tracktion' && rc=0 || rc=$?
  expect "自报版本带 v 且后面有别的字对得上" 0 "$rc"
  version_matches 1.0.4 'pluginval 1.0.4' && rc=0 || rc=$?
  expect "钉版标签不带 v 也行" 0 "$rc"
  version_matches v1.0.4 'pluginval 1.0.40' && rc=0 || rc=$?
  expect "1.0.40 冒充 1.0.4 判红" 1 "$rc"
  version_matches v1.0.4 'pluginval 1.0.4-rc1' && rc=0 || rc=$?
  expect "1.0.4-rc1 冒充 1.0.4 判红" 1 "$rc"
  version_matches v1.0.4 'pluginval 11.0.4' && rc=0 || rc=$?
  expect "11.0.4 冒充 1.0.4 判红" 1 "$rc"
  version_matches v1.0.4 'pluginval 1x0x4' && rc=0 || rc=$?
  expect "点号不当通配符" 1 "$rc"
  version_matches v1.0.4 '' && rc=0 || rc=$?
  expect "没有输出判红" 1 "$rc"

  [ "$bad" = 0 ] || { echo "pluginval-gate 自测失败"; return 1; }
  echo "pluginval-gate 自测通过"
}

if [ "${1:-}" = "--self-test" ]; then self_test; exit $?; fi

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
bundles=$1
installed=$2
logs=$3
mkdir -p "$logs"
logs=$(cd "$logs" && pwd)

tag=$(tr -d '[:space:]' < "$root/.pluginval-version")
read -r pinned_tag pinned_sha < "$here/pluginval-macos.sha256"
if [ -z "$tag" ] || [ "$tag" != "$pinned_tag" ]; then
  echo "::error::.pluginval-version 是 '$tag'，pluginval-macos.sha256 登记的是 '$pinned_tag'：升版本要同时补新版本的 sha256"
  exit 1
fi

# 每次都在新建的目录里干活，不去清旧目录；退出时把这次新建的目录删掉，别在开发机上攒下来
work_root="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/rinne.XXXXXX")"
# 「走完了」标记（约定写法，selftest.sh 与本机 gate 会扫）：macOS 自带的 bash 3.2 在变量展开出错时会静默结束脚本，
# 装了 EXIT trap 时进入 trap 的 $? 常常是 0，这道闸门就会在半路死掉之后静默放行。所以只有走到末尾才把标记置 1，
# trap 里退出码为 0 而标记没置位，就改成非 0（上面和下面的 exit 1 本来就是非 0，不受影响）。
RINNE_RAN_TO_END=0
cleanup() {
  local rc=$?
  # rinne-delete-ok: 本脚本 mktemp -d 新建的目录，退出时只删它自己
  rm -rf "$work_root" || true
  if [ "$rc" -eq 0 ] && [ "$RINNE_RAN_TO_END" != 1 ]; then
    echo "::error::pluginval-gate.sh：脚本没有走完就退出了（bash 3.2 下变量展开出错会静默结束脚本，退出码是 0）：按失败处理，闸门不能放行"
    exit 1
  fi
  exit $rc
}
trap cleanup EXIT
work="$work_root/pluginval"
mkdir -p "$work"
curl -fsSL --retry 3 --retry-delay 5 -o "$work/pluginval_macOS.zip" \
  "https://github.com/Tracktion/pluginval/releases/download/$tag/pluginval_macOS.zip"
got=$(shasum -a 256 "$work/pluginval_macOS.zip" | awk '{ print $1 }')
if [ "$got" != "$pinned_sha" ]; then
  echo "::error::pluginval_macOS.zip 的 sha256 是 ${got}，登记的是 $pinned_sha"
  exit 1
fi
unzip -q "$work/pluginval_macOS.zip" -d "$work"
exe="$work/pluginval.app/Contents/MacOS/pluginval"
[ -f "$exe" ] || { echo "::error::压缩包里没有 pluginval.app/Contents/MacOS/pluginval"; exit 1; }
chmod +x "$exe"
ver=$("$exe" --version </dev/null 2>&1 || true)
echo "pluginval --version：$ver"
if ! version_matches "$tag" "$ver"; then
  echo "::error::下载的 pluginval 自报版本 '$ver'，.pluginval-version 是 $tag"
  exit 1
fi

targets="$logs/targets.txt"
: > "$targets"
while IFS= read -r b; do
  case "$b" in *.vst3) printf 'VST3\t%s\n' "$b" >> "$targets" ;; esac
done < "$bundles"
while IFS= read -r b; do printf 'AudioUnit\t%s\n' "$b" >> "$targets"; done < "$installed"
n=$(grep -c . "$targets" || true)
if [ "${n:-0}" != "4" ]; then echo "::error::pluginval 的目标有 ${n:-0} 个，应当是 4 个（2 个 .vst3 加 2 个已装 .component）"; exit 1; fi

{
  echo "### pluginval ${tag}（strictness 5，不开 GUI）"
  echo ""
  echo "| bundle | 格式 | 退出码 | SUCCESS 行 | 结论 |"
  echo "|---|---|---|---|---|"
} >> "${GITHUB_STEP_SUMMARY:-/dev/null}"

fail=0
while IFS=$'\t' read -r fmt path <&3; do
  name=$(basename "$path")
  log="$logs/stdout-$fmt-$name.log"
  rc=0
  "$exe" --strictness-level 5 --timeout-ms 60000 --skip-gui-tests \
    --output-dir "$logs" --validate "$path" </dev/null > "$log" 2>&1 || rc=$?
  ok=no
  if grep -qx 'SUCCESS' "$log"; then ok=yes; fi
  verdict=$(cell_verdict "$rc" "$log")
  if [ "$verdict" = "FAIL" ]; then fail=1; fi
  echo "::group::pluginval ${name}（${fmt}）：退出码 ${rc}，SUCCESS 行 ${ok}，$verdict"
  cat "$log"
  echo "::endgroup::"
  echo "| \`$name\` | $fmt | $rc | $ok | $verdict |" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
  if [ "$verdict" = "FAIL" ]; then echo "::error::pluginval ${name}（${fmt}）未通过：退出码 ${rc}，SUCCESS 行 $ok"; fi
done 3< "$targets"
RINNE_RAN_TO_END=1
exit "$fail"
