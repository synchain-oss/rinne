#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 配置之后核对 CMakeCache.txt：架构与部署目标来自 cmake/RinneMac.cmake 的默认值，段名前缀是默认值。
#   check-cache.sh <构建目录> formal|probe
#   check-cache.sh --self-test
# 命令行不传架构与部署目标，所以这里读回来的就是 RinneMac.cmake 在起作用。正式构建恰好 arm64，探针构建是 arm64 与 x86_64；
# 部署目标是 11.0（ADR-0005）。RINNE_SHM_PREFIX 必须等于本次配置的默认值（没被命令行覆盖），探针构建的默认值是带 P 的探针前缀。

set -euo pipefail

cache_value() { # $1 名字 $2 缓存文件
  sed -n "s/^$1:[A-Z]*=//p" "$2" | head -n 1
}

# 返回错误文字，对上了返回空
verify() { # $1 缓存文件 $2 formal|probe
  local f=$1 flavor=$2 archs target prefix want_prefix want_archs probe_flag want_probe
  archs=$(cache_value CMAKE_OSX_ARCHITECTURES "$f")
  target=$(cache_value CMAKE_OSX_DEPLOYMENT_TARGET "$f")
  prefix=$(cache_value RINNE_SHM_PREFIX "$f")
  want_prefix=$(cache_value RINNE_SHM_PREFIX_DEFAULT "$f")
  probe_flag=$(cache_value RINNE_PROBE_BUILD "$f")
  if [ "$flavor" = "probe" ]; then want_archs="arm64;x86_64"; want_probe=ON; else want_archs="arm64"; want_probe=OFF; fi
  if [ "$archs" != "$want_archs" ]; then echo "CMAKE_OSX_ARCHITECTURES 是 '$archs'，$flavor 构建应当是 '$want_archs'"; return; fi
  if [ "$target" != "11.0" ]; then echo "CMAKE_OSX_DEPLOYMENT_TARGET 是 '$target'，应当是 11.0"; return; fi
  if [ -z "$prefix" ] || [ -z "$want_prefix" ]; then echo "缓存里没有 RINNE_SHM_PREFIX 或 RINNE_SHM_PREFIX_DEFAULT"; return; fi
  if [ "$prefix" != "$want_prefix" ]; then echo "RINNE_SHM_PREFIX 被覆盖成 '$prefix'，默认值是 '$want_prefix'"; return; fi
  if [ "$flavor" = "probe" ] && [ "$probe_flag" != "$want_probe" ]; then echo "RINNE_PROBE_BUILD 是 '$probe_flag'，探针构建应当是 ON"; return; fi
  if [ "$flavor" = "formal" ] && [ "${probe_flag:-OFF}" != "OFF" ]; then echo "RINNE_PROBE_BUILD 是 '$probe_flag'，正式构建应当是 OFF"; return; fi
}

if [ "${1:-}" = "--self-test" ]; then
  tmp=$(mktemp -d "${TMPDIR:-/tmp}/rinne.XXXXXX")
  trap 'rm -rf "$tmp"' EXIT
  bad=0
  mk() { # $1 文件 $2 架构 $3 目标 $4 前缀 $5 默认前缀 $6 探针开关
    printf 'CMAKE_OSX_ARCHITECTURES:STRING=%s\nCMAKE_OSX_DEPLOYMENT_TARGET:STRING=%s\nRINNE_SHM_PREFIX:STRING=%s\nRINNE_SHM_PREFIX_DEFAULT:INTERNAL=%s\nRINNE_PROBE_BUILD:BOOL=%s\n' "$2" "$3" "$4" "$5" "$6" > "$1"
  }
  expect() { # $1 说明 $2 期望（ok 或 bad） $3 缓存文件 $4 formal|probe
    local got
    got=$(verify "$3" "$4")
    if { [ "$2" = ok ] && [ -z "$got" ]; } || { [ "$2" = bad ] && [ -n "$got" ]; }; then echo "PASS  $1"; else echo "FAIL  ${1}（'$got'）"; bad=1; fi
  }
  mk "$tmp/a" "arm64" "11.0" "SynchainRinne" "SynchainRinne" OFF;       expect "正式构建的默认值对得上" ok "$tmp/a" formal
  mk "$tmp/b" "arm64;x86_64" "11.0" "SynchainRinne" "SynchainRinne" OFF; expect "正式构建出了 universal 判错" bad "$tmp/b" formal
  mk "$tmp/c" "arm64" "12.0" "SynchainRinne" "SynchainRinne" OFF;       expect "部署目标不是 11.0 判错" bad "$tmp/c" formal
  mk "$tmp/d" "arm64" "11.0" "SynchainRinneD" "SynchainRinne" OFF;      expect "段名前缀被覆盖判错" bad "$tmp/d" formal
  mk "$tmp/e" "arm64;x86_64" "11.0" "SynchainRinneP" "SynchainRinneP" ON; expect "探针构建的默认值对得上" ok "$tmp/e" probe
  mk "$tmp/f" "arm64" "11.0" "SynchainRinneP" "SynchainRinneP" ON;      expect "探针构建只有 arm64 判错" bad "$tmp/f" probe
  mk "$tmp/g" "arm64" "11.0" "SynchainRinne" "SynchainRinne" ON;        expect "正式构建开了探针开关判错" bad "$tmp/g" formal
  [ "$bad" = 0 ] || { echo "check-cache 自测失败"; exit 1; }
  echo "check-cache 自测通过"
  exit 0
fi

err=$(verify "$1/CMakeCache.txt" "$2")
if [ -n "$err" ]; then echo "::error::$err"; exit 1; fi
echo "缓存核对通过（${2}）：架构 $(cache_value CMAKE_OSX_ARCHITECTURES "$1/CMakeCache.txt")，部署目标 $(cache_value CMAKE_OSX_DEPLOYMENT_TARGET "$1/CMakeCache.txt")，段名前缀 $(cache_value RINNE_SHM_PREFIX "$1/CMakeCache.txt")"
