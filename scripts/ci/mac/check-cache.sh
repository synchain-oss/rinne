#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 配置之后核对 CMakeCache.txt 与生成的 src/core/generated/util/RinneVersion.h：架构与部署目标来自 cmake/RinneMac.cmake
# 的默认值，段名前缀是默认值，编进去的段名前缀与探针开关也和本次身份一致。
#   check-cache.sh <构建目录> formal|probe
#   check-cache.sh --self-test
# 命令行不传架构与部署目标，所以这里读回来的就是 RinneMac.cmake 在起作用。正式构建恰好 arm64，探针构建是 arm64 与 x86_64；
# 部署目标是 11.0（ADR-0005）。RINNE_SHM_PREFIX 必须等于本次配置的默认值（没被命令行覆盖），探针构建的默认值是带 P 的探针前缀。
# 根 CMakeLists.txt 的 RINNE_SHM_PREFIX_FORMAL 是真源：cache 里的值可能被同名的普通变量遮住（CMP0126），编进
# RinneVersion.h 的才算数，所以这里和 build.ps1 的 -CheckCache 一样读回来再比一遍。

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)

cache_value() { # $1 名字 $2 缓存文件
  sed -n "s/^$1:[A-Z]*=//p" "$2" | head -n 1
}

formal_prefix() { # 根 CMakeLists.txt 里唯一一处正式前缀，例如 SynchainRinne
  [ -f "$root/CMakeLists.txt" ] || return 0
  sed -n 's/^set(RINNE_SHM_PREFIX_FORMAL "\([A-Za-z]*\)")$/\1/p' "$root/CMakeLists.txt" | head -n 1
}

header_prefix() { # $1 RinneVersion.h
  sed -nE 's/^[[:space:]]*inline constexpr const char[*] kShmPrefix = "([^"]*)";.*$/\1/p' "$1" | head -n 1
}

header_probe() { # $1 RinneVersion.h
  sed -nE 's/^[[:space:]]*inline constexpr bool kProbeBuild = ([^;]*);.*$/\1/p' "$1" | head -n 1
}

# 返回错误文字，对上了返回空
verify() { # $1 构建目录 $2 formal|probe
  local dir=$1 flavor=$2 f h archs target prefix want_prefix probe_flag want_archs want_probe
  local formal want_compiled_prefix want_compiled_probe have_prefix have_probe
  f="$1/CMakeCache.txt"
  h="$1/src/core/generated/util/RinneVersion.h"
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

  formal=$(formal_prefix || true)
  if [ -z "$formal" ]; then echo "根 CMakeLists.txt 里找不到 set(RINNE_SHM_PREFIX_FORMAL \"…\")"; return; fi
  if [ "$flavor" = "probe" ]; then
    want_compiled_prefix="${formal}P"
    want_compiled_probe=true
  else
    want_compiled_prefix="$formal"
    want_compiled_probe=false
  fi
  if [ ! -f "$h" ]; then echo "没有生成的 ${h}（配置之后才会有）"; return; fi
  have_prefix=$(header_prefix "$h")
  have_probe=$(header_probe "$h")
  if [ -z "$have_prefix" ] || [ -z "$have_probe" ]; then echo "生成的 RinneVersion.h 里找不到 kShmPrefix 或 kProbeBuild"; return; fi
  case "$have_probe" in true|false) ;; *) echo "生成的 kProbeBuild 是 '$have_probe'，不是 true 或 false"; return ;; esac
  if [ "$have_prefix" != "$want_compiled_prefix" ]; then
    echo "编进去的段名前缀是 '$have_prefix'，$flavor 构建应为 '$want_compiled_prefix'"
    return
  fi
  if [ "$have_probe" != "$want_compiled_probe" ]; then
    echo "编进去的 kProbeBuild 是 '$have_probe'，$flavor 构建应为 '$want_compiled_probe'"
    return
  fi
}

if [ "${1:-}" = "--self-test" ]; then
  tmp=$(mktemp -d "${TMPDIR:-/tmp}/rinne.XXXXXX")
  # rinne-delete-ok: 自测里 mktemp -d 新建的目录
  trap 'rm -rf "$tmp"' EXIT
  bad=0
  mk() { # $1 目录 $2 架构 $3 目标 $4 前缀 $5 默认前缀 $6 探针开关 [$7 编进去的前缀] [$8 编进去的探针]
    local dir=$1 hp hb
    hp=${7:-$4}
    hb=${8:-}
    if [ -z "$hb" ]; then
      if [ "$6" = ON ]; then hb=true; else hb=false; fi
    fi
    mkdir -p "$dir/src/core/generated/util"
    printf 'CMAKE_OSX_ARCHITECTURES:STRING=%s\nCMAKE_OSX_DEPLOYMENT_TARGET:STRING=%s\nRINNE_SHM_PREFIX:STRING=%s\nRINNE_SHM_PREFIX_DEFAULT:INTERNAL=%s\nRINNE_PROBE_BUILD:BOOL=%s\n' "$2" "$3" "$4" "$5" "$6" > "$dir/CMakeCache.txt"
    printf 'inline constexpr const char* kShmPrefix = "%s";\ninline constexpr bool kProbeBuild = %s;\n' "$hp" "$hb" > "$dir/src/core/generated/util/RinneVersion.h"
  }
  expect() { # $1 说明 $2 期望（ok 或 bad） $3 构建目录 $4 formal|probe
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
  # 编进头文件的值和缓存对不上：缓存干净也必须在头文件这一关变红
  mk "$tmp/h" "arm64" "11.0" "SynchainRinne" "SynchainRinne" OFF "SynchainRinneD" false
  expect "编进去的段名前缀带 D 判错" bad "$tmp/h" formal
  mk "$tmp/i" "arm64;x86_64" "11.0" "SynchainRinneP" "SynchainRinneP" ON "SynchainRinne" true
  expect "探针构建编进去的前缀不带 P 判错" bad "$tmp/i" probe
  mk "$tmp/j" "arm64" "11.0" "SynchainRinne" "SynchainRinne" OFF "SynchainRinne" true
  expect "正式构建编进去的是探针判错" bad "$tmp/j" formal
  mk "$tmp/k" "arm64;x86_64" "11.0" "SynchainRinneP" "SynchainRinneP" ON "SynchainRinneP" false
  expect "探针构建编进去的不是探针判错" bad "$tmp/k" probe
  mk "$tmp/l" "arm64" "11.0" "SynchainRinne" "SynchainRinne" OFF
  rm "$tmp/l/src/core/generated/util/RinneVersion.h"
  expect "没有生成的 RinneVersion.h 判错" bad "$tmp/l" formal
  mk "$tmp/m" "arm64" "11.0" "SynchainRinne" "SynchainRinne" OFF
  printf 'inline constexpr const char* kShmPrefix = "SynchainRinne";\n' > "$tmp/m/src/core/generated/util/RinneVersion.h"
  expect "头文件里没有 kProbeBuild 判错" bad "$tmp/m" formal
  mk "$tmp/n" "arm64" "11.0" "SynchainRinne" "SynchainRinne" OFF "SynchainRinne" 1
  expect "头文件里的 kProbeBuild 不是 true/false 判错" bad "$tmp/n" formal
  [ "$bad" = 0 ] || { echo "check-cache 自测失败"; exit 1; }
  echo "check-cache 自测通过"
  exit 0
fi

err=$(verify "$1" "$2")
if [ -n "$err" ]; then echo "::error::$err"; exit 1; fi
echo "缓存核对通过（${2}）：架构 $(cache_value CMAKE_OSX_ARCHITECTURES "$1/CMakeCache.txt")，部署目标 $(cache_value CMAKE_OSX_DEPLOYMENT_TARGET "$1/CMakeCache.txt")，段名前缀 $(cache_value RINNE_SHM_PREFIX "$1/CMakeCache.txt")，编进去 $(header_prefix "$1/src/core/generated/util/RinneVersion.h")（kProbeBuild=$(header_probe "$1/src/core/generated/util/RinneVersion.h")）"
