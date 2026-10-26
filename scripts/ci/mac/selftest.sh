#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# mac CI 脚本的判据自测：不需要 mac，不需要构建产物，也不联网。build-mac 在最后跑它（前面的步骤红了也照跑），本机也能跑。
# sign-and-verify.sh 与 stress.sh 的反例要真 mac 和构建产物，在各自的步骤里实跑，不在这里。
# 后两段是对 git 跟踪的所有 *.sh 的静态扫描，各自的坏样例与正例先过，再扫真实的仓库：
#   - bash 3.2 兼容扫描：「$变量」后面紧跟非 ASCII 字节（以及 ASCII 控制字节）。
#   - EXIT trap 的「走完了」标记检查：凡是装了 EXIT trap 的脚本，都要带约定写法，脚本没走完就退出时不许以 0 退出。
#     另有一个动态用例：用同样写法的最小脚本注入一个未定义变量，用 /bin/bash 跑，必须非 0。
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
# 对 git 跟踪的所有 *.sh 逐个扫，命中即红；本机 Full 档 gate（scripts/gates.d/60-package.ps1）扫同一个字节集合。
# 不是 mac 上跑的脚本（只在 Linux 上跑，bash 4 以上）登记在这里，整条路径逐字匹配，不是前缀、不是通配。
# 后面的 EXIT trap 标记检查共用这张登记表：tests/sanitizer-canary/run.sh 的「变量后紧跟全角标点」与「没有走完的标记」
# 两处都改掉之后，才可以把它从这里删掉。
bash32_exempt() { # $1 仓内相对路径；0 = 登记过的 Linux 专用脚本
  case "$1" in
    tests/sanitizer-canary/run.sh) return 0 ;;
  esac
  return 1
}

# 一个文件里的命中，逐行打印成 <行号>:<内容>；有命中返回 1。
# 用 LC_ALL=C 逐字节看。BSD grep（mac 自带）没有 -P，所以用 POSIX 字符类。
# 判的是哪些字节：C locale 下 [:print:] 是 0x20–0x7E，[:space:] 是 0x09–0x0D 与 0x20，所以 [^[:print:][:space:]] 命中
# 「不在 0x09–0x0D 与 0x20–0x7E 里」的字节：0x00–0x08、0x0E–0x1F、0x7F 这些 ASCII 控制字节，加上 0x80–0xFF 的全部非 ASCII 字节。
# scripts/gates.d/60-package.ps1 按 Latin-1 逐字节读，用 [^\x09-\x0D\x20-\x7E]，两边的字节集合逐字节相同（下面的逐字节用例钉着）。
# ASCII 控制字节其实不会被 bash 3.2 并进变量名，这里从严一并判红：*.sh 里本来就不该有裸的控制字节。
# -a：含 NUL 的文件也按文本逐行匹配。不加的话 GNU grep 把二进制文件里的 NUL 当成行尾，变量名后紧跟 NUL 的写法就漏了。
bash32_scan_file() { # $1 文件
  if LC_ALL=C grep -anE '\$[A-Za-z_][A-Za-z0-9_]*[^[:print:][:space:]]' -- "$1"; then
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
  # 坏样例：变量后紧跟 ASCII 控制字节（0x00–0x08、0x0E–0x1F、0x7F）。这些字节 mac 侧的字符类命中、PowerShell 侧也要命中。
  # 样例用 printf 的八进制转义造，本文件自己不含裸的控制字节。
  sample bad7.sh 'echo "$name\001x"\n'
  expect "坏：变量后紧跟 ASCII 控制字节 0x01" hit "$(verdict bad7.sh)"
  sample bad8.sh 'echo "$name\033[0m"\n'
  expect "坏：变量后紧跟 ESC（0x1B）" hit "$(verdict bad8.sh)"
  sample bad9.sh 'echo "$name\177"\n'
  expect "坏：变量后紧跟 DEL（0x7F）" hit "$(verdict bad9.sh)"
  sample bad10.sh 'echo "$name\010"\n'
  expect "坏：变量后紧跟退格（0x08，空白类之前的最后一个控制字节）" hit "$(verdict bad10.sh)"
  sample bad11.sh 'echo "$name\016"\n'
  expect "坏：变量后紧跟 0x0E（空白类之后的第一个控制字节）" hit "$(verdict bad11.sh)"
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
  sample ok9.sh 'echo "$name"\t$x\v$y\f$z\r\n'
  expect "好：变量在引号里、后面是 TAB、VT、FF、CR（0x09–0x0D 属于空白类）" clean "$(verdict ok9.sh)"
  sample ok10.sh 'x=$name\011y=$z\n'
  expect "好：变量后紧跟 TAB（0x09）" clean "$(verdict ok10.sh)"
  sample ok11.sh 'x=$name~\n'
  expect "好：变量后紧跟 ~（0x7E，可打印 ASCII 的最后一个）" clean "$(verdict ok11.sh)"
  # 逐字节：「变量名 + 一个字节」的文件，0x01–0xFF（0x0A 是换行，行就断了，不在内）里恰好「不在 0x09–0x0D 与 0x20–0x7E」的字节命中。
  # 0x00 不在这里扫：BSD grep 对含 NUL 的文件按二进制文件处理；PowerShell 侧的同一组逐字节用例包含 0x00。
  local b fmtb want sweep_wrong=""
  for ((b = 1; b <= 255; b++)); do
    [ "$b" -ne 10 ] || continue
    fmtb=$(printf 'echo "$name\\%03ox"\\n' "$b")
    # shellcheck disable=SC2059
    printf "$fmtb" > "$dir/sweep.sh"
    if { [ "$b" -ge 9 ] && [ "$b" -le 13 ]; } || { [ "$b" -ge 32 ] && [ "$b" -le 126 ]; }; then want=clean; else want=hit; fi
    if [ "$(verdict sweep.sh)" != "$want" ]; then sweep_wrong="$sweep_wrong $b"; fi
  done
  expect "逐字节：0x01–0xFF 里恰好不在 0x09–0x0D、0x20–0x7E 的字节命中（不符的字节：${sweep_wrong:-无}）" "" "${sweep_wrong# }"
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


# ── EXIT trap 的「走完了」标记检查 ───────────────────────────────────────────────
# macOS 自带的 bash 3.2 在 set -u 下遇到变量展开出错会直接结束整个脚本；脚本装了 EXIT trap 时，进入 trap 的 $? 常常是 0
# （出事前最后一条命令的状态），于是脚本半路死了却以 0 退出，调用它的 CI 步骤是绿的（pluginval 闸门因此会静默放行）。
# 对策是「走完了」标记：主流程正常结束处（以及每个合法的提前 exit 0 之前）置位；EXIT trap 里退出码为 0 而标记没置位，
# 就改成非 0 并打印 ::error::。约定写法固定，git 跟踪的 *.sh 里凡是装了 EXIT trap（trap … EXIT，或末尾写 0）的，都要同时带下面五样，
# 按行扫，注释行与空行不算：
#   RINNE_RAN_TO_END=0                                      装 trap 之前初始化（整个文件里有这一行）
#   RINNE_RAN_TO_END=1                                      主流程正常结束处置位（整个文件里有这一行）
# 下面三样只认 EXIT trap 的处理函数的函数体里（trap 后面直接写函数名，不写带引号的命令；函数定义写成「名字() {」独占一行，
# 右花括号独占一行、与定义行同缩进）：
#   rc=$?                                                   函数体的第一条命令（可以带 local）：一进来先取退出码；行首与 rc 之间
#                                                           只许空白与 local，所以 foo_rc=$?、self_test_rc=$? 不算；只认第一条，
#                                                           是因为前面再有别的命令，$? 就已经被冲掉了
#   [ "$rc" -eq 0 ] && [ "$RINNE_RAN_TO_END" != 1 ]         退出码为 0、标记却没置位（前后要有分隔符，写在字符串里的不算；
#                                                           因为 rc=$? 必是函数体的第一条，判据读到的 rc 一定是取过的）
#   脚本没有走完就退出了                                      报错文字
# 所以「foo_rc=$?」「别处的 || rc=$?」「写在别的函数里的判据」都不算数；处理程序是带引号的命令、找不到函数定义、
# 函数写在一行里、右花括号对不上缩进，一律判红：宁可多报，约定写法就这一种。
# 判据是否真的接在 if 里并 exit 1，静态扫描核不了：下面的动态用例用同样写法的最小脚本、/bin/bash 真跑来验这一段的逻辑。
# 本机 Full 档 gate（scripts/gates.d/60-package.ps1）扫同一套写法；登记表（bash32_exempt）两道扫描共用。
# RETURN、INT、TERM 的 trap 不是 EXIT trap，不在此列。字符串里写出「trap … EXIT」也会被当成装了 trap：宁可多报。
exit_trap_re='(^|[^A-Za-z0-9_])trap[[:space:]].*[[:space:]](EXIT|0)([[:space:];})]|$)'
# trap 后面直接是函数名（可带 --），函数名之后只有信号名，信号表里有 EXIT 或 0；BASH_REMATCH[3] 是函数名
exit_handler_re='(^|[^A-Za-z0-9_])trap[[:space:]]+(--[[:space:]]+)?([A-Za-z_][A-Za-z0-9_]*)[[:space:]]+([A-Za-z0-9_]+[[:space:]]+)*(EXIT|0)([[:space:];})]|$)'
exit_init_re='(^|[[:space:];{(])RINNE_RAN_TO_END=0([[:space:];})]|$)'
exit_fin_re='(^|[[:space:];{(])RINNE_RAN_TO_END=1([[:space:];})]|$)'
exit_rcfirst_re='^[[:space:]]*(local[[:space:]]+)?rc=\$\?([[:space:];]|$)'
exit_guard_re='(^|[[:space:];{(])\[ "\$rc" -eq 0 \] && \[ "\$RINNE_RAN_TO_END" != 1 \]([[:space:];})]|$)'
exit_skip_re='^[[:space:]]*(#|$)'
exit_guard_with_trap=0

# 注释行之外有没有装 EXIT trap
exit_guard_has_trap() { # $1 文件
  local code
  code=$(LC_ALL=C grep -av '^[[:space:]]*#' -- "$1" || true)
  LC_ALL=C grep -Eq "$exit_trap_re" <<< "$code"
}

# 一个文件的检查：没装 EXIT trap 或五样都齐返回 0；缺了就逐行打印缺什么，返回 1。
# 函数体是子 shell：LC_ALL=C 只管这里，下面的 =~ 与 case 都按字节匹配。整个文件读进数组 code（去掉注释行与空行），
# 先扫出 EXIT trap 的处理函数名，再在每个处理函数的函数体（定义行的下一行起，到与定义行同缩进的右花括号前）里核三样。
# 不用 ${#code[@]} 与 "${code[@]}"：bash 3.2 在 set -u 下对空数组会报 unbound variable。
exit_guard_scan_file() ( # $1 文件
  LC_ALL=C
  export LC_ALL
  local line name names="" def_re end_re found end g t i j n=0
  local miss=0 init_ok=0 fin_ok=0 unres=0 nodef="" noend="" rc_bad=0 guard_bad=0 text_bad=0
  local -a code
  if ! exit_guard_has_trap "$1"; then exit 0; fi
  while IFS= read -r line || [ -n "$line" ]; do
    if [[ $line =~ $exit_skip_re ]]; then continue; fi
    code[n]=$line
    n=$((n + 1))
  done < "$1"
  for ((i = 0; i < n; i++)); do
    line=${code[i]}
    if [[ $line =~ $exit_init_re ]]; then init_ok=1; fi
    if [[ $line =~ $exit_fin_re ]]; then fin_ok=1; fi
    if [[ $line =~ $exit_trap_re ]]; then
      if [[ $line =~ $exit_handler_re ]]; then
        name=${BASH_REMATCH[3]}
        case " $names " in *" $name "*) ;; *) names="$names $name" ;; esac
      else
        unres=1
      fi
    fi
  done
  # 装了 EXIT trap 却一个处理函数名也没认出来：按处理程序不是函数名处理（不会发生，防的是两处正则漂移）
  if [ -z "$names" ]; then unres=1; fi
  for name in $names; do
    def_re='^([[:space:]]*)(function[[:space:]]+'"$name"'([[:space:]]*\(\))?|'"$name"'[[:space:]]*\(\))[[:space:]]*\{[[:space:]]*$'
    found=0
    for ((i = 0; i < n; i++)); do
      if ! [[ ${code[i]} =~ $def_re ]]; then continue; fi
      found=1
      end_re='^'"${BASH_REMATCH[1]}"'\}([[:space:];]|$)'
      end=-1
      for ((j = i + 1; j < n; j++)); do
        if [[ ${code[j]} =~ $end_re ]]; then end=$j; break; fi
      done
      if [ "$end" -lt 0 ]; then noend="$noend $name"; continue; fi
      if [ $((i + 1)) -ge "$end" ] || ! [[ ${code[i + 1]} =~ $exit_rcfirst_re ]]; then rc_bad=1; fi
      g=0
      t=0
      for ((j = i + 1; j < end; j++)); do
        if [[ ${code[j]} =~ $exit_guard_re ]]; then g=1; fi
        case "${code[j]}" in *"脚本没有走完就退出了"*) t=1 ;; esac
      done
      if [ "$g" = 0 ]; then guard_bad=1; fi
      if [ "$t" = 0 ]; then text_bad=1; fi
    done
    if [ "$found" = 0 ]; then nodef="$nodef $name"; fi
  done
  if [ "$init_ok" = 0 ]; then echo "缺 RINNE_RAN_TO_END=0（装 trap 之前初始化）"; miss=1; fi
  if [ "$fin_ok" = 0 ]; then echo "缺 RINNE_RAN_TO_END=1（主流程正常结束处置位）"; miss=1; fi
  if [ "$unres" = 1 ]; then echo "EXIT trap 的处理程序不是函数名（要写成「trap 函数名 EXIT」，不写带引号的命令）"; miss=1; fi
  for name in $nodef; do echo "找不到 EXIT trap 处理函数 ${name} 的定义（要写成「${name}() {」独占一行）"; miss=1; done
  for name in $noend; do echo "EXIT trap 处理函数 ${name} 找不到函数体的结尾（右花括号要独占一行、与定义行同缩进）"; miss=1; done
  if [ "$rc_bad" = 1 ]; then echo "缺 rc=\$?（EXIT trap 处理函数的第一条命令要先取退出码）"; miss=1; fi
  if [ "$guard_bad" = 1 ]; then echo "缺 [ \"\$rc\" -eq 0 ] && [ \"\$RINNE_RAN_TO_END\" != 1 ]（EXIT trap 处理函数里：退出码为 0、标记却没置位）"; miss=1; fi
  if [ "$text_bad" = 1 ]; then echo "缺报错文字「脚本没有走完就退出了」（要写在 EXIT trap 处理函数里）"; miss=1; fi
  exit "$miss"
)

# 仓库里 git 跟踪的 *.sh 全扫（跳过登记过的）；有问题的打印成 <路径>: <缺什么>，有问题返回 1。装了 EXIT trap 的个数记在 exit_guard_with_trap。
exit_guard_scan_repo() { # $1 仓根
  local root="$1" f out bad=0
  exit_guard_with_trap=0
  while IFS= read -r -d '' f; do
    if bash32_exempt "$f"; then continue; fi
    if exit_guard_has_trap "$root/$f"; then exit_guard_with_trap=$((exit_guard_with_trap + 1)); fi
    if ! out=$(exit_guard_scan_file "$root/$f"); then
      printf '%s\n' "$out" | sed "s|^|${f}: |"
      bad=1
    fi
  done < <(git -C "$root" ls-files -z -- '*.sh')
  return "$bad"
}

# 整仓检查（真正挂在 CI 上的那一步）：有问题就打印位置和一条 ::error::，返回 1。
exit_guard_check_repo() { # $1 仓根
  if ! exit_guard_scan_repo "$1"; then
    echo "::error::上面这些 *.sh 装了 EXIT trap，却没有带「走完了」标记的约定写法（写法见 scripts/ci/mac/selftest.sh 里这一节的注释）：macOS 自带的 bash 3.2 下变量展开出错会静默结束脚本，退出码可能是 0"
    return 1
  fi
  echo "走完了标记检查：git 跟踪的 *.sh 里有 ${exit_guard_with_trap} 个装了 EXIT trap（登记过的除外），全部带约定写法"
}

exit_guard_selftest() {
  local dir bad=0 eg_rc=0 eg_out=""
  dir=$(mktemp -d "${TMPDIR:-/tmp}/rinne.XXXXXX")
  # rinne-delete-ok: 自测里 mktemp -d 新建的目录
  trap 'rm -rf "$dir"' RETURN
  # 样例里的 trap 一词用变量 $T 拼出来，不直接写在本文件的行里：本文件自己也是被扫的 *.sh，
  # 免得它被当成装了 EXIT trap 的脚本。其余四样的写法放在下面的单引号里，本文件没有 trap 行，不受影响。
  local T=trap
  local init='RINNE_RAN_TO_END=0' fin='RINNE_RAN_TO_END=1'
  local rcl='  local rc=$?'
  local grd='  if [ "$rc" -eq 0 ] && [ "$RINNE_RAN_TO_END" != 1 ]; then'
  local msg='    echo "::error::脚本没有走完就退出了"; exit 1'
  local full_lines=('#!/bin/bash' 'set -euo pipefail' "$init" 'on_exit() {' "$rcl" "$grd" "$msg" '  fi' '}' "$T on_exit EXIT" 'work' "$fin")
  local l
  eg_expect() { # $1 说明 $2 期望 $3 实际
    if [ "$2" != "$3" ]; then echo "FAIL  ${1}（期望 ${2}，实际 ${3}）"; bad=1; else echo "PASS  $1"; fi
  }
  eg_write() { # $1 文件名 其余：一行一个参数
    local f=$1
    shift
    printf '%s\n' "$@" > "$dir/$f"
  }
  eg_full_without() { # $1 文件名 $2 要去掉的那一行
    local f=$1 skip=$2 line
    : > "$dir/$f"
    for line in "${full_lines[@]}"; do
      if [ "$line" != "$skip" ]; then printf '%s\n' "$line" >> "$dir/$f"; fi
    done
  }
  eg_verdict() { # $1 文件名 → hit（缺东西）| clean
    if exit_guard_scan_file "$dir/$1" > /dev/null; then echo clean; else echo hit; fi
  }
  eg_miss() { # $1 说明 $2 文件名 $3 输出里应当点名的片段：判红，并且点名缺的是哪一样
    local out rc=0
    out=$(exit_guard_scan_file "$dir/$2") || rc=$?
    if [ "$rc" != 0 ] && grep -Fq -- "$3" <<< "$out"; then echo "PASS  $1"; else echo "FAIL  ${1}（退出码 ${rc}，输出 '${out}'）"; bad=1; fi
  }

  # 正例：五样齐全
  eg_write good1.sh "${full_lines[@]}"
  eg_expect "好：五样齐全" clean "$(eg_verdict good1.sh)"
  # 坏样例：装了 EXIT trap，少其中一样（少一样就红，并且点名缺的那一样）
  eg_full_without bad1.sh "$init"
  eg_miss "坏：没有初始化 RINNE_RAN_TO_END=0" bad1.sh "缺 RINNE_RAN_TO_END=0"
  eg_full_without bad2.sh "$fin"
  eg_miss "坏：从不置位 RINNE_RAN_TO_END=1" bad2.sh "缺 RINNE_RAN_TO_END=1"
  eg_full_without bad3.sh "$rcl"
  eg_miss "坏：trap 函数里没有先取 rc=\$?" bad3.sh '缺 rc=$?'
  eg_full_without bad4.sh "$grd"
  eg_miss "坏：trap 里没有「退出码为 0、标记没置位」的判据" bad4.sh '（EXIT trap 处理函数里：退出码为 0'
  eg_full_without bad5.sh "$msg"
  eg_miss "坏：没有报错文字" bad5.sh "缺报错文字"
  # 坏样例：只写在注释里不算
  : > "$dir/bad6.sh"
  for l in "${full_lines[@]}"; do
    if [ "$l" = "$T on_exit EXIT" ]; then printf '%s\n' "$l" >> "$dir/bad6.sh"; else printf '%s\n' "# $l" >> "$dir/bad6.sh"; fi
  done
  eg_expect "坏：标记只写在注释里不算" hit "$(eg_verdict bad6.sh)"
  # 坏样例：各种装 EXIT trap 的写法都认得出来
  eg_write bad7.sh '#!/bin/bash' "if true; then $T 'rm -f x' EXIT; fi"
  eg_expect "坏：if 的一行分支里装 EXIT trap" hit "$(eg_verdict bad7.sh)"
  eg_write bad8.sh '#!/bin/bash' "$T 'rm -f x' 0"
  eg_expect "坏：trap 末尾写 0（等同 EXIT）" hit "$(eg_verdict bad8.sh)"
  eg_write bad9.sh '#!/bin/bash' 'f() {' "    $T 'rm -f x' EXIT" '}'
  eg_expect "坏：trap 装在函数里（缩进）" hit "$(eg_verdict bad9.sh)"
  eg_write bad10.sh '#!/bin/bash' "$T 'rm -f x' INT EXIT"
  eg_expect "坏：信号列表里带 EXIT" hit "$(eg_verdict bad10.sh)"
  # 坏样例：写法只是长得像
  eg_write bad11.sh '#!/bin/bash' 'PKG_RINNE_RAN_TO_END=0' "$rcl" "$grd" "$msg" "$fin" "$T cleanup EXIT"
  eg_miss "坏：变量名带前缀的同名写法不算初始化" bad11.sh "缺 RINNE_RAN_TO_END=0"
  eg_write bad12.sh '#!/bin/bash' "$init" 'RINNE_RAN_TO_END=10' "$rcl" "$grd" "$msg" "$T cleanup EXIT"
  eg_miss "坏：RINNE_RAN_TO_END=10 不算置位 1" bad12.sh "缺 RINNE_RAN_TO_END=1"
  # 正例：没装 EXIT trap 的不管；缩进、写在 case 分支里的齐全版本放行
  eg_write ok1.sh '#!/bin/bash' "$T 'echo x' RETURN"
  eg_expect "好：只有 RETURN trap" clean "$(eg_verdict ok1.sh)"
  eg_write ok2.sh '#!/bin/bash' "$T 'exit 130' INT" "$T 'exit 143' TERM"
  eg_expect "好：只有 INT、TERM trap" clean "$(eg_verdict ok2.sh)"
  eg_write ok3.sh '#!/bin/bash' "# $T 'rm -f x' EXIT"
  eg_expect "好：注释里提到 trap … EXIT" clean "$(eg_verdict ok3.sh)"
  eg_write ok4.sh '#!/bin/bash' "$T 'echo 0' INT"
  eg_expect "好：引号里的 0 不当成 EXIT" clean "$(eg_verdict ok4.sh)"
  : > "$dir/ok5.sh"
  eg_expect "好：空文件" clean "$(eg_verdict ok5.sh)"
  : > "$dir/ok6.sh"
  for l in "${full_lines[@]}"; do
    if [ "$l" = "$fin" ]; then printf '%s\n' '    --self-test) RINNE_RAN_TO_END=1; exit 0 ;;' >> "$dir/ok6.sh"; else printf '    %s\n' "$l" >> "$dir/ok6.sh"; fi
  done
  eg_expect "好：整体缩进、置位写在 case 分支里（RINNE_RAN_TO_END=1; exit 0）" clean "$(eg_verdict ok6.sh)"

  # S1：rc=$? 只认 EXIT trap 处理函数的函数体里、第一条命令的位置（可以带 local），带左边界。
  # 坏样例：处理函数里只有「长得像」的 foo_rc=$? 之类，没有 rc=$?（之前整个文件做子串匹配，这些都会被当成已经取了退出码）
  local rcn
  for rcn in foo_rc self_test_rc main_rc xrc; do
    eg_write "bad13-${rcn}.sh" '#!/bin/bash' "$init" 'on_exit() {' "  local ${rcn}"'=$?' "$grd" "$msg" '  fi' '}' "$T on_exit EXIT" "$fin"
    eg_miss "坏：处理函数里只有 ${rcn}=\$?，没有 rc=\$?" "bad13-${rcn}.sh" '缺 rc=$?'
  done
  # 坏样例：处理函数里没有，文件别处有（别的函数里的 rc=$?、到处都有的 || rc=$?、install-macos.sh 那样的 self_rc=$?）
  eg_write bad14.sh '#!/bin/bash' "$init" 'other() {' '  rc=$?' '  return "$rc"' '}' 'on_exit() {' "$grd" "$msg" '  fi' '}' "$T on_exit EXIT" 'work || rc=$?' 'main "$@" || main_rc=$?' 'self_test; self_rc=$?' "$fin"
  eg_miss "坏：rc=\$? 写在别的函数与别的行里，处理函数里没有" bad14.sh '缺 rc=$?'
  # 坏样例：rc=$? 不是处理函数的第一条命令（前面的命令已经把 $? 冲掉了），或者处理函数是空的
  eg_write bad15.sh '#!/bin/bash' "$init" 'on_exit() {' '  cleanup_temp "$tmp"' "$rcl" "$grd" "$msg" '  fi' '}' "$T on_exit EXIT" "$fin"
  eg_miss "坏：rc=\$? 不是处理函数的第一条命令" bad15.sh '缺 rc=$?'
  eg_write bad16.sh '#!/bin/bash' "$init" 'on_exit() {' '}' "$T on_exit EXIT" "$fin"
  eg_miss "坏：处理函数是空的" bad16.sh '缺 rc=$?'
  # 坏样例：判据与报错文字不在处理函数里，或者判据只是写在字符串里
  eg_write bad17.sh '#!/bin/bash' "$init" 'helper() {' "$grd" '  fi' '}' 'on_exit() {' "$rcl" "$msg" '}' "$T on_exit EXIT" "$fin"
  eg_miss "坏：「退出码为 0、标记没置位」的判据写在别的函数里" bad17.sh '（EXIT trap 处理函数里：退出码为 0'
  eg_write bad18.sh '#!/bin/bash' "$init" 'helper() {' "$msg" '}' 'on_exit() {' "$rcl" "$grd" '  fi' '}' "$T on_exit EXIT" "$fin"
  eg_miss "坏：报错文字写在别的函数里" bad18.sh "缺报错文字"
  eg_write bad19.sh '#!/bin/bash' "$init" 'on_exit() {' "$rcl" "  echo '[ \"\$rc\" -eq 0 ] && [ \"\$RINNE_RAN_TO_END\" != 1 ]'" "$msg" '}' "$T on_exit EXIT" "$fin"
  eg_miss "坏：判据只是写在字符串里（前后没有分隔符）" bad19.sh '（EXIT trap 处理函数里：退出码为 0'
  # 坏样例：认不出处理函数就判红
  eg_write bad20.sh '#!/bin/bash' "$init" 'on_exit() {' "$rcl" "$grd" "$msg" '  fi' '}' "$T \"on_exit\" EXIT" "$fin"
  eg_miss "坏：处理程序是带引号的命令（不是函数名）" bad20.sh "处理程序不是函数名"
  eg_write bad21.sh '#!/bin/bash' "$init" 'on_exit() {' "$rcl" "$grd" "$msg" '  fi' '}' "$T other_exit EXIT" "$fin"
  eg_miss "坏：处理函数找不到定义" bad21.sh "找不到 EXIT trap 处理函数 other_exit 的定义"
  eg_write bad22.sh '#!/bin/bash' "$init" 'on_exit() { local rc=$?; if [ "$rc" -eq 0 ] && [ "$RINNE_RAN_TO_END" != 1 ]; then echo "::error::脚本没有走完就退出了"; exit 1; fi; }' "$T on_exit EXIT" "$fin"
  eg_miss "坏：处理函数写在一行里（不认）" bad22.sh "找不到 EXIT trap 处理函数 on_exit 的定义"
  eg_write bad23.sh '#!/bin/bash' "$init" 'on_exit() {' "$rcl" "$grd" "$msg" '  fi' '  }' "$T on_exit EXIT" "$fin"
  eg_miss "坏：右花括号对不上定义行的缩进" bad23.sh "找不到函数体的结尾"
  # 两个 EXIT trap 处理函数：每个都要齐全
  eg_write bad24.sh '#!/bin/bash' "$init" 'h1() {' "$rcl" "$grd" "$msg" '  fi' '}' 'h2() {' "$rcl" "$msg" '}' "$T h1 EXIT" "$T h2 EXIT" "$fin"
  eg_miss "坏：两个处理函数，第二个缺判据" bad24.sh '（EXIT trap 处理函数里：退出码为 0'
  # rc=$? 后面紧跟别的字符不算；判据后面紧跟引号（写在双引号字符串里）不算；有函数处理程序之外，再来一个带引号的 EXIT trap 也判红（后装的会盖掉前一个）
  eg_write bad25.sh '#!/bin/bash' "$init" 'on_exit() {' "  local rc"'=$?x' "$grd" "$msg" '  fi' '}' "$T on_exit EXIT" "$fin"
  eg_miss "坏：rc=\$? 后面紧跟别的字符" bad25.sh '缺 rc=$?'
  eg_write bad26.sh '#!/bin/bash' "$init" 'on_exit() {' "$rcl" '  echo "x [ "$rc" -eq 0 ] && [ "$RINNE_RAN_TO_END" != 1 ]"' "$msg" '}' "$T on_exit EXIT" "$fin"
  eg_miss "坏：判据后面紧跟引号（写在字符串里）" bad26.sh '（EXIT trap 处理函数里：退出码为 0'
  eg_write bad27.sh '#!/bin/bash' "$init" 'on_exit() {' "$rcl" '  if x[ "$rc" -eq 0 ] && [ "$RINNE_RAN_TO_END" != 1 ]; then' "$msg" '  fi' '}' "$T on_exit EXIT" "$fin"
  eg_miss "坏：判据前面紧跟别的字符（粘在别的词后面）" bad27.sh '（EXIT trap 处理函数里：退出码为 0'
  eg_write bad28.sh '#!/bin/bash' "$init" 'on_exit() {' "$rcl" "$grd" "$msg" '  fi' '}' "$T on_exit EXIT" "$T 'rm -f x' EXIT" "$fin"
  eg_miss "坏：函数处理程序之外还有一个带引号的 EXIT trap" bad28.sh "处理程序不是函数名"
  # 正例：行首的 rc=$?（不带 local）、带注释与空行、function 关键字、带 -- 与别的信号、两个处理函数各自齐全
  eg_write ok7.sh '#!/bin/bash' "$init" 'on_exit() {' 'rc=$?' "$grd" "$msg" '  fi' '}' "$T on_exit EXIT" "$fin"
  eg_expect "好：处理函数里行首的 rc=\$?（不带 local）" clean "$(eg_verdict ok7.sh)"
  eg_write ok8.sh '#!/bin/bash' "$init" 'on_exit() {' '' '  # 先取退出码，别在前面放别的命令' '  local rc=$? # 退出码' "$grd" "$msg" '  fi' '}' "$T on_exit EXIT" "$fin"
  eg_expect "好：函数体里有注释与空行，rc=\$? 后面跟注释" clean "$(eg_verdict ok8.sh)"
  eg_write ok9.sh '#!/bin/bash' "$init" 'function on_exit {' "$rcl" "$grd" "$msg" '  fi' '}' "$T on_exit INT EXIT" "$fin"
  eg_expect "好：function 关键字的写法、信号列表里带别的信号" clean "$(eg_verdict ok9.sh)"
  eg_write ok10.sh '#!/bin/bash' "$init" 'on_exit () {' "$rcl" "$grd" "$msg" '  fi' '}' "$T -- on_exit EXIT" "$fin"
  eg_expect "好：trap -- 函数名 EXIT，定义写成「名字 () {」" clean "$(eg_verdict ok10.sh)"
  eg_write ok11.sh '#!/bin/bash' "$init" 'h1() {' "$rcl" "$grd" "$msg" '  fi' '}' 'h2() {' "$rcl" "$grd" "$msg" '  fi' '}' "$T h1 EXIT" "$T h2 EXIT" "$fin"
  eg_expect "好：两个处理函数各自齐全" clean "$(eg_verdict ok11.sh)"

  # 整仓（真走 git ls-files）：临时仓里放好文件 → 绿；再放坏文件 → 红并点名；登记路径里的坏写法放行；未跟踪的与非 .sh 不扫
  local repo="$dir/repo"
  mkdir -p "$repo/scripts" "$repo/tests/sanitizer-canary" "$repo/other"
  git -C "$repo" init -q
  git -C "$repo" config core.autocrlf false
  printf '%s\n' "${full_lines[@]}" > "$repo/scripts/good.sh"
  git -C "$repo" add scripts/good.sh
  if exit_guard_scan_repo "$repo" > /dev/null; then eg_expect "整仓扫描：只有好文件时通过" clean clean; else eg_expect "整仓扫描：只有好文件时通过" clean hit; fi
  eg_expect "整仓扫描：数得出装了 EXIT trap 的脚本个数" 1 "$exit_guard_with_trap"
  printf '%s\n' '#!/bin/bash' "$T 'rm -f x' EXIT" > "$repo/scripts/bad.sh"
  git -C "$repo" add scripts/bad.sh
  if exit_guard_scan_repo "$repo" > "$dir/scan.out"; then eg_expect "整仓扫描：混进一个没标记的脚本就红" hit clean; else eg_expect "整仓扫描：混进一个没标记的脚本就红" hit hit; fi
  eg_expect "整仓扫描：红的输出点名文件与缺的东西" "scripts/bad.sh: 缺 RINNE_RAN_TO_END=0（装 trap 之前初始化）" "$(sed -n '1p' "$dir/scan.out")"
  if exit_guard_check_repo "$repo" > "$dir/check.out" 2>&1; then eg_expect "整仓检查：有坏文件时返回非 0" hit clean; else eg_expect "整仓检查：有坏文件时返回非 0" hit hit; fi
  eg_expect "整仓检查：有坏文件时打出 error 标注" 1 "$(grep -c '^::error::' "$dir/check.out" || true)"
  git -C "$repo" rm -q -f --cached scripts/bad.sh
  if exit_guard_check_repo "$repo" > "$dir/check.out" 2>&1; then eg_expect "整仓检查：坏文件拿掉后返回 0" clean clean; else eg_expect "整仓检查：坏文件拿掉后返回 0" clean hit; fi
  printf '%s\n' '#!/bin/bash' "$T 'rm -f x' EXIT" > "$repo/tests/sanitizer-canary/run.sh"
  git -C "$repo" add tests/sanitizer-canary/run.sh
  if exit_guard_scan_repo "$repo" > /dev/null; then eg_expect "整仓扫描：登记过的路径里没标记也放行" clean clean; else eg_expect "整仓扫描：登记过的路径里没标记也放行" clean hit; fi
  printf '%s\n' '#!/bin/bash' "$T 'rm -f x' EXIT" > "$repo/other/run.sh"
  git -C "$repo" add other/run.sh
  if exit_guard_scan_repo "$repo" > /dev/null; then eg_expect "整仓扫描：同名文件在别的目录里不放行" hit clean; else eg_expect "整仓扫描：同名文件在别的目录里不放行" hit hit; fi
  git -C "$repo" rm -q -f --cached other/run.sh
  printf '%s\n' '#!/bin/bash' "$T 'rm -f x' EXIT" > "$repo/other/untracked.sh"
  printf '%s\n' '#!/bin/bash' "$T 'rm -f x' EXIT" > "$repo/other/notes.txt"
  git -C "$repo" add other/notes.txt
  if exit_guard_scan_repo "$repo" > /dev/null; then eg_expect "整仓扫描：未跟踪的 .sh 与非 .sh 文件不扫" clean clean; else eg_expect "整仓扫描：未跟踪的 .sh 与非 .sh 文件不扫" clean hit; fi
  # 接线：本文件末尾真的调用了整仓检查（行首匹配；把那一行删掉，这条就红）
  eg_expect "接线：末尾调用了整仓检查" 1 "$(grep -c '^exit_guard_check_repo "\$root"' "${BASH_SOURCE[0]}" || true)"

  # 动态用例：同样写法的最小脚本，注入一个未定义变量，用 /bin/bash 跑（mac 上就是 3.2），必须非 0。
  # 注入在 bash 4 以上本来就会让脚本以 1 退出，所以在本机或 Linux 上这几条只证明「注入确实让它红了」；3.2 上才是真正的检验。
  # 另有「半路 exit 0」的注入：它在任何版本的 bash 上都会以 0 退出，没有标记就看不出来、有标记才会红，所以标记的逻辑在哪个版本上都被验到。
  eg_min() { # $1 文件名 $2 注入（none | func | main | exit0） $3 guard | noguard
    local f=$1 inject=$2 mode=$3
    {
      printf '%s\n' '#!/bin/bash' 'set -euo pipefail'
      if [ "$mode" = guard ]; then
        printf '%s\n' "$init" 'on_exit() {' "$rcl" "$grd" "$msg" '  fi' '}' "$T on_exit EXIT"
      else
        printf '%s\n' "$T ':' EXIT"
      fi
      case "$inject" in
        func) printf '%s\n' 'work() { : "$rinne_injected_unset"; }' 'work' ;;
        main) printf '%s\n' ': "$rinne_injected_unset"' ;;
        exit0) printf '%s\n' 'exit 0' ;;
      esac
      printf '%s\n' 'echo reached-the-end'
      if [ "$mode" = guard ]; then printf '%s\n' "$fin"; fi
    } > "$dir/$f"
  }
  eg_run() { # $1 文件名 → eg_out、eg_rc
    eg_rc=0
    eg_out=$(/bin/bash "$dir/$1" 2>&1) || eg_rc=$?
  }
  eg_min m-none-guard.sh none guard
  eg_expect "最小脚本带标记的写法过静态检查" clean "$(eg_verdict m-none-guard.sh)"
  eg_min m-none-noguard.sh none noguard
  eg_expect "最小脚本不带标记的写法过不了静态检查" hit "$(eg_verdict m-none-noguard.sh)"
  eg_run m-none-guard.sh
  eg_expect "动态：不注入、带标记，正常走完，退出码 0" "0 reached-the-end" "${eg_rc} $(grep -o 'reached-the-end' <<< "$eg_out" || true)"
  eg_min m-func-guard.sh func guard
  eg_run m-func-guard.sh
  if [ "$eg_rc" != 0 ]; then eg_expect "动态：函数里展开未定义变量，退出码非 0" nonzero nonzero; else eg_expect "动态：函数里展开未定义变量，退出码非 0" nonzero "0，输出：${eg_out}"; fi
  eg_min m-main-guard.sh main guard
  eg_run m-main-guard.sh
  if [ "$eg_rc" != 0 ]; then eg_expect "动态：主流程里展开未定义变量，退出码非 0" nonzero nonzero; else eg_expect "动态：主流程里展开未定义变量，退出码非 0" nonzero "0，输出：${eg_out}"; fi
  eg_min m-exit0-guard.sh exit0 guard
  eg_run m-exit0-guard.sh
  if [ "$eg_rc" != 0 ] && grep -Fq '脚本没有走完就退出了' <<< "$eg_out" && ! grep -Fq 'reached-the-end' <<< "$eg_out"; then
    eg_expect "动态：半路 exit 0（模拟静默结束），带标记的退出码非 0 并打印原因" ok ok
  else
    eg_expect "动态：半路 exit 0（模拟静默结束），带标记的退出码非 0 并打印原因" ok "退出码 ${eg_rc}，输出：${eg_out}"
  fi
  eg_min m-exit0-noguard.sh exit0 noguard
  eg_run m-exit0-noguard.sh
  eg_expect "对照：同样的半路 exit 0，没有标记就以 0 退出（看不出来）" "0" "$eg_rc"
  # 对照：同样的注入、没有标记。3.2 上预期是 0（这就是要防的坑），bash 4 以上是 1；只报告，不判。
  eg_min m-func-noguard.sh func noguard
  eg_run m-func-noguard.sh
  echo "INFO  /bin/bash 版本 $(/bin/bash -c 'echo "${BASH_VERSION}"')：函数里展开未定义变量、没有标记的写法，退出码 ${eg_rc}（3.2 上预期 0，bash 4 以上是 1）"

  [ "$bad" = 0 ] || { echo "EXIT trap 标记检查的自测失败"; return 1; }
  echo "EXIT trap 标记检查的自测通过"
}

bash32_selftest
exit_guard_selftest
root=$(cd "$here/../../.." && pwd)
bash32_check_repo "$root" || exit 1
exit_guard_check_repo "$root" || exit 1
