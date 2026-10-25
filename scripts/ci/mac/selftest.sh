#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# mac CI 脚本的判据自测：不需要 mac，不需要构建产物，也不联网。build-mac 在最后跑它（前面的步骤红了也照跑），本机也能跑。
# sign-and-verify.sh 与 stress.sh 的反例要真 mac 和构建产物，在各自的步骤里实跑，不在这里。
# 最后一段是 bash 3.2 兼容扫描（对 git 跟踪的所有 *.sh）：判据自己的坏样例与正例先过，再扫真实的仓库。
#   bash scripts/ci/mac/selftest.sh

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
bash "$here/expect-red.sh" --self-test
bash "$here/au-gate.sh" --self-test
bash "$here/pluginval-gate.sh" --self-test
bash "$here/check-cache.sh" --self-test
bash "$here/shm-leftover.sh" --self-test
bash "$here/alloc-canary.sh" --self-test
node "$here/check-bundles.mjs" --self-test

# ── bash 3.2 兼容扫描 ────────────────────────────────────────────────────────
# macOS 自带的 /bin/bash 是 3.2。变量名后紧跟非 ASCII 字符（全角标点、中文）时，3.2 在 UTF-8 下把多字节的首字节
# 当成变量名的一部分：set -u 下直接 unbound variable，随后整个脚本静默结束，调用它的 CI 步骤退出码可能是 0。
# Linux 与 Windows 的 bash 上看不出来，所以只能靠扫描。一律写成 ${name}。
# 对 git 跟踪的所有 *.sh 逐个扫，命中即红；本机 Full 档 gate（scripts/gates.d/60-package.ps1）扫同一个模式。
# 不是 mac 上跑的脚本（只在 Linux 上跑，bash 4 以上）登记在这里，整条路径逐字匹配，不是前缀、不是通配。
bash32_exempt() { # $1 仓内相对路径；0 = 登记过的 Linux 专用脚本
  case "$1" in
    tests/sanitizer-canary/run.sh) return 0 ;;
  esac
  return 1
}

# 一个文件里的命中，逐行打印成 <行号>:<内容>；有命中返回 1。
# 用 LC_ALL=C 逐字节看。BSD grep（mac 自带）没有 -P，所以用 POSIX 字符类：不是可打印 ASCII、也不是空白的，都算非 ASCII 字节。
bash32_scan_file() { # $1 文件
  if LC_ALL=C grep -nE '\$[A-Za-z_][A-Za-z0-9_]*[^[:print:][:space:]]' -- "$1"; then
    return 1
  fi
  return 0
}

# 仓库里 git 跟踪的 *.sh 全扫（跳过登记过的）；命中的打印成 <路径>:<行号>:<内容>，有命中返回 1。
bash32_scan_repo() { # $1 仓根
  local root="$1" f hits bad=0
  while IFS= read -r -d '' f; do
    if bash32_exempt "$f"; then continue; fi
    if ! hits=$(bash32_scan_file "$root/$f"); then
      printf '%s\n' "$hits" | sed "s|^|${f}:|"
      bad=1
    fi
  done < <(git -C "$root" ls-files -z -- '*.sh')
  return "$bad"
}

# 整仓检查（真正挂在 CI 上的那一步）：命中就打印命中位置和一条 ::error::，返回 1。
bash32_check_repo() { # $1 仓根
  if ! bash32_scan_repo "$1"; then
    echo "::error::上面这些 *.sh 里有「\${变量}」没加花括号、后面紧跟非 ASCII 字符：macOS 自带的 bash 3.2 会把它当成变量名的一部分（改成 \${name}）"
    return 1
  fi
  echo "bash 3.2 兼容扫描：git 跟踪的 *.sh 全部干净"
}

bash32_selftest() {
  local dir bad=0
  dir=$(mktemp -d "${TMPDIR:-/tmp}/rinne.XXXXXX")
  # rinne-delete-ok: 自测里 mktemp -d 新建的目录
  trap 'rm -rf "$dir"' RETURN
  expect() { # $1 说明 $2 期望（hit|clean） $3 实际
    if [ "$2" != "$3" ]; then echo "FAIL  ${1}（期望 ${2}，实际 ${3}）"; bad=1; else echo "PASS  $1"; fi
  }
  # 用八进制转义造出非 ASCII 字节：本文件自己不含「变量名后紧跟非 ASCII」的写法，扫描不会误伤自己。
  local colon rparen han
  colon=$(printf '\357\274\232') # 全角冒号
  rparen=$(printf '\357\274\211') # 全角右括号
  han=$(printf '\344\270\255') # 中
  sample() { # $1 文件名 $2 内容（printf 格式，可含 %s） 其余：格式的参数
    local name="$1" fmt="$2"
    shift 2
    # shellcheck disable=SC2059
    printf "$fmt" "$@" > "$dir/$name"
  }
  verdict() { # $1 文件名 → hit | clean
    if bash32_scan_file "$dir/$1" > /dev/null; then echo clean; else echo hit; fi
  }
  # 坏样例：变量名后紧跟非 ASCII
  sample bad1.sh 'echo "$name%sx"\n' "$colon"
  expect "坏：\$name 后紧跟全角冒号" hit "$(verdict bad1.sh)"
  sample bad2.sh 'echo "($k%s"\n' "$rparen"
  expect "坏：单字母变量后紧跟全角括号" hit "$(verdict bad2.sh)"
  sample bad3.sh 'echo "$_%sx"\n' "$colon"
  expect "坏：下划线变量名后紧跟非 ASCII" hit "$(verdict bad3.sh)"
  sample bad4.sh 'echo "$name%s"\n' "$han"
  expect "坏：变量后紧跟汉字" hit "$(verdict bad4.sh)"
  sample bad5.sh 'echo ok\necho ok\ndie "没有 $name%s"\n' "$colon"
  expect "坏：命中在第 3 行，报的行号是 3" "3" "$(bash32_scan_file "$dir/bad5.sh" | sed -n '1s/:.*//p')"
  sample bad6.sh 'x=$name%s\n' "$colon"
  expect "坏：不在引号里的 \$name 后紧跟非 ASCII" hit "$(verdict bad6.sh)"
  # 正例：这些写法在 3.2 上没问题，不许误报
  sample ok1.sh 'echo "${name}%sx"\n' "$colon"
  expect "好：\${name} 后紧跟非 ASCII" clean "$(verdict ok1.sh)"
  sample ok2.sh 'echo "$1%s" "$#%s" "$?%s" "$@%s"\n' "$colon" "$colon" "$colon" "$colon"
  expect "好：特殊参数 \$1 \$# \$? \$@ 后紧跟非 ASCII" clean "$(verdict ok2.sh)"
  sample ok3.sh 'echo "${have:-%s}"\n' "$han"
  expect "好：\${x:-汉字} 的默认值里有汉字" clean "$(verdict ok3.sh)"
  sample ok4.sh 'echo "$name x%s"\n' "$colon"
  expect "好：变量后面先有空格" clean "$(verdict ok4.sh)"
  sample ok5.sh 'echo "$name"%s\n' "$colon"
  expect "好：变量在引号里、非 ASCII 在引号外" clean "$(verdict ok5.sh)"
  sample ok6.sh 'echo "$name"\r\n'
  expect "好：行尾是 CRLF" clean "$(verdict ok6.sh)"
  sample ok7.sh '# ${name}%s%s\n' "$colon" "$han"
  expect "好：注释里写成 \${name}" clean "$(verdict ok7.sh)"
  sample ok8.sh 'echo %s\n' "$colon"
  expect "好：没有变量的纯中文行" clean "$(verdict ok8.sh)"
  # 登记表：整条路径逐字匹配
  if bash32_exempt tests/sanitizer-canary/run.sh; then expect "登记过的 Linux 专用脚本被放行" clean clean; else expect "登记过的 Linux 专用脚本被放行" clean hit; fi
  if bash32_exempt scripts/tests/sanitizer-canary/run.sh; then expect "登记表不是后缀匹配" clean hit; else expect "登记表不是后缀匹配" clean clean; fi
  if bash32_exempt tests/sanitizer-canary/run.sh.bak; then expect "登记表不是前缀匹配" clean hit; else expect "登记表不是前缀匹配" clean clean; fi
  if bash32_exempt scripts/package-macos.sh; then expect "打包脚本不在登记表里" clean hit; else expect "打包脚本不在登记表里" clean clean; fi
  # 整仓扫描（真走 git ls-files）：临时仓里放坏文件 → 红，放好文件 → 绿，登记过的路径放坏内容 → 绿，别的目录放坏内容 → 红
  local repo="$dir/repo"
  mkdir -p "$repo/scripts" "$repo/tests/sanitizer-canary" "$repo/other"
  git -C "$repo" init -q
  git -C "$repo" config core.autocrlf false
  sample repo/scripts/good.sh 'echo "${name}%sx"\n' "$colon"
  git -C "$repo" add scripts/good.sh
  if bash32_scan_repo "$repo" > /dev/null; then expect "整仓扫描：只有好文件时通过" clean clean; else expect "整仓扫描：只有好文件时通过" clean hit; fi
  sample repo/scripts/bad.sh 'echo "$name%sx"\n' "$colon"
  git -C "$repo" add scripts/bad.sh
  if bash32_scan_repo "$repo" > "$dir/scan.out"; then expect "整仓扫描：混进一个坏文件就红" hit clean; else expect "整仓扫描：混进一个坏文件就红" hit hit; fi
  expect "整仓扫描：红的输出点名文件与行号" "scripts/bad.sh:1:" "$(sed -n '1s/\(^[^:]*:[0-9]*:\).*/\1/p' "$dir/scan.out")"
  # 整仓检查函数本身：输出一律进文件，不进 CI 日志（里面的 ::error:: 会被 GitHub 当成真的报错）
  if bash32_check_repo "$repo" > "$dir/check.out" 2>&1; then expect "整仓检查：有坏文件时返回非 0" hit clean; else expect "整仓检查：有坏文件时返回非 0" hit hit; fi
  expect "整仓检查：有坏文件时打出 error 标注" 1 "$(grep -c '^::error::' "$dir/check.out" || true)"
  git -C "$repo" rm -q -f --cached scripts/bad.sh
  if bash32_check_repo "$repo" > "$dir/check.out" 2>&1; then expect "整仓检查：坏文件拿掉后返回 0" clean clean; else expect "整仓检查：坏文件拿掉后返回 0" clean hit; fi
  sample repo/tests/sanitizer-canary/run.sh 'echo "$name%sx"\n' "$colon"
  git -C "$repo" add tests/sanitizer-canary/run.sh
  if bash32_scan_repo "$repo" > /dev/null; then expect "整仓扫描：登记过的路径里有坏写法也放行" clean clean; else expect "整仓扫描：登记过的路径里有坏写法也放行" clean hit; fi
  sample repo/other/run.sh 'echo "$name%sx"\n' "$colon"
  git -C "$repo" add other/run.sh
  if bash32_scan_repo "$repo" > /dev/null; then expect "整仓扫描：同名文件在别的目录里不放行" hit clean; else expect "整仓扫描：同名文件在别的目录里不放行" hit hit; fi
  # 没被 git 跟踪的文件不扫（扫的是进仓的内容）；非 *.sh 的文件不扫
  git -C "$repo" rm -q -f --cached other/run.sh
  sample repo/other/untracked.sh 'echo "$name%sx"\n' "$colon"
  sample repo/other/notes.txt 'echo "$name%sx"\n' "$colon"
  git -C "$repo" add other/notes.txt
  if bash32_scan_repo "$repo" > /dev/null; then expect "整仓扫描：未跟踪的 .sh 与非 .sh 文件不扫" clean clean; else expect "整仓扫描：未跟踪的 .sh 与非 .sh 文件不扫" clean hit; fi

  # 接线：本文件末尾真的调用了整仓检查（行首匹配；把那一行删掉，这条就红）
  expect "接线：末尾调用了整仓检查" 1 "$(grep -c '^bash32_check_repo "\$root"' "${BASH_SOURCE[0]}" || true)"

  [ "$bad" = 0 ] || { echo "bash 3.2 兼容扫描的自测失败"; return 1; }
  echo "bash 3.2 兼容扫描的自测通过"
}

bash32_selftest
root=$(cd "$here/../../.." && pwd)
bash32_check_repo "$root" || exit 1
