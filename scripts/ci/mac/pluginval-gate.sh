#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/mac-ci.json
#
# mac 上的 pluginval 门：2 个 .vst3（构建树里的那份）加 2 个已装的 .component（按安装后的路径验），strictness 5，不开 GUI。
#   pluginval-gate.sh <bundle 列表> <已装 AU 列表> <日志目录>
# 版本只从仓根 .pluginval-version 读；包的 sha256 登记在同目录的 pluginval-macos.sha256（首次下载的值，上游不发校验文件），
# 版本与登记不一致时停下，不凑合。每格判据：退出码 0，输出里有单独一行 SUCCESS。四格都跑完再统一判。

set -euo pipefail

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
# rinne-delete-ok: 本脚本 mktemp -d 新建的目录，退出时只删它自己
cleanup() { local rc=$?; rm -rf "$work_root" || true; exit $rc; }
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
# 版本号要作为一个完整的词出现在 --version 的输出里（子串匹配会让 1.0.40 或 1.0.4-rc1 冒充 1.0.4）
want=${tag#v}
escaped=$(printf '%s' "$want" | sed 's/\./\\./g')
pattern="(^|[^0-9A-Za-z.-])v?${escaped}([^0-9A-Za-z.-]|$)"
if ! grep -Eq "$pattern" <<< "$ver"; then
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
  verdict=PASS
  if [ "$rc" != "0" ] || [ "$ok" != "yes" ]; then verdict=FAIL; fail=1; fi
  echo "::group::pluginval ${name}（${fmt}）：退出码 ${rc}，SUCCESS 行 ${ok}，$verdict"
  cat "$log"
  echo "::endgroup::"
  echo "| \`$name\` | $fmt | $rc | $ok | $verdict |" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
  if [ "$verdict" = "FAIL" ]; then echo "::error::pluginval ${name}（${fmt}）未通过：退出码 ${rc}，SUCCESS 行 $ok"; fi
done 3< "$targets"
exit "$fail"
