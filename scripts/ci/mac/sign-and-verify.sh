#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ad-hoc 签名并核对（ADR-0005：mac 包不签名、不公证，CI 只核对包内的 ad-hoc 签名完整）。
#   sign-and-verify.sh <bundle 列表>
# 构建出来的 bundle 只有链接器给主程序打的签名，资源（Info.plist）没有封进去；这里整体 ad-hoc 重签，让 bundle 的签名封住全部内容。
# 然后 codesign --verify --strict，且签名必须是 ad-hoc（没有任何 Authority）。再做两个反例，证明这一步会变红：
# 改动签好的副本里主程序的一个字节、以及去掉副本的签名，codesign --verify 都必须失败。

set -euo pipefail

list=$1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/rinne.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
first=""
fail=0

while IFS= read -r b; do
  [ -n "$b" ] || continue
  codesign --force --sign - --timestamp=none "$b"
  if ! codesign --verify --strict --verbose=2 "$b"; then
    echo "::error::codesign --verify 未通过：$(basename "$b")"
    fail=1
    continue
  fi
  info=$(codesign -dvv "$b" 2>&1)
  if ! grep -q 'Signature=adhoc' <<< "$info" || grep -q '^Authority=' <<< "$info"; then
    echo "::error::$(basename "$b") 的签名不是纯 ad-hoc：$info"
    fail=1
  fi
  echo "- \`$(basename "$b")\`：ad-hoc 签名完整" >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
  [ -n "$first" ] || first=$b
done < "$list"

[ -n "$first" ] || { echo "::error::列表里没有可核对的 bundle"; exit 1; }

# 反例一：改动已签名的主程序。往代码页里写一个字节（追加到文件末尾不一定在签名覆盖的范围内，所以不用追加）；
# 万一写进去的值恰好等于原值，再换一个值写一次
tampered="$tmp/tampered/$(basename "$first")"
mkdir -p "$tmp/tampered"
ditto "$first" "$tampered"
exe=$(find "$tampered/Contents/MacOS" -type f -perm -u+x | head -n 1)
caught=0
for byte in '\132' '\245'; do
  printf "$byte" | dd of="$exe" bs=1 seek=20000 conv=notrunc 2>/dev/null
  if ! codesign --verify --strict "$tampered" 2>/dev/null; then caught=1; break; fi
done
if [ "$caught" = 1 ]; then
  echo "反例通过：主程序被改动后 codesign --verify 失败"
else
  echo "::error::反例失效：主程序被改动后 codesign --verify 仍然通过"
  fail=1
fi

# 反例二：去掉签名
stripped="$tmp/stripped/$(basename "$first")"
mkdir -p "$tmp/stripped"
ditto "$first" "$stripped"
codesign --remove-signature "$stripped"
if codesign --verify --strict "$stripped" 2>/dev/null; then
  echo "::error::反例失效：去掉签名后 codesign --verify 仍然通过"
  fail=1
else
  echo "反例通过：去掉签名后 codesign --verify 失败"
fi

exit "$fail"
