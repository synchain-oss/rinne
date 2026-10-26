#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/release.json
#
# scripts/package-macos.sh —— macOS arm64 发行打包的唯一真源（CI 冒烟与发版共用这一份）。
# 与 scripts/package.ps1 并列：Windows 包走 .ps1，macOS 包走本脚本。硬要求在这里的落点：
#   1. zip 名由版本号算出：Rinne-v<ver>-macos-arm64.zip。版本缺省读根 CMakeLists.txt 的
#      project(Rinne VERSION x.y.z)，与 Windows 同一个真源；不写字面量。
#   2. 按目录枚举 bundle，断言**恰好** 2 个 .vst3 + 2 个 .component，名字逐字是
#      Rinne Input / Rinne Output，层级保住 <name>.<ext>/Contents/...；bundle 里**每个**
#      Mach-O 都只有 arm64 一个架构（胖二进制判红）。
#   3. .sha256 是独立文件，格式与 package.ps1 逐字一致（`<小写 hex><两个空格><zip 名>` + LF）；
#      另出 package-summary-macos-arm64.md（与 Windows 的 package-summary-win64.md 同一个表头，
#      文件名带平台，两边能放进同一个 Release）。
#   4. INSTALL.txt 由文档生成器渲染（gen-docs --emit install-txt --platform mac），不在这里抄第二份；
#      渲染后断言 GPLv3 第 6 条 (d) 的对应源码指引那几行在（tag、源码 commit、JUCE 的版本与 40 位 commit）。
#   5. zip 根目录带 LICENSE / THIRD-PARTY-NOTICES.md / LICENSES/ / third_party/notices/ / SHA256SUMS /
#      install-macos.sh / uninstall-macos.sh，与 Windows 包同一批合规文件（多两个脚本与两个 .component）。
#   6. 打包后断言：zip 根条目集合与打包清单相同、4 个 Contents/MacOS 条目全带可执行位、
#      禁入条目一个没有、解压回来逐文件比对字节、解出来的 4 个 bundle 再验一次 ad-hoc 签名、
#      对应源码指引那几行在（位置也对：标题后第一段，在「READ THIS FIRST」之前）；
#      .sha256 用 INSTALL.txt 里教用户的那条 `shasum -a 256 -c` 自验。
#
# **合规判据只有一份**：本脚本调 `pwsh scripts/package.ps1 -Preflight -PreflightOut <清单>`，
# 许可证全文覆盖、NOTICES 点名的声明文件、要打进 zip 的 LICENSES/ 与 third_party/notices/ 文件、
# 打包清单（清单文件本身也由 package.ps1 读，它把清单的行写进机读清单）都从那份清单来，
# 这里不重写第二套解析。所以本脚本需要 pwsh。
#
# mac 专属：
#   - 拷贝与压缩一律用 ditto（cp -r / zip -r 会丢符号链接与可执行位，用户解压后拿到加载不了的死壳）；
#     压缩带 --norsrc --noextattr --noacl，不产生 __MACOSX/ 条目。
#   - 确定性：暂存区里每个文件与目录的时间戳统一取 SOURCE_DATE_EPOCH / HEAD 提交时间
#     （都取不到才用 1980-01-01），合规文件与 INSTALL.txt 的权限统一 0644，
#     所以同一份输入重跑，zip 的 sha256 不变。
#
# 只用 macOS 自带的 bash 3.2 与 BSD 工具能跑通的写法：不用关联数组 / mapfile，不展开可能为空的数组，
# sed 只用 -E 与 POSIX 字符类，`find | head` 一律不用（pipefail 下 SIGPIPE）。
# 绝不在 workflow 里内联打包命令 —— 打包逻辑只在此处。
#
# 用法：
#   bash scripts/package-macos.sh [--version X.Y.Z[-pre]] [--build-dir build] [--out-dir dist]
#                                 [--source-commit <sha>] [--kit] [--kit-name <名字>]
#                                 [--build-run <链接>] [--validation <结论>] [--dry-run]
#                                 [--self-test [all|units|pipeline|real|subs]]

set -euo pipefail

PROG="package-macos"

# 仓根：本脚本在 <仓根>/scripts/ 下；取真实路径（解开符号链接），后面的路径判据都对着它。
REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"

# 期望的四个 bundle，名字逐字相符（与 package.ps1 的 $ExpectedBundles 同一组）。
EXPECTED_BUNDLES="Rinne Input.vst3
Rinne Output.vst3
Rinne Input.component
Rinne Output.component"

# preflight 清单里必须有的键（每个至少一行）。少一个就是清单不合契约。
REQUIRED_PF_KEYS="format version tag cmakeVersion sourceCommit sourceUrl sourceLine juceVersion juceCommit zipName shaName license notice cited ruleEn ruleZh manifest"

# 打包清单（zip 根条目清单）。由 package.ps1 -Preflight 读，本脚本只从机读清单里拿它的行。
MANIFEST_FILE="packaging/manifest-macos-arm64.txt"

# 本进程建的临时目录的名字前缀：删之前据此确认「这是本进程建的」。
TEMP_PREFIX="rinne-package-macos.$$"
TEMP_DIR=""

# 外部命令的名字：默认用系统自带的；自测用环境变量换成桩（桩只为让自测不依赖某台机器上装没装这些命令）。
ditto_cmd() { printf '%s' "${RINNE_DITTO:-ditto}"; }
lipo_cmd() { printf '%s' "${RINNE_LIPO:-lipo}"; }
file_cmd() { printf '%s' "${RINNE_FILE:-file}"; }
otool_cmd() { printf '%s' "${RINNE_OTOOL:-otool}"; }
unzip_cmd() { printf '%s' "${RINNE_UNZIP:-unzip}"; }
shasum_cmd() { printf '%s' "${RINNE_SHASUM:-shasum}"; }
strings_cmd() { printf '%s' "${RINNE_STRINGS:-strings}"; }
codesign_cmd() { printf '%s' "${RINNE_CODESIGN:-codesign}"; }

die() {
    echo "$PROG: $*" >&2
    exit 1
}

# 传给 pwsh / node 的路径。mac 与 Linux 上原样返回；在 Windows 的 Git Bash 里（开发机跑自测）
# 它们收的是 Windows 路径，要转一下。
native_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m -- "$1"
    else
        printf '%s' "$1"
    fi
}

# 必需的外部命令：逐个确认在 PATH 上（名字取自 *_cmd，自测换了桩也认）。
# 缺的一次列全，不是撞上一个就停。
tools_missing() {
    local tool miss=""
    for tool in "$(ditto_cmd)" "$(lipo_cmd)" "$(file_cmd)" "$(otool_cmd)" "$(unzip_cmd)" "$(shasum_cmd)" "$(strings_cmd)" "$(codesign_cmd)"; do
        command -v "$tool" >/dev/null 2>&1 || miss="$miss $tool"
    done
    printf '%s' "$miss"
}

usage() {
    cat <<'USAGE'
用法：package-macos.sh [选项]
  --version <X.Y.Z[-pre]>    不带前导 v 的版本号；不给则从 CMakeLists.txt 的 project(Rinne VERSION) 读
  --build-dir <路径>         放 2 个 .vst3 + 2 个 .component 的构建目录（递归找）；默认 build
  --out-dir <路径>           zip、.sha256 与 package-summary-macos-arm64.md 的输出目录；默认 dist
  --source-commit <sha>      写进 INSTALL.txt / summary 的源码提交；默认取本仓 HEAD
  --kit                      打 kit 包（多一个 tools/ 目录）；必须同时给 --kit-name
  --kit-name <名字>          kit 包的名字，形如 Rinne-kit-macos-arm64-<kit 版本>-<40 位 commit>
  --build-run <链接>         构建的运行链接，只写进 summary
  --validation <结论>        验证结论（如 auval 与 pluginval 的结果），只写进 summary
  --dry-run                  解析版本、跑合规 preflight、打印计划；什么都不构建
  --self-test [分组]         判据自测（坏样例与正例都跑一遍）；分组 all（默认）/ units / pipeline / real / subs
相对路径按当前目录解析（与 scripts/package.ps1 同口径）。
USAGE
}

# 相对路径按调用者的当前位置解析（绝对路径原样返回）。
abs_path() {
    case "$1" in
        /*) printf '%s' "$1" ;;
        *) printf '%s/%s' "$PWD" "$1" ;;
    esac
}

# ── 判据用的纯函数（只吃事实，不碰文件系统；自测直接喂夹具） ────────────────────

# 清单里某个键有没有出现（只看键在不在，不看值）。
# $1 清单文件 $2 键
pf_has() {
    awk -v k="$2" -F'\t' 'NF >= 1 && $1 == k { found = 1 } END { exit(found ? 0 : 1) }' "$1"
}

# 清单里某个键的所有值（同名键按出现顺序累积，一行一个）。
# $1 清单文件 $2 键
mf_all() {
    awk -v k="$2" '{ i = index($0, "\t"); if (i > 0 && substr($0, 1, i - 1) == k) print substr($0, i + 1) }' "$1"
}

# 清单里某个键的唯一值（必须恰好一行，否则判红）。
# $1 清单文件 $2 键
mf_one() {
    local n v
    n="$(mf_all "$1" "$2" | grep -c . || true)"
    [ "$n" = "1" ] || die "preflight 清单里 '$2' 应当恰好一行，实际 $n 行"
    v="$(mf_all "$1" "$2")"
    printf '%s' "$v"
}

# 文件里有没有 CR（删掉所有 CR 后字节数变少就是有）。不用 grep：有些平台的 grep 按文本模式读文件，会先吃掉 CR。
# $1 文件
file_has_cr() {
    local with without
    with="$(wc -c < "$1" | tr -d '[:space:]')"
    without="$(tr -d '\r' < "$1" | wc -c | tr -d '[:space:]')"
    [ "$with" != "$without" ]
}

# preflight 清单的契约：首行是 format<TAB>rinne-package-preflight/1、必需键一个不缺、
# 每行恰好一个 TAB（值里不许再有 TAB）、值非空、键非空且是单个词、全文没有 CR。
# 返回问题条数（0 = 通过），问题逐行打印。
# $1 清单文件
preflight_problems() {
    local f="$1" n=0 first bad key
    if [ ! -s "$f" ]; then
        echo "preflight 清单不存在或为空：$f"
        return 1
    fi
    if file_has_cr "$f"; then
        echo "preflight 清单里有 CR（必须全是 LF）"
        n=$((n + 1))
    fi
    first="$(head -n 1 "$f")"
    if [ "$first" != "$(printf 'format\trinne-package-preflight/1')" ]; then
        echo "preflight 清单首行不是 format<TAB>rinne-package-preflight/1，实际是 '$first'"
        n=$((n + 1))
    fi
    # 每行恰好一个 TAB：值里再有 TAB 就是两列以上，值里含 CRLF 会让这一行多出 CR（上面已拦）。
    bad="$(awk -F'\t' 'NF != 2 { print NR }' "$f")"
    if [ -n "$bad" ]; then
        echo "preflight 清单里这些行不是恰好一个 TAB（TAB / CR / LF 混进了键或值里）：$bad"
        n=$((n + 1))
    fi
    bad="$(awk -F'\t' 'NF == 2 && $2 == "" { print NR }' "$f")"
    if [ -n "$bad" ]; then
        echo "preflight 清单里这些行的值是空的：$bad"
        n=$((n + 1))
    fi
    bad="$(awk -F'\t' 'NF == 2 && $1 !~ /^[A-Za-z][A-Za-z0-9]*$/ { print NR }' "$f")"
    if [ -n "$bad" ]; then
        echo "preflight 清单里这些行的键不合法：$bad"
        n=$((n + 1))
    fi
    for key in $REQUIRED_PF_KEYS; do
        if ! pf_has "$f" "$key"; then
            echo "preflight 清单缺必需的键：$key"
            n=$((n + 1))
        fi
    done
    return "$n"
}

# 版本回落与 package.ps1 的一致性：两个解析器读出来的版本必须逐字相同。
# $1 本脚本读到的版本 $2 package.ps1 报的 cmakeVersion
version_pair_error() {
    if [ -z "$1" ]; then
        echo "从 CMakeLists.txt 读不到版本"
        return 1
    fi
    if [ -z "$2" ]; then
        echo "package.ps1 的清单里没有 cmakeVersion"
        return 1
    fi
    if [ "$1" != "$2" ]; then
        echo "本脚本读到版本 '$1'，package.ps1 读到 '$2'：两个解析器不一致"
        return 1
    fi
    return 0
}

# bundle 集合：恰好 2 个 .vst3 + 2 个 .component，名字逐字相符。
# 多一个、少一个、混进别的身份（探针）都红。
# $1 找到的 bundle 名字（一行一个）$2 期望的名字（一行一个）
bundle_set_error() {
    local found="$1" want="$2" n_vst3=0 n_comp=0 name bad=0 sorted_found sorted_want
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        case "$name" in
            *.vst3) n_vst3=$((n_vst3 + 1)) ;;
            *.component) n_comp=$((n_comp + 1)) ;;
            *)
                echo "bundle 名字既不是 .vst3 也不是 .component：$name"
                bad=$((bad + 1))
                ;;
        esac
        case "$name" in
            *[Pp]robe*) echo "发行包里不能有探针：$name"; bad=$((bad + 1)) ;;
        esac
    done <<EOF
$found
EOF
    if [ "$n_vst3" != "2" ]; then
        echo "期望恰好 2 个 .vst3 bundle（Rinne Input / Output），实际 $n_vst3 个"
        bad=$((bad + 1))
    fi
    if [ "$n_comp" != "2" ]; then
        echo "期望恰好 2 个 .component bundle（Rinne Input / Output），实际 $n_comp 个"
        bad=$((bad + 1))
    fi
    # 名字集合逐字相等：多的、少的、拼错的都算不符。
    sorted_found="$(printf '%s\n' "$found" | grep -v '^$' | LC_ALL=C sort || true)"
    sorted_want="$(printf '%s\n' "$want" | grep -v '^$' | LC_ALL=C sort || true)"
    if [ "$sorted_found" != "$sorted_want" ]; then
        echo "bundle 名字不是期望的那四个："
        printf '%s\n' "$sorted_found" | sed 's/^/  找到: /'
        printf '%s\n' "$sorted_want" | sed 's/^/  期望: /'
        bad=$((bad + 1))
    fi
    return "$bad"
}

# 架构：bundle 里每个 Mach-O 都只能有 arm64 一个架构。胖二进制（arm64 + x86_64）判红。
# $1 lipo -archs 的输出
macho_arch_error() {
    local archs
    archs="$(printf '%s' "$1" | tr -s '[:space:]' ' ' | sed 's/^ //; s/ $//')"
    if [ "$archs" != "arm64" ]; then
        echo "架构是 '$archs'，本包只出 arm64 单架构"
        return 1
    fi
    return 0
}

# zip 里 Contents/MacOS 下的可执行位：恰好 $2 个条目，首字符是 -（文件）或 l（符号链接），
# 且属主、属组、其他三组权限里都有 x。丢 x 位判红。
# 行首是 unzip -Z 的权限串（如 -rwxr-xr-x），行尾是条目名（名字里可以有空格）。
# $1 unzip -Z 的输出 $2 期望的条目数
zip_exec_bits_error() {
    local info="$1" want="$2" n bad
    n="$(printf '%s\n' "$info" | grep -cE '/Contents/MacOS/[^/]+[[:space:]]*$' || true)"
    if [ "$n" != "$want" ]; then
        echo "zip 里 Contents/MacOS 下的条目应当恰好 $want 个，实际 $n 个"
        return 1
    fi
    bad="$(printf '%s\n' "$info" | grep -E '/Contents/MacOS/[^/]+[[:space:]]*$' \
        | grep -vE '^[[:space:]]*[-l]..[xs]..[xs]..[xt]' || true)"
    if [ -n "$bad" ]; then
        echo "zip 里的可执行条目丢了 x 位："
        printf '%s\n' "$bad"
        return 1
    fi
    return 0
}

# kit 的 tools/rinne_nulltest 要在 zip 里并带 x 位（kit 的用途就是运行它）。
# $1 unzip -Z 的输出
zip_kit_tool_error() {
    local line
    line="$(printf '%s\n' "$1" | grep -E '[[:space:]]tools/rinne_nulltest[[:space:]]*$' || true)"
    if [ -z "$line" ]; then
        echo "zip 里没有 tools/rinne_nulltest 条目"
        return 1
    fi
    if ! printf '%s\n' "$line" | grep -qE '^[[:space:]]*[-l]..[xs]..[xs]..[xt]'; then
        echo "tools/rinne_nulltest 丢了 x 位："
        printf '%s\n' "$line"
        return 1
    fi
    return 0
}

# 禁入条目：__MACOSX/、.DS_Store、绝对路径、.. 段，一个都不能有。
# $1 unzip -Z1 的输出
forbidden_entry_lines() {
    printf '%s\n' "$1" | grep -E '(^|/)__MACOSX(/|$)|(^|/)\.DS_Store$|^/|(^|/)\.\.(/|$)' || true
}

# zip 的根条目：按第一段归到根单元。「根条目」= zip 内路径的第一段，目录单元带尾斜杠，顶层文件不带。
# third_party/ 下面只有 notices/，所以 third_party 这一段归到 third_party/notices/
# （只取第一段的话 third_party/notices/x 会被算成 third_party/，与清单对不上）。
# 已知的目录单元之外的第一段原样输出（带了子路径的会因为不带尾斜杠而与清单对不上，判红）。
# 同一个单元只输出一次。
# $1 unzip -Z1 的输出
zip_root_entries() {
    local e seg unit
    while IFS= read -r e; do
        [ -n "$e" ] || continue
        seg="${e%%/*}"
        if [ "$seg" = "$e" ]; then
            printf '%s\n' "$seg"
            continue
        fi
        unit="$seg"
        case "$seg" in
            "Rinne Input.vst3") unit="Rinne Input.vst3/" ;;
            "Rinne Output.vst3") unit="Rinne Output.vst3/" ;;
            "Rinne Input.component") unit="Rinne Input.component/" ;;
            "Rinne Output.component") unit="Rinne Output.component/" ;;
            "LICENSES") unit="LICENSES/" ;;
            "third_party") unit="third_party/notices/" ;;
            "tools") unit="tools/" ;;
        esac
        printf '%s\n' "$unit"
    done <<EOF | LC_ALL=C sort -u
$1
EOF
}

# third_party/ 下面只许有 notices/：返回 notices/ 以外的条目（一行一个），没有就什么都不打印。
# $1 unzip -Z1 的输出
third_party_stray_lines() {
    # ditto 会给每个目录写条目：third_party/ 这一条本身是 notices/ 的祖先，不算越界。
    printf '%s\n' "$1" | grep '^third_party/' | grep -v '^third_party/notices/' | grep -vx 'third_party/' || true
}

# 打包清单与 zip 根条目的比对：集合相等且数量相等才算通过（与 package.ps1 同一口径）。
# $1 清单文件（一行一个）$2 zip 根条目（每行一个）
manifest_root_error() {
    local want have bad=0 nw nh
    want="$(grep -v '^[[:space:]]*#' "$1" | grep -v '^[[:space:]]*$' | LC_ALL=C sort -u || true)"
    have="$(printf '%s\n' "$2" | grep -v '^[[:space:]]*$' | LC_ALL=C sort -u || true)"
    if [ -z "$want" ]; then
        echo "打包清单是空的：$1"
        bad=$((bad + 1))
    fi
    if [ "$want" != "$have" ]; then
        echo "zip 的根条目与打包清单不一致："
        diff <(printf '%s\n' "$want") <(printf '%s\n' "$have") || true
        bad=$((bad + 1))
    fi
    nw="$(printf '%s\n' "$want" | grep -c . || true)"
    nh="$(printf '%s\n' "$have" | grep -c . || true)"
    if [ "$nw" != "$nh" ]; then
        echo "根条目数量不对：清单 $nw 条，zip $nh 条"
        bad=$((bad + 1))
    fi
    return "$bad"
}

# 本机路径扫描：在一个文件里找本机路径（给定的字面量，以及盘符路径与 mac 的用户目录）。
# 命中片段逐行打印到 stdout；没有命中就什么都不打印。与 package.ps1 的同名判据同一口径。
# $1 文件 $2.. 要查的字面量
local_path_hits() {
    local file="$1"
    shift
    local lit form
    for lit in "$@"; do
        # 太短的字面量（如 HOME 是 /）会命中一切，当它没给。
        [ "${#lit}" -ge 4 ] || continue
        for form in "$lit" "$(printf '%s' "$lit" | tr '\\' '/')"; do
            [ -n "$form" ] || continue
            grep -F -m1 -o -- "$form" "$file" 2>/dev/null || true
        done
    done
    grep -oE '[A-Za-z]:[\\/][A-Za-z0-9_.$()+ -]{2,}[\\/][A-Za-z0-9_.$()+ -]{1,}' "$file" 2>/dev/null || true
    # /Users/Shared 是所有用户共用的目录，不是谁的主目录，不算。
    grep -oE '/(Users|home)/[A-Za-z0-9._-]+/' "$file" 2>/dev/null | grep -vx '/Users/Shared/' || true
}

# ── 临时目录 ──────────────────────────────────────────────────────────────────

# 本进程建的临时目录：真实路径必须在临时根下一层，且名字带本进程的前缀。删之前必过这一关。
temp_dir_is_ours() {
    local dir="$1" root_real real base
    [ -n "$dir" ] || return 1
    root_real="$(cd -- "${TMPDIR:-/tmp}" 2>/dev/null && pwd -P)" || return 1
    real="$(cd -- "$dir" 2>/dev/null && pwd -P)" || return 1
    base="$(basename -- "$real")"
    case "$base" in
        "$TEMP_PREFIX".*) ;;
        *) return 1 ;;
    esac
    [ "$(dirname -- "$real")" = "$root_real" ] || return 1
    return 0
}

# 删本进程建的临时目录。删不到只记原因，不强行删。
cleanup_temp() {
    local dir="${1:-$TEMP_DIR}"
    [ -n "$dir" ] || return 0
    if temp_dir_is_ours "$dir"; then
        # rinne-delete-ok: 本进程 mktemp -d 建的临时目录；temp_dir_is_ours 已核真实路径在临时根下一层且带本进程前缀
        rm -rf -- "$dir" || echo "$PROG: 临时目录没删掉：$dir" >&2
    else
        echo "$PROG: 临时目录不是本进程建的，不删：$dir" >&2
    fi
    if [ "$dir" = "$TEMP_DIR" ]; then TEMP_DIR=""; fi
    return 0
}

# 「走完了」标记（约定写法，scripts/ci/mac/selftest.sh 与本机 gate 会扫）：只有 main（或 --self-test）正常返回之后，文件末尾才把它置 1。
RINNE_RAN_TO_END=0

# 退出时的收尾：清掉本进程建的临时目录，并拦住「没走完却以 0 退出」。
# macOS 自带的 bash 3.2 在 set -u 下遇到未定义的变量（比如变量名后紧跟全角标点被当成变量名的一部分），会直接结束整个脚本，
# 而且装了 EXIT trap 时，进入 trap 的 $? 是出事前最后一条命令的状态，常常是 0：脚本半路死了，调用它的 CI 步骤却是绿的，
# 后面的上传步骤再报「没有文件」。所以到这里 $? 是 0、脚本却没走完，一律按失败处理。
on_exit() {
    local rc=$?
    cleanup_temp "$TEMP_DIR"
    if [ "$rc" -eq 0 ] && [ "$RINNE_RAN_TO_END" != 1 ]; then
        echo "$PROG: 脚本没有走完就退出了（bash 3.2 下变量展开出错会静默结束脚本，退出码是 0）：按失败处理" >&2
        exit 1
    fi
}
trap on_exit EXIT

# ── 输出目录的删除纪律 ────────────────────────────────────────────────────────

# 输出目录只写不删。删的只有「输出目录里的单个文件」：输出目录不能是盘根、仓根、仓根的上级目录、
# 用户目录或任何仓库目录；要删的文件名里不许有路径分隔符。
assert_safe_out_dir() {
    local full="$1" repo up parent home_real
    full="$(cd -- "$full" 2>/dev/null && pwd -P)" || die "输出目录不存在：$1"
    [ "$full" != "/" ] || die "输出目录不能是根目录：$full"
    repo="$(cd -- "$REPO_ROOT" && pwd -P)"
    [ "$full" != "$repo" ] || die "输出目录不能是仓根：$full"
    up="$repo"
    while [ -n "$up" ]; do
        parent="$(dirname -- "$up")"
        [ "$parent" != "$up" ] || break
        up="$parent"
        [ "$full" != "$up" ] || die "输出目录不能是仓根的上级目录：$full"
    done
    if [ -n "${HOME:-}" ]; then
        home_real="$(cd -- "$HOME" 2>/dev/null && pwd -P || printf '%s' "$HOME")"
        [ "$full" != "$home_real" ] || die "输出目录不能是用户目录：$full"
    fi
    [ ! -e "$full/.git" ] || die "输出目录不能是仓库目录：$full"
    printf '%s' "$full"
}

# 删输出目录里的一个旧产物：先断言文件落在输出目录的真实路径之内，再按单文件删。
# 不递归删、不删目录、不删参数传入的路径本身。
remove_out_file() {
    local dir="$1" name="$2" target
    case "$name" in
        */* | *\\* | "") return 0 ;;
    esac
    target="$dir/$name"
    [ -e "$target" ] || return 0
    case "$target" in
        "$dir"/*) rm -f -- "$target" || die "删不掉输出目录里的旧文件：$target" ;;
        *) die "要删的文件不在输出目录里：$target" ;;
    esac
}

# ── 版本回落 ──────────────────────────────────────────────────────────────────

# 从 CMakeLists.txt 读 project(Rinne ... VERSION x.y.z)。
# 与 scripts/lib/release-version.ps1 的 Get-RinneCMakeVersion 同一口径：行首锚定，
# 被 # 注释掉的 project(...) 不算，cmake_minimum_required 那一行不算，
# 项目名不是 Rinne 的不算，MYVERSION 这种词里含 VERSION 的也不算。
# 两个解析器必须给出同一个版本，不一致当场红（见 version_pair_error）。
# $1 CMakeLists.txt 的路径（缺省是仓根那份）
read_cmake_version() {
    local f="${1:-$REPO_ROOT/CMakeLists.txt}"
    awk '
        {
            if ($0 !~ /^[ \t]*project[ \t]*\([ \t]*Rinne([^A-Za-z0-9_]|$)/) next
            s = $0
            sub(/^[ \t]*project[ \t]*\([ \t]*/, "", s)
            # project(...) 可以拆成多行：读到第一个右括号为止（与 Get-RinneCMakeVersion 的 [^)]*? 同口径）。
            while (index(s, ")") == 0 && (getline nxt) > 0) s = s "\n" nxt
            p = index(s, ")")
            if (p > 0) s = substr(s, 1, p - 1)
            if (match(s, /(^|[^A-Za-z0-9_])VERSION[ \t]+[0-9]+\.[0-9]+\.[0-9]+/)) {
                seg = substr(s, RSTART, RLENGTH)
                sub(/.*VERSION[ \t]+/, "", seg)
                print seg
                exit
            }
        }
    ' "$f"
}

# ── 文档生成器（渲染 INSTALL.txt） ───────────────────────────────────────────

# 钉版 Node：RINNE_NODE 环境变量、git config rinne.node、PATH 上的 node（依次取）。
node_cmd() {
    local p="${RINNE_NODE:-}"
    [ -n "$p" ] || p="$(git -C "$REPO_ROOT" config --get rinne.node 2>/dev/null || true)"
    [ -n "$p" ] || p="node"
    printf '%s' "$p"
}

# 钉版 Node：主版本必须等于 .node-version 的主版本，否则判红（全局 Node 的版本不可信）。
# 通过时打印 node 命令。
pinned_node() {
    local node want have
    node="$(node_cmd)"
    want="$(tr -d '[:space:]' < "$REPO_ROOT/.node-version" | cut -d. -f1)"
    have="$("$node" --version 2>/dev/null | sed -nE 's/^v([0-9]+).*/\1/p')"
    if [ "$have" != "$want" ]; then
        echo "$PROG: 需要 Node ${want}（.node-version），得到 '${have:-找不到}'" >&2
        return 1
    fi
    printf '%s' "$node"
}

# 渲染 mac 版 INSTALL.txt。变量里的值全部来自 preflight 清单（唯一真源）。
# $1 变量 JSON
render_install_txt() {
    local node
    node="$(pinned_node)" || return 1
    (cd -- "$REPO_ROOT" && "$node" scripts/gen-docs.mjs --emit install-txt --platform mac --vars "$1")
}

# 渲染逐文件来源表（gen-docs --emit provenance）：发行包里那份 NOTICES 的「从哪来」一节由它填。
# 与 package.ps1 用同一个生成器、同一个钉版 Node。
render_provenance() {
    local node
    node="$(pinned_node)" || return 1
    (cd -- "$REPO_ROOT" && "$node" scripts/gen-docs.mjs --emit provenance)
}

# 某个固定串第一次出现在第几行（没有则不打印）。awk 的 index 是固定串匹配，不是正则。
# $1 全文 $2 固定串
text_line_of() {
    printf '%s\n' "$1" | awk -v pat="$2" 'index($0, pat) { print NR; exit }'
}

# 对应源码指引里的一行：必须在，且落在「READ THIS FIRST」所在行之前。有问题就打印一句并返回 1。
# $1 全文 $2 READ THIS FIRST 所在行（可空）$3 固定串 $4 缺的时候怎么说
source_needle_problem() {
    local at
    at="$(text_line_of "$1" "$3")"
    if [ -z "$at" ]; then
        echo "INSTALL.txt 缺对应源码指引的一行：$4"
        return 1
    fi
    if [ -n "$2" ] && [ "$at" -gt "$2" ]; then
        echo "INSTALL.txt 里对应源码指引的一行落在「READ THIS FIRST」之后：$4"
        return 1
    fi
    return 0
}

# INSTALL.txt 里必须有的对应源码指引那几行（GPLv3 第 6 条 (d)）。纯函数。
# 三层判据：逐行在；位置对（标题行之后的第一段就是这个块，各行都在「READ THIS FIRST」之前）；kit 另有一条。
# mac 上没有 WebView2，所以不查 WebView2 那一行。
# kit 没有 tag：链接要钉在 40 位 commit 上（$3 以 /tree/$4 结尾），正文里不许有 tree/v<版本> 的死链。
# $1 文本 $2 版本 $3 源码链接（已含 tag；kit 时是含 commit 的）$4 commit $5 JUCE 版本 $6 JUCE commit $7 是否 kit（1 或 0，缺省 0）
install_source_problems() {
    local text="$1" version="$2" source_url="$3" commit="$4" juce_version="$5" juce_commit="$6" kit="${7:-0}" bad=0
    local head_pat='Corresponding Source for this build (GPLv3' first read_line
    if [ "$kit" = "1" ]; then
        case "$source_url" in
            */tree/"$commit") ;;
            *) echo "kit 包没有 tag：对应源码链接要钉在 40 位 commit 上，实际是 $source_url"; bad=$((bad + 1)) ;;
        esac
        case "$text" in
            *"tree/v$version"*) echo "kit 包的 INSTALL.txt 里有指向 tree/v$version 的链接：kit 没有这个 tag，是死链"; bad=$((bad + 1)) ;;
        esac
    fi
    read_line="$(text_line_of "$text" 'READ THIS FIRST')"
    if [ -z "$read_line" ]; then
        echo "INSTALL.txt 缺「READ THIS FIRST」段：对应源码指引的位置无从核对"
        bad=$((bad + 1))
    fi
    first="$(printf '%s\n' "$text" | awk 'NR > 1 && NF { print; exit }')"
    case "$first" in
        "$head_pat"*) ;;
        *) echo "INSTALL.txt 标题之后的第一段不是对应源码指引（要以「${head_pat}」开头）"; bad=$((bad + 1)) ;;
    esac
    source_needle_problem "$text" "$read_line" "Rinne  $version: $source_url" "Rinne $version 对应 $source_url" || bad=$((bad + 1))
    source_needle_problem "$text" "$read_line" "(commit $commit)" "源码 commit $commit" || bad=$((bad + 1))
    source_needle_problem "$text" "$read_line" "JUCE   $juce_version" "JUCE $juce_version" || bad=$((bad + 1))
    source_needle_problem "$text" "$read_line" "/tree/$juce_commit" "JUCE 的 40 位 commit" || bad=$((bad + 1))
    source_needle_problem "$text" "$read_line" "Build steps: docs/BUILDING.md" "构建说明的路径" || bad=$((bad + 1))
    case "$text" in
        *"{{"* | *"}}"*) echo "INSTALL.txt 里还有未替换的占位符"; bad=$((bad + 1)) ;;
    esac
    return "$bad"
}

# kit 没有 tag（不打演练 tag，kit 的构建提交也不在任何 tag 上）：把 preflight 清单里含 tag 的源码链接
# 改成钉在 40 位 commit 上的。纯函数。
# $1 清单里的 sourceUrl（形如 …/tree/<tag>）$2 40 位 commit
kit_source_url() {
    printf '%s/tree/%s' "${1%/tree/*}" "$2"
}

# INSTALL.txt 里必须带上 preflight 清单点名的使用前必读规则（ruleEn / ruleZh 各一行一条，逐字）。
# 规则原文只在文档真源里，包里的说明少了其中一条就红。
# $1 INSTALL.txt 全文 $2 规则（一行一条）
install_rule_problems() {
    local text="$1" rules="$2" rule bad=0
    while IFS= read -r rule; do
        [ -n "$rule" ] || continue
        case "$text" in
            *"$rule"*) ;;
            *) echo "INSTALL.txt 缺使用前必读的一条：$(printf '%s' "$rule" | cut -c1-40)…"; bad=$((bad + 1)) ;;
        esac
    done <<EOF
$2
EOF
    return "$bad"
}

# ── 暂存区 ────────────────────────────────────────────────────────────────────

# 把四个 bundle 复制进暂存区（ditto 保住可执行位与符号链接）。
# $1 暂存区 $2 找到的 bundle 路径（一行一个）
stage_bundles() {
    local stage="$1" found="$2" src
    while IFS= read -r src <&3; do
        [ -n "$src" ] || continue
        "$(ditto_cmd)" --norsrc --noextattr --noacl "$src" "$stage/$(basename -- "$src")" || return 1
    done 3<<EOF
$found
EOF
}

# 把 LICENSE、LICENSES/ 与 third_party/notices/ 复制进暂存区。要复制哪些文件完全取自 preflight 清单。
# THIRD-PARTY-NOTICES.md 另走 stage_notices（要插来源表）。
# $1 暂存区 $2 preflight 清单
stage_compliance() {
    local stage="$1" pf="$2" rel
    cp "$REPO_ROOT/LICENSE" "$stage/LICENSE" || return 1
    while IFS= read -r rel <&3; do
        [ -n "$rel" ] || continue
        mkdir -p "$stage/$(dirname -- "$rel")" || return 1
        cp "$REPO_ROOT/$rel" "$stage/$rel" || return 1
    done 3< <({ mf_all "$pf" license; mf_all "$pf" notice; } | LC_ALL=C sort -u)
}

# 把逐文件来源表插进 NOTICES 的 scvb-derived 小节，插在小节结束标记之前。
# 与 scripts/package.ps1 的 Add-RinneProvenanceTable 同一口径：标记之前的正文去掉尾部空白，
# 空一行放表（表也去掉尾部空白），再空一行接结束标记。
# 开始标记或结束标记不在、或结束标记在开始标记之前，返回 1（不打包半张表，也不打包没有来源表的 NOTICES）。
# $1 NOTICES 文件 $2 来源表文件；结果写到标准输出
provenance_inject() {
    (
        export LC_ALL=C
        local open='<!-- section:scvb-derived -->' close='<!-- /section:scvb-derived -->'
        local txt table head tail pre_open
        txt="$(cat -- "$1"; printf x)"
        txt="${txt%x}"
        table="$(cat -- "$2"; printf x)"
        table="${table%x}"
        case "$txt" in
            *"$open"*"$close"*) ;;
            *) exit 1 ;;
        esac
        pre_open="${txt%%"$open"*}"
        head="${txt%%"$close"*}"
        [ "${#pre_open}" -lt "${#head}" ] || exit 1
        tail="${txt#"$head"}"
        while :; do
            case "$head" in *[[:space:]]) head="${head%?}" ;; *) break ;; esac
        done
        while :; do
            case "$table" in *[[:space:]]) table="${table%?}" ;; *) break ;; esac
        done
        [ -n "$table" ] || exit 1
        printf '%s\n\n%s\n\n%s' "$head" "$table" "$tail"
    )
}

# 包里那份 NOTICES：仓里那份加上渲染出来的逐文件来源表（插不进去就红）。
# $1 暂存区 $2 来源表文件
stage_notices() {
    provenance_inject "$REPO_ROOT/THIRD-PARTY-NOTICES.md" "$2" > "$1/THIRD-PARTY-NOTICES.md" \
        || die "THIRD-PARTY-NOTICES.md 里没有 scvb-derived 小节（或标记顺序不对），逐文件来源表插不进去"
}

# 把安装与卸载脚本复制进暂存区（发行包里带着，用户解压后就能装 / 卸）。
# $1 暂存区
stage_install_scripts() {
    local stage="$1"
    cp "$REPO_ROOT/packaging/macos/install-macos.sh" "$stage/install-macos.sh" || return 1
    cp "$REPO_ROOT/packaging/macos/uninstall-macos.sh" "$stage/uninstall-macos.sh" || return 1
}

# kit 的工具：只有 --kit 才打进去，发行包里没有它。找法与 package.ps1 同一口径：
# <构建目录>/tools/ 与 <构建目录>/tools-bin/Release/，都没有再在 tools-bin/ 下找。
# $1 暂存区 $2 构建目录
stage_kit_tool() {
    local stage="$1" build_dir="$2" cand tool_path=""
    for cand in "$build_dir/tools/rinne_nulltest" "$build_dir/tools-bin/Release/rinne_nulltest"; do
        if [ -f "$cand" ] && [ -x "$cand" ]; then tool_path="$cand"; break; fi
    done
    if [ -z "$tool_path" ] && [ -d "$build_dir/tools-bin" ]; then
        tool_path="$(find "$build_dir/tools-bin" -type f -name rinne_nulltest -perm -100 -print | LC_ALL=C sort | sed -n 1p)"
    fi
    [ -n "$tool_path" ] || die "带 --kit 但构建目录里没有 rinne_nulltest（找过 tools/ 与 tools-bin/）"
    mkdir -p "$stage/tools" || return 1
    "$(ditto_cmd)" --norsrc --noextattr --noacl "$tool_path" "$stage/tools/rinne_nulltest" || return 1
}

# SHA256SUMS：包里除它自己以外每个文件的哈希（sha256sum 格式，路径相对包根，LF，按字节序排）。
# 与 package.ps1 写出来的那份同一格式。
# $1 暂存区 $2 写到哪里（不能在暂存区里面，否则会把自己算进去）
write_sha256sums() {
    local stage="$1" out="$2" f rel
    : > "$out" || return 1
    while IFS= read -r f <&3; do
        rel="${f#"$stage/"}"
        printf '%s  %s\n' "$("$(shasum_cmd)" -a 256 "$f" | awk '{ print $1 }')" "$rel" >> "$out"
    done 3< <(find "$stage" -type f | LC_ALL=C sort)
}

# 确定性：不让构建机的 umask、检出方式和文件时间进到 zip 里。
#   目录一律 0755；
#   bundle 与 tools/ 里的文件：属主有 x 位的 0755，其余 0644（可执行位是判据要求的，其余位固定）；
#   其余文件（合规文件、INSTALL.txt、两个脚本、SHA256SUMS）一律 0644；
#   每个文件与目录的时间戳统一取 epoch（SOURCE_DATE_EPOCH > HEAD 提交时间 > 1980-01-01）。
# zip 里的 DOS 时间是「本地时间」：暂存与压缩都钉在 UTC，换台机器、换个时区，条目时间不变。
# $1 暂存区 $2 epoch
normalize_stage() {
    local stage="$1" epoch="$2" touch_ts
    touch_ts="$(date -u -r "$epoch" '+%Y%m%d%H%M.%S' 2>/dev/null || date -u -d "@$epoch" '+%Y%m%d%H%M.%S')"
    export TZ=UTC
    find "$stage" -type d -exec chmod 0755 {} + || return 1
    find "$stage" -type f ! -path '*.vst3/*' ! -path '*.component/*' ! -path "$stage/tools/*" \
        -exec chmod 0644 {} + || return 1
    find "$stage" -type f \( -path '*.vst3/*' -o -path '*.component/*' -o -path "$stage/tools/*" \) -perm -100 \
        -exec chmod 0755 {} + || return 1
    find "$stage" -type f \( -path '*.vst3/*' -o -path '*.component/*' -o -path "$stage/tools/*" \) ! -perm -100 \
        -exec chmod 0644 {} + || return 1
    find "$stage" -exec touch -h -t "$touch_ts" {} + || return 1
}

# 条目时间戳：SOURCE_DATE_EPOCH > HEAD 提交时间 > 1980-01-01（zip 的 DOS 时间下限）。
# $1 仓根
stage_epoch() {
    local epoch
    epoch="${SOURCE_DATE_EPOCH:-}"
    case "$epoch" in '' | *[!0-9]*) epoch="$(git -C "$1" log -1 --format=%ct 2>/dev/null || true)" ;; esac
    case "$epoch" in '' | *[!0-9]*) epoch=315532800 ;; esac
    # 与 package.ps1 同一口径：不低于 1980-01-01（zip 的 DOS 时间下限；更早的值写进 DOS 时间会下溢），
    # 取整到偶数秒（DOS 时间只有两秒粒度）。位数过多的当作无效（免得算术溢出）。
    [ "${#epoch}" -le 12 ] || epoch=315532800
    if [ "$epoch" -lt 315532800 ]; then epoch=315532800; fi
    epoch=$((epoch - epoch % 2))
    printf '%s' "$epoch"
}

# 压缩：ditto -c -k，不加 --keepParent（暂存区内容平铺到 zip 根，与 package.ps1 一致）。
# 带 --norsrc --noextattr --noacl：不产生 __MACOSX/ 条目，也不带扩展属性。
# $1 暂存区 $2 zip 路径
pack_zip() {
    "$(ditto_cmd)" -c -k --norsrc --noextattr --noacl "$1" "$2"
}

# kit 包的名字：形如 Rinne-kit-macos-arm64-<kit 版本>-<40 位 commit>。不许空、不许以点开头，
# 只放行文件名里安全的字符（挡住路径分隔符、空白、换行）。
# $1 名字
kit_name_problem() {
    case "$1" in
        "") echo "--kit 必须同时给 --kit-name"; return 1 ;;
        .*) echo "--kit-name 不能以点开头：'$1'"; return 1 ;;
        *[!0-9A-Za-z._+-]*) echo "--kit-name 里只许字母、数字和 . _ + -：'$1'"; return 1 ;;
    esac
    return 0
}

# ── bundle 检查（读真实文件，外部命令可换桩） ────────────────────────────────

# bundle 里每个 Mach-O 都只有 arm64 一个架构。至少要有一个 Mach-O。问题逐行打印，返回问题条数。
# $1 bundle 目录
bundle_arch_problems() {
    local bundle="$1" f archs n=0 macho=0
    while IFS= read -r -d '' f <&3; do
        case "$("$(file_cmd)" -b "$f" 2>/dev/null)" in
            *Mach-O*) ;;
            *) continue ;;
        esac
        macho=$((macho + 1))
        archs="$("$(lipo_cmd)" -archs "$f" 2>&1)" || archs="lipo 读不出：$archs"
        if ! macho_arch_error "$archs" >/dev/null; then
            echo "${f}：架构是 '$(printf '%s' "$archs" | tr -s '[:space:]' ' ')'，本包只出 arm64 单架构"
            n=$((n + 1))
        fi
    done 3< <(find "$bundle" -type f -print0)
    if [ "$macho" -lt 1 ]; then
        echo "$bundle 里没有 Mach-O 文件"
        n=$((n + 1))
    fi
    return "$n"
}

# bundle 里每个 Mach-O 的字符串里都不许有本机路径（给定的字面量、盘符路径、用户主目录）。
# 问题逐行打印，返回问题条数。
# $1 bundle 目录 $2 草稿文件（放 strings 的输出）其余：要查的本机路径字面量
bundle_path_problems() {
    local bundle="$1" scratch="$2" f hits n=0
    shift 2
    while IFS= read -r -d '' f <&3; do
        case "$("$(file_cmd)" -b "$f" 2>/dev/null)" in
            *Mach-O*) ;;
            *) continue ;;
        esac
        "$(strings_cmd)" -a "$f" > "$scratch" 2>/dev/null || true
        hits="$(local_path_hits "$scratch" "$@")"
        if [ -n "$hits" ]; then
            echo "$f 含有本机路径：$(printf '%s\n' "$hits" | sed -n 1p)"
            n=$((n + 1))
        fi
    done 3< <(find "$bundle" -type f -print0)
    return "$n"
}

# 这一行在不在这份清单里（逐行整行比对）。用 here-string 喂，不经管道（pipefail 下 grep -q 提前退出会误报）。
# $1 清单（一行一个）$2 要找的行
list_has() {
    grep -Fqx -- "$2" <<EOF
$1
EOF
}

# 判据函数的统一收口：退出码非 0，或者有任何输出（问题清单），都算红。
# 判据函数中途意外退出（退出码非 0 而没有输出）也算红，不会被当成「没发现问题」放过去。
# $1 红了时的说明；其余：判据函数与它的参数
gate() {
    local msg="$1" out rc=0
    shift
    out="$("$@")" || rc=$?
    if [ "$rc" -ne 0 ] || [ -n "$out" ]; then
        if [ -n "$out" ]; then printf '%s\n' "$out" >&2; fi
        die "$msg"
    fi
}

# 版本串会进文件名与链接：只放行 x.y.z 或 x.y.z-<预发布标识>（与 package.ps1 同一形态），不带前导 v。
# 整串匹配（bash 的 =~ 里 $ 只在串尾；grep 是逐行匹配，带换行的串能混过去，所以不用它）。
# $1 版本
version_shape_problem() {
    local re='^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z]+(\.[0-9A-Za-z]+)*)?$'
    if [[ "$1" =~ $re ]]; then return 0; fi
    echo "版本号形态不对：'$1'（应为 x.y.z 或 x.y.z-<预发布标识>，不带前导 v）"
    return 1
}

# zip 条目里带 probe 的（探针不进发行包；不分大小写）。有就逐行打印。
# $1 unzip -Z1 的输出
probe_entry_lines() {
    printf '%s\n' "$1" | grep -i probe || true
}

# ── 主流程 ────────────────────────────────────────────────────────────────────

main() {
    local version="" version_from="" kit=0 kit_name="" build_dir="build" out_dir="dist"
    local build_run="" validation="" dry_run=0 source_commit=""

    while [ $# -gt 0 ]; do
        case "$1" in
            --version)       [ $# -ge 2 ] || die "--version 需要一个值"; version="$2"; version_from="--version"; shift 2 ;;
            --build-dir)     [ $# -ge 2 ] || die "--build-dir 需要一个值"; build_dir="$2"; shift 2 ;;
            --out-dir)       [ $# -ge 2 ] || die "--out-dir 需要一个值"; out_dir="$2"; shift 2 ;;
            --source-commit) [ $# -ge 2 ] || die "--source-commit 需要一个值"; source_commit="$2"; shift 2 ;;
            --kit)           kit=1; shift ;;
            --kit-name)      [ $# -ge 2 ] || die "--kit-name 需要一个值"; kit_name="$2"; shift 2 ;;
            --build-run)     [ $# -ge 2 ] || die "--build-run 需要一个值"; build_run="$2"; shift 2 ;;
            --validation)    [ $# -ge 2 ] || die "--validation 需要一个值"; validation="$2"; shift 2 ;;
            --dry-run)       dry_run=1; shift ;;
            -h | --help)     usage; return 0 ;;
            *)               usage >&2; die "不认识的参数：$1" ;;
        esac
    done

    build_dir="$(abs_path "$build_dir")"
    out_dir="$(abs_path "$out_dir")"

    # ── 版本 ──────────────────────────────────────────────────────────────────
    # 参数优先；**没传** --version 才从 CMakeLists.txt 回落。传了 --version "" 属于调用方没算出版本号，
    # 静默回落会产出版本对不上的资产，所以直接 die。
    if [ "$version_from" = "--version" ]; then
        [ -n "$version" ] || die "--version 给了空串：调用方没算出版本号（不回落到 CMakeLists.txt）"
    else
        version="$(read_cmake_version)"
        [ -n "$version" ] || die "从 CMakeLists.txt 读不到 project(Rinne VERSION x.y.z)"
        version_from="CMakeLists.txt"
    fi
    version="${version#v}"
    local vshape
    vshape="$(version_shape_problem "$version")" || die "$vshape"

    if [ -z "$source_commit" ]; then
        source_commit="$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || true)"
    fi
    if [ "${#source_commit}" -ne 40 ]; then
        die "源码 commit 不是 40 位小写十六进制：'$source_commit'"
    fi
    case "$source_commit" in
        *[!0-9a-f]*) die "源码 commit 不是 40 位小写十六进制：'$source_commit'" ;;
    esac

    if [ "$kit" -eq 1 ]; then
        local kerr
        kerr="$(kit_name_problem "$kit_name")" || die "$kerr"
    elif [ -n "$kit_name" ]; then
        die "--kit-name 只能与 --kit 一起用"
    fi

    # ── 合规 preflight（唯一真源 = scripts/package.ps1） ───────────────────────
    command -v pwsh >/dev/null 2>&1 \
        || die "找不到 pwsh：合规判据与打包清单都在 scripts/package.ps1 -Preflight 里"
    local work
    work="$(mktemp -d "${TMPDIR:-/tmp}/${TEMP_PREFIX}.XXXXXX")" || die "建不出临时目录"
    TEMP_DIR="$work"
    trap 'exit 130' INT
    trap 'exit 143' TERM

    local pf manifest
    pf="$work/preflight.tsv"
    manifest="$work/manifest.txt"
    pwsh -NoProfile -File "$(native_path "$REPO_ROOT/scripts/package.ps1")" -Preflight \
        -PreflightOut "$(native_path "$pf")" -Manifest "$MANIFEST_FILE" \
        -Version "$version" -SourceCommit "$source_commit" \
        || die "scripts/package.ps1 -Preflight 没过（看它上面的输出）"
    [ -s "$pf" ] || die "package.ps1 -Preflight 没写出清单"

    gate "preflight 清单不合契约" preflight_problems "$pf"

    # 清单里的版本与本脚本的版本必须一致；回落读的版本还要与 package.ps1 的解析器读到的一致。
    local mf_version mf_cmake tag source_url source_ref juce_version juce_commit
    mf_version="$(mf_one "$pf" version)"
    mf_cmake="$(mf_one "$pf" cmakeVersion)"
    [ "$mf_version" = "$version" ] \
        || die "package.ps1 报的版本是 '$mf_version'，本脚本用的是 '$version'"
    if [ "$version_from" = "CMakeLists.txt" ]; then
        local verr
        verr="$(version_pair_error "$version" "$mf_cmake")" || die "版本回落与 package.ps1 不一致：$verr"
    fi
    tag="$(mf_one "$pf" tag)"
    source_url="$(mf_one "$pf" sourceUrl)"
    # 对应源码链接钉在哪：发行包钉 tag；kit 没有 tag，钉 40 位 commit（钉在不存在的 tag 上，主链接就是 404）。
    source_ref="$tag"
    if [ "$kit" -eq 1 ]; then
        source_url="$(kit_source_url "$source_url" "$source_commit")"
        source_ref="$source_commit"
    fi
    juce_version="$(mf_one "$pf" juceVersion)"
    juce_commit="$(mf_one "$pf" juceCommit)"

    local zip_name sha_name summary_name
    zip_name="Rinne-v$version-macos-arm64.zip"
    summary_name="package-summary-macos-arm64.md"
    if [ "$kit" -eq 1 ]; then
        zip_name="$kit_name.zip"
        # kit 包与发行包可能落在同一个输出目录：summary 不同名，发行包的那份不能被 kit 包顶掉。
        summary_name="package-summary-$kit_name.md"
    fi
    sha_name="$zip_name.sha256"

    # 清单里的路径要拼进暂存区：只放行仓库内的相对路径，且文件要在。
    local rel
    while IFS= read -r rel <&3; do
        case "$rel" in
            /* | *..* | "") die "preflight 清单里有一条意外的路径：'$rel'" ;;
        esac
        [ -f "$REPO_ROOT/$rel" ] || die "preflight 清单列出 '$rel'，但仓里没有这个文件"
    done 3< <({ mf_all "$pf" license; mf_all "$pf" notice; } | LC_ALL=C sort -u)

    # 打包清单（zip 根条目清单）由 package.ps1 读并写进机读清单，这里从那里拿，不自己再解析一遍。
    # kit 包多一个 tools/ 目录。
    mf_all "$pf" manifest > "$manifest"
    [ -s "$manifest" ] || die "preflight 清单里没有打包清单的行"
    if [ "$kit" -eq 1 ]; then printf 'tools/\n' >> "$manifest"; fi

    if [ "$dry_run" -eq 1 ]; then
        echo "[DryRun] 仓根      : $REPO_ROOT"
        echo "[DryRun] 版本      : ${version}（来自 ${version_from}；package.ps1 读到的 cmakeVersion 是 ${mf_cmake}）"
        echo "[DryRun] tag       : $tag"
        echo "[DryRun] 构建目录  : $build_dir"
        echo "[DryRun] 输出目录  : $out_dir"
        echo "[DryRun] zip       : $out_dir/$zip_name"
        echo "[DryRun] sha256    : $out_dir/$sha_name"
        echo "[DryRun] summary   : $out_dir/$summary_name"
        echo "[DryRun] kit       : $(if [ "$kit" -eq 1 ]; then echo "是（${kit_name}）"; else echo '否'; fi)"
        echo "[DryRun] 合规      : $(mf_all "$pf" license | grep -c . || true) 份随包许可文件、$(mf_all "$pf" cited | grep -c . || true) 条被点名的声明路径（都由 package.ps1 -Preflight 核过）"
        echo "[DryRun] 好了 —— 什么都没构建。"
        return 0
    fi

    # ── 工具与输入 ───────────────────────────────────────────────────────────
    local miss
    miss="$(tools_missing)"
    [ -z "$miss" ] || die "缺少必需的 macOS 工具：$miss"
    [ -d "$build_dir" ] || die "构建目录不存在：$build_dir"
    [ -f "$REPO_ROOT/$MANIFEST_FILE" ] || die "仓库缺 $MANIFEST_FILE"
    [ -f "$REPO_ROOT/packaging/macos/install-macos.sh" ] || die "仓库缺 packaging/macos/install-macos.sh"
    [ -f "$REPO_ROOT/packaging/macos/uninstall-macos.sh" ] || die "仓库缺 packaging/macos/uninstall-macos.sh"

    # 输出目录只写不删：不存在就建，建完核对它不是盘根、仓根、仓根的上级、用户目录或仓库目录。
    mkdir -p -- "$out_dir" || die "建不出输出目录：$out_dir"
    local out_real
    out_real="$(assert_safe_out_dir "$out_dir")" || exit 1

    # 输出目录里同名的旧产物先删（只按文件名删单个文件）：这一次中途红了，
    # 不能留下上一次的 zip 冒充这一次的结果。新产物先在临时目录里做完全部断言，最后才挪进输出目录。
    remove_out_file "$out_real" "$zip_name"
    remove_out_file "$out_real" "$sha_name"
    remove_out_file "$out_real" "$summary_name"

    # ── 枚举 bundle（硬要求 2） ───────────────────────────────────────────────
    # 跳过 FetchContent 的 _deps/；命中一个 bundle 就不再往里找（-prune），bundle 里不该再套 bundle。
    local found names
    found="$(find "$build_dir" -name _deps -prune -o -type d \( -name '*.vst3' -o -name '*.component' \) -print -prune | LC_ALL=C sort)"
    names="$(printf '%s\n' "$found" | while IFS= read -r rel; do if [ -n "$rel" ]; then basename -- "$rel"; fi; done)"
    gate "bundle 集合不对" bundle_set_error "$names" "$EXPECTED_BUNDLES"

    # 每个 bundle：Contents/Info.plist、Contents/MacOS/<名字> 可执行且是 Mach-O；bundle 里**每个** Mach-O 都只有
    # arm64 一个架构（带 x86_64 的胖二进制或纯 x86_64 都红 —— 只支持 Apple Silicon 是 INSTALL.txt 写给用户的承诺）；
    # 字符串里不许有本机路径。
    local build_real name stem exe minos problems
    build_real="$(cd -- "$build_dir" && pwd -P)"
    : > "$work/minos.txt"
    while IFS= read -r rel <&3; do
        [ -n "$rel" ] || continue
        name="$(basename -- "$rel")"
        stem="${name%.*}"
        [ -f "$rel/Contents/Info.plist" ] || die "'$rel' 没有 Contents/Info.plist"
        exe="$rel/Contents/MacOS/$stem"
        { [ -f "$exe" ] && [ -x "$exe" ]; } || die "'$exe' 不存在或没有可执行位"
        gate "'$name' 的架构不合格" bundle_arch_problems "$rel"
        gate "'$name' 的二进制里有本机路径" bundle_path_problems "$rel" "$work/strings.txt" "$build_dir" "$build_real" "$REPO_ROOT" "${HOME:-}"
        "$(otool_cmd)" -l "$exe" > "$work/otool.txt" 2>/dev/null || die "'$exe' 的 otool -l 失败"
        minos="$(awk '/LC_BUILD_VERSION/ { f = 1 } f && $1 == "minos" { print $2; exit }' "$work/otool.txt")"
        printf '%s\t%s\n' "$name" "${minos:-?}" >> "$work/minos.txt"
        echo "bundle 可用：${name}（arm64 单架构，minos ${minos:-?}）"
    done 3<<EOF
$found
EOF

    # ── 暂存区（硬要求 5） ────────────────────────────────────────────────────
    local stage="$work/stage"
    mkdir "$stage"
    stage_bundles "$stage" "$found" || die "复制 bundle 进暂存区失败"
    stage_compliance "$stage" "$pf" || die "复制合规文件进暂存区失败"
    render_provenance > "$work/provenance.md" || die "渲染逐文件来源表失败"
    stage_notices "$stage" "$work/provenance.md"
    stage_install_scripts "$stage" || die "复制安装 / 卸载脚本失败"
    if [ "$kit" -eq 1 ]; then stage_kit_tool "$stage" "$build_dir" || die "放 kit 工具失败"; fi

    # ── INSTALL.txt（硬要求 4） ───────────────────────────────────────────────
    # 由文档生成器渲染（mac 版），不在这里抄第二份；渲染后断言对应源码指引那几行与使用前必读的规则在。
    local vars
    vars="$(printf '{"VERSION":"%s","TAG":"%s","COMMIT":"%s","JUCE_VERSION":"%s","JUCE_COMMIT":"%s","ZIP":"%s"}' \
        "$version" "$source_ref" "$source_commit" "$juce_version" "$juce_commit" "$zip_name")"
    render_install_txt "$vars" > "$stage/INSTALL.txt" || die "渲染 mac 版 INSTALL.txt 失败"
    local install_text
    install_text="$(cat -- "$stage/INSTALL.txt")"
    gate "INSTALL.txt 的判据没过" install_source_problems "$install_text" "$version" "$source_url" "$source_commit" "$juce_version" "$juce_commit" "$kit"
    gate "INSTALL.txt 缺使用前必读的规则" install_rule_problems "$install_text" "$(mf_all "$pf" ruleEn; mf_all "$pf" ruleZh)"

    # ── SHA256SUMS、权限与时间戳（硬要求 6） ─────────────────────────────────
    write_sha256sums "$stage" "$work/SHA256SUMS" || die "写 SHA256SUMS 失败"
    cp "$work/SHA256SUMS" "$stage/SHA256SUMS" || die "放 SHA256SUMS 失败"
    local epoch
    epoch="$(stage_epoch "$REPO_ROOT")"
    normalize_stage "$stage" "$epoch" || die "规整暂存区的权限与时间戳失败"

    # ── 压缩 ──────────────────────────────────────────────────────────────────
    local zip_w="$work/$zip_name"
    pack_zip "$stage" "$zip_w" || die "ditto 压缩失败"

    # ── 打包后断言（硬要求 6） ───────────────────────────────────────────────
    local entries info roots forbidden
    entries="$("$(unzip_cmd)" -Z1 "$zip_w")" || die "读不出 zip 的条目清单"
    forbidden="$(forbidden_entry_lines "$entries")"
    [ -z "$forbidden" ] || die "zip 里有不该发的条目：
$forbidden"
    forbidden="$(probe_entry_lines "$entries")"
    [ -z "$forbidden" ] || die "zip 里出现了探针条目：
$forbidden"
    forbidden="$(third_party_stray_lines "$entries")"
    [ -z "$forbidden" ] || die "zip 里有 third_party/ 下 notices/ 以外的条目：
$forbidden"

    roots="$(zip_root_entries "$entries")"
    gate "zip 的根条目与打包清单不一致" manifest_root_error "$manifest" "$roots"

    # 合规文件与 NOTICES 点名的每个声明文件路径都在 zip 里（区分大小写：路径要在解压目录里原样可查）。
    for rel in LICENSE THIRD-PARTY-NOTICES.md INSTALL.txt SHA256SUMS install-macos.sh uninstall-macos.sh; do
        list_has "$entries" "$rel" || die "zip 里缺 $rel"
    done
    while IFS= read -r rel <&3; do
        list_has "$entries" "$rel" || die "zip 里缺 '$rel'（package.ps1 -Preflight 点名的）"
    done 3< <({ mf_all "$pf" license; mf_all "$pf" notice; mf_all "$pf" cited; } | LC_ALL=C sort -u)

    # 可执行位（zip 元数据）：Contents/MacOS/ 下的文件条目恰好 4 个，全带 x 位。
    info="$("$(unzip_cmd)" -Z "$zip_w")" || die "读不出 zip 的权限信息"
    gate "zip 里的 Contents/MacOS 条目不合格" zip_exec_bits_error "$info" 4
    if [ "$kit" -eq 1 ]; then gate "kit 的工具条目不合格" zip_kit_tool_error "$info"; fi

    # 真解压一遍（ditto -x -k，与 INSTALL.txt 教用户的同一条命令），与暂存区逐文件比对：
    # 文件集合相同、每个文件字节相同、符号链接指向相同；解出来的可执行文件仍可执行、仍是 arm64。
    local unpack="$work/unpack"
    mkdir "$unpack"
    "$(ditto_cmd)" -x -k "$zip_w" "$unpack" || die "ditto 解压失败"
    (cd "$stage" && find . \( -type f -o -type l \) -print | LC_ALL=C sort) > "$work/stage-files.txt"
    (cd "$unpack" && find . \( -type f -o -type l \) -print | LC_ALL=C sort) > "$work/unpack-files.txt"
    if ! cmp -s "$work/stage-files.txt" "$work/unpack-files.txt"; then
        diff "$work/stage-files.txt" "$work/unpack-files.txt" >&2 || true
        die "从 zip 解出来的文件与暂存区不一样"
    fi
    while IFS= read -r rel <&3; do
        if [ -L "$stage/$rel" ]; then
            { [ -L "$unpack/$rel" ] && [ "$(readlink "$stage/$rel")" = "$(readlink "$unpack/$rel")" ]; } \
                || die "符号链接 '$rel' 没扛过 zip"
        else
            cmp -s "$stage/$rel" "$unpack/$rel" || die "'$rel' 从 zip 解出来的字节与暂存区不一样"
        fi
    done 3< "$work/stage-files.txt"
    local u
    while IFS= read -r name <&3; do
        stem="${name%.*}"
        u="$unpack/$name/Contents/MacOS/$stem"
        { [ -f "$u" ] && [ -x "$u" ]; } || die "解出来的 '$u' 没有可执行位"
        [ "$("$(lipo_cmd)" -archs "$u")" = "arm64" ] || die "解出来的 '$u' 不是 arm64 单架构"
    done 3<<EOF
$EXPECTED_BUNDLES
EOF

    # 重打包之后再验一次 ad-hoc 签名：构建 job 的 sign-and-verify 只验过构建树里的 bundle，复制、规整权限与时间、
    # 重压之后封印有没有被破坏，要对着最终 zip 解出来的 bundle 再验一次。
    while IFS= read -r name <&3; do
        "$(codesign_cmd)" --verify --deep --strict "$unpack/$name" > "$work/codesign.out" 2>&1 \
            || { cat -- "$work/codesign.out" >&2; die "'$name' 从 zip 解出来之后 codesign --verify 没过（重打包可能破坏了签名封印）"; }
    done 3<<EOF
$EXPECTED_BUNDLES
EOF

    # ── .sha256 与 summary（硬要求 3） ───────────────────────────────────────
    local hash size
    hash="$("$(shasum_cmd)" -a 256 "$zip_w" | awk '{ print $1 }')"
    size="$(wc -c < "$zip_w" | tr -d '[:space:]')"
    printf '%s  %s\n' "$hash" "$zip_name" > "$work/$sha_name"
    # .sha256 必须是 LF 行尾、全文没有 CR、哈希是 64 位小写十六进制。
    if file_has_cr "$work/$sha_name"; then
        die ".sha256 里有 CR（必须是 LF 行尾）"
    fi
    case "$hash" in
        *[!0-9a-f]*) die ".sha256 的哈希不是小写十六进制" ;;
    esac
    [ "${#hash}" -eq 64 ] || die ".sha256 的哈希不是 64 位"

    local n_entries minos_list bundle_list
    n_entries="$(grep -c . "$work/unpack-files.txt" || true)"
    bundle_list="$(printf '%s\n' "$EXPECTED_BUNDLES" | paste -sd, - | sed 's/,/, /g')"
    minos_list="$(cut -f2 "$work/minos.txt" | LC_ALL=C sort -u | paste -sd, - | sed 's/,/, /g')"
    {
        echo "# Rinne package summary (macOS arm64)"
        echo ""
        echo "| key | value |"
        echo "| --- | --- |"
        echo "| version | $version |"
        echo "| tag | $(if [ "$kit" -eq 1 ]; then echo 'n/a (kit build: no tag, source pinned to the commit)'; else echo "$tag"; fi) |"
        echo "| sourceCommit | $source_commit |"
        echo "| cmakeVersion | $mf_cmake |"
        echo "| juceVersion | $juce_version |"
        echo "| juceCommit | $juce_commit |"
        echo "| zipFileName | $zip_name |"
        echo "| sizeBytes | $size |"
        echo "| sha256 | $hash |"
        echo "| bundles | $bundle_list |"
        echo "| architectures | arm64 only (every Mach-O in every bundle checked with lipo -archs) |"
        echo "| minimumMacOS | $minos_list (LC_BUILD_VERSION minos of the bundle executables) |"
        echo "| kit | $(if [ "$kit" -eq 1 ]; then echo 'yes (tools/rinne_nulltest included)'; else echo 'no'; fi) |"
        echo "| licenseTexts | $(mf_all "$pf" license | grep -c . || true) files under LICENSES/ and third_party/notices/ |"
        echo "| citedNoticePaths | $(mf_all "$pf" cited | paste -sd, - | sed 's/,/, /g') |"
        echo "| buildRun | $(if [ -n "$build_run" ]; then echo "$build_run"; else echo 'n/a'; fi) |"
        echo "| validation | $(if [ -n "$validation" ]; then echo "$validation"; else echo 'n/a'; fi) |"
        echo ""
        echo "Corresponding source: $source_url"
        echo "JUCE ${juce_version}: https://github.com/juce-framework/JUCE/tree/${juce_commit}"
        echo "Build steps: docs/BUILDING.md at the source above"
        echo ""
        echo "## zip contents ($n_entries files)"
        echo ""
        echo "| entry | sha256 |"
        echo "| --- | --- |"
        while IFS= read -r rel <&3; do
            printf '| `%s` | %s |\n' "${rel#./}" "$("$(shasum_cmd)" -a 256 "$unpack/$rel" | awk '{ print $1 }')"
        done 3< "$work/unpack-files.txt"
    } > "$work/$summary_name"

    mv -f "$zip_w" "$out_real/$zip_name" || die "挪不动 zip 到输出目录"
    mv -f "$work/$sha_name" "$out_real/$sha_name" || die "挪不动 .sha256 到输出目录"
    mv -f "$work/$summary_name" "$out_real/$summary_name" || die "挪不动 summary 到输出目录"
    # 用 INSTALL.txt 教用户的那条命令自验（在输出目录里、对着挪过去的那一对文件）。
    (cd "$out_real" && "$(shasum_cmd)" -a 256 -c "$sha_name") || die "shasum -a 256 -c $sha_name 在 $out_real 里没过"

    if [ -n "${GITHUB_OUTPUT:-}" ]; then
        {
            echo "zip=$out_real/$zip_name"
            echo "sha256=$hash"
            echo "summary=$out_real/$summary_name"
        } >> "$GITHUB_OUTPUT"
    fi

    echo "$PROG: 好了"
    echo "  zip      $out_real/${zip_name}（$size 字节，$n_entries 个文件）"
    echo "  sha256   $hash"
    echo "  summary  $out_real/$summary_name"
    return 0
}

# ── 自测 ──────────────────────────────────────────────────────────────────────
# 每个判据都有该绿的正例和该红的坏例；删掉任何一条判据（或把它从主流程里摘掉），至少有一个用例变红。
# 不依赖 mac 硬件：ditto / lipo / file / otool / unzip / strings / pwsh / node 在流水线用例里全部换成桩
# （写在临时目录的 bin/ 里，只放在被测子进程的 PATH 前面），流水线用例把本脚本复制进一个假仓库里整条跑通。
# 有 pwsh（和钉版 Node）的机器上，另外对着真的 scripts/package.ps1 与文档生成器核一遍；没有就跳过并写明。
# 全部在 mktemp -d 建的临时目录里做；绝不对真实的 ~/Library 或仓库做删除测试，
# 「拒绝删除」的用例只做纯函数断言；临时目录删之前核对真实路径在临时根下一层、名字带本进程前缀。

ST_TOTAL=0
ST_BAD=0
ST_WORK=""
ST_C40="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
ST_JUCE40="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
ST_ENV=""
ST_EXPECT=""
ST_SELF=""
ST_REAL_PWSH=""
ST_FS_CHMOD=1

st_pass() { ST_TOTAL=$((ST_TOTAL + 1)); echo "PASS  $1"; }
st_fail() { ST_TOTAL=$((ST_TOTAL + 1)); ST_BAD=$((ST_BAD + 1)); echo "FAIL  $1"; }
st_skip() { echo "SKIP  $1"; }
# 该绿：命令在子 shell 里跑（函数里的 die / exit 只结束子 shell）。
st_ok() {
    local d="$1"
    shift
    if ( "$@" ) >/dev/null 2>&1; then st_pass "$d"; else st_fail "$d"; fi
}
# 该红。
st_red() {
    local d="$1"
    shift
    if ( "$@" ) >/dev/null 2>&1; then st_fail "${d}（本该红却绿了）"; else st_pass "$d"; fi
}
st_eq() { # 描述 实际 期望
    if [ "$2" = "$3" ]; then st_pass "$1"; else st_fail "$1（期望 '$3'，实际 '$2'）"; fi
}
st_contains() { # 描述 全文 片段
    if [ -z "$3" ]; then
        st_fail "$1（要找的片段是空的，用例本身没意义）"
        return
    fi
    case "$2" in *"$3"*) st_pass "$1" ;; *) st_fail "$1（找不到 '$3'）" ;; esac
}

st_mtime() { if stat --version >/dev/null 2>&1; then stat -c %Y -- "$1"; else stat -f %m -- "$1"; fi; }
st_mode() { ls -ld -- "$1" | awk '{ print substr($1, 1, 10) }'; }

# 目录的指纹：每个条目的权限串、mtime、内容哈希、路径。两个暂存区指纹相同 = 打出来的包字节相同。
st_fingerprint() { # $1 目录
    local f
    (
        cd -- "$1" && find . -print | LC_ALL=C sort | while IFS= read -r f; do
            if [ -d "$f" ]; then
                printf '%s %s %s\n' "$(st_mode "$f")" "$(st_mtime "$f")" "$f"
            else
                printf '%s %s %s %s\n' "$(st_mode "$f")" "$(st_mtime "$f")" "$("$(shasum_cmd)" -a 256 "$f" | awk '{ print $1 }')" "$f"
            fi
        done
    )
}

# 一个够用的暂存区（bundle 里一个可执行文件，合规文件若干）。
st_make_stage() { # $1 目录
    local d="$1"
    mkdir -p "$d/Rinne Input.vst3/Contents/MacOS" "$d/LICENSES" "$d/third_party/notices"
    printf '#!MACHO arm64\nexe\n' > "$d/Rinne Input.vst3/Contents/MacOS/Rinne Input"
    chmod 0755 "$d/Rinne Input.vst3/Contents/MacOS/Rinne Input"
    printf 'plist\n' > "$d/Rinne Input.vst3/Contents/Info.plist"
    printf 'license\n' > "$d/LICENSE"
    printf 'text\n' > "$d/LICENSES/GPL-3.0-or-later.txt"
    printf 'note\n' > "$d/third_party/notices/x.txt"
    printf 'notices\n' > "$d/THIRD-PARTY-NOTICES.md"
    printf 'install\n' > "$d/INSTALL.txt"
    printf '#!/bin/bash\n' > "$d/install-macos.sh"
    chmod 0755 "$d/install-macos.sh"
    printf 'sums\n' > "$d/SHA256SUMS"
}

# 一个合法的 preflight 清单。
st_make_pf() { # $1 文件
    {
        printf 'format\trinne-package-preflight/1\n'
        printf 'version\t0.9.0\n'
        printf 'tag\tv0.9.0\n'
        printf 'cmakeVersion\t0.9.0\n'
        printf 'sourceCommit\t%s\n' "$ST_C40"
        printf 'sourceUrl\thttps://example.invalid/rinne/tree/v0.9.0\n'
        printf 'sourceLine\tRinne  0.9.0: https://example.invalid/rinne/tree/v0.9.0  (commit %s)\n' "$ST_C40"
        printf 'sourceLine\tBuild steps: docs/BUILDING.md at the tag above\n'
        printf 'juceVersion\t8.0.8\n'
        printf 'juceCommit\t%s\n' "$ST_JUCE40"
        printf 'zipName\tRinne-v0.9.0-win64.zip\n'
        printf 'shaName\tRinne-v0.9.0-win64.zip.sha256\n'
        printf 'license\tLICENSES/GPL-3.0-or-later.txt\n'
        printf 'license\tthird_party/notices/x.txt\n'
        printf 'notice\tthird_party/notices/x.txt\n'
        printf 'cited\tthird_party/notices/x.txt\n'
        printf 'ruleEn\tPlacement: Put Rinne Input first\n'
        printf 'ruleZh\t放置位置：先放 Input\n'
        printf 'manifest\tINSTALL.txt\n'
    } > "$1"
}

# 一个假 bundle：Contents/{Info.plist,MacOS/<名字>,Resources/x.txt}，主程序第一行是桩认的 Mach-O 标记。
st_make_bundle() { # $1 父目录 $2 bundle 名 $3 主程序第一行之后的内容
    local stem="${2%.*}"
    mkdir -p "$1/$2/Contents/MacOS" "$1/$2/Contents/Resources"
    printf '#!MACHO arm64\n%s\n' "${3:-body-of-$2}" > "$1/$2/Contents/MacOS/$stem"
    chmod 0755 "$1/$2/Contents/MacOS/$stem"
    printf '<plist/>\n' > "$1/$2/Contents/Info.plist"
    printf 'resource\n' > "$1/$2/Contents/Resources/x.txt"
}

# 一份假构建目录：四个 bundle，_deps 里还有一个该被跳过的 bundle，tools-bin 里有 kit 工具。
st_make_build() { # $1 目录
    local b
    mkdir -p "$1"
    while IFS= read -r b; do
        [ -n "$b" ] || continue
        st_make_bundle "$1" "$b"
    done <<EOF
$EXPECTED_BUNDLES
EOF
    mkdir -p "$1/_deps/foo/Stray.vst3/Contents" "$1/tools-bin/Release"
    printf '#!/bin/sh\n' > "$1/tools-bin/Release/rinne_nulltest"
    chmod 0755 "$1/tools-bin/Release/rinne_nulltest"
}

# 桩：写进 $1/bin。
st_write_stubs() { # $1 临时根
    local bin="$1/bin"
    mkdir -p "$bin" "$1/scratch"
    cat > "$bin/ditto" <<'STUB'
#!/bin/bash
# 自测用的 ditto 桩：-c -k 打 tar（条目顺序固定），-x -k 解 tar，其余当复制。
mode=copy
pos1=""
pos2=""
for a in "$@"; do
    case "$a" in
        -c) mode=pack ;;
        -x) mode=unpack ;;
        -k | --norsrc | --noextattr | --noacl | --keepParent) ;;
        -*) ;;
        *) if [ -z "$pos1" ]; then pos1="$a"; else pos2="$a"; fi ;;
    esac
done
[ -n "$pos1" ] && [ -n "$pos2" ] || exit 2
case "$mode" in
    pack)
        export COPYFILE_DISABLE=1
        # 像真的 ditto 一样：目录有自己的条目（带尾斜杠），文件和符号链接各一条。
        (cd "$pos1" && { find . ! -name . -type d -print | sed 's|^\./||; s|$|/|'; find . \( -type f -o -type l \) -print | sed 's|^\./||'; } \
            | LC_ALL=C sort | tar --no-recursion -cf "$pos2" -T /dev/stdin) || exit 1
        ;;
    unpack)
        mkdir -p "$pos2" && tar -xf "$pos1" -C "$pos2"
        ;;
    *)
        if [ -d "$pos1" ]; then
            mkdir -p "$pos2" && cp -R "$pos1/." "$pos2/"
        else
            mkdir -p "$(dirname "$pos2")" && cp "$pos1" "$pos2"
        fi
        ;;
esac
STUB
    cat > "$bin/file" <<'STUB'
#!/bin/bash
f="$1"
[ "$1" = "-b" ] && f="$2"
first="$(head -n 1 -- "$f" 2>/dev/null)"
case "$first" in
    "#!MACHO"*) echo "Mach-O 64-bit bundle arm64" ;;
    *) echo "ASCII text" ;;
esac
STUB
    cat > "$bin/lipo" <<'STUB'
#!/bin/bash
[ "$1" = "-archs" ] || exit 2
first="$(head -n 1 -- "$2" 2>/dev/null)"
case "$first" in
    "#!MACHO"*) printf '%s\n' "${first#"#!MACHO"}" | sed 's/^ *//' ;;
    *) echo "lipo: not a Mach-O file" >&2; exit 1 ;;
esac
STUB
    cat > "$bin/otool" <<'STUB'
#!/bin/bash
printf 'Load command 9\n      cmd LC_BUILD_VERSION\n  cmdsize 32\n platform 1\n    minos 11.0\n'
STUB
    cat > "$bin/strings" <<'STUB'
#!/bin/bash
[ "$1" = "-a" ] && shift
cat -- "$1"
STUB
    cat > "$bin/unzip" <<'STUB'
#!/bin/bash
case "$1" in
    -Z1) tar -tf "$2" ;;
    -Z)
        # RINNE_STUB_STRIPX：模拟「压缩时丢了可执行位」——读回来的权限串里 x 全变成 -。
        if [ -n "${RINNE_STUB_STRIPX:-}" ]; then
            tar -tvf "$2" | sed -E 's/^([-l])(.)(.)x(.)(.)x(.)(.)x/\1\2\3-\4\5-\6\7-/'
        elif [ -n "${RINNE_STUB_STRIPTOOLX:-}" ]; then
            # 只丢 tools/rinne_nulltest 的可执行位（4 个 bundle 条目照旧），盯住 kit 工具那一道判据。
            tar -tvf "$2" | sed -E '/ tools\/rinne_nulltest$/ s/^([-l])(.)(.)x(.)(.)x(.)(.)x/\1\2\3-\4\5-\6\7-/'
        else
            tar -tvf "$2"
        fi
        ;;
    *) exit 2 ;;
esac
STUB
    cat > "$bin/pwsh" <<'STUB'
#!/bin/bash
# 自测用的 pwsh 桩：照 package.ps1 -Preflight -PreflightOut 的契约写一份机读清单。
[ -z "${RINNE_STUB_PWSH_FAIL:-}" ] || { echo "stub pwsh: forced failure" >&2; exit 1; }
out=""
man=""
ver=""
com=""
while [ $# -gt 0 ]; do
    case "$1" in
        -PreflightOut) out="$2"; shift 2 ;;
        -Manifest) man="$2"; shift 2 ;;
        -Version) ver="$2"; shift 2 ;;
        -SourceCommit) com="$2"; shift 2 ;;
        *) shift ;;
    esac
done
repo="${RINNE_STUB_REPO:?}"
cmake="${RINNE_STUB_CMAKE_VERSION:-$(sed -nE 's/^project\(Rinne VERSION ([0-9.]+).*/\1/p' "$repo/CMakeLists.txt")}"
{
    if [ -n "${RINNE_STUB_PWSH_BADFORMAT:-}" ]; then
        printf 'format\tsomething-else/9\n'
    else
        printf 'format\trinne-package-preflight/1\n'
    fi
    printf 'version\t%s\ntag\tv%s\ncmakeVersion\t%s\nsourceCommit\t%s\n' "$ver" "$ver" "$cmake" "$com"
    printf 'sourceUrl\thttps://example.invalid/rinne/tree/v%s\n' "$ver"
    printf 'sourceLine\tBuild steps: docs/BUILDING.md at the tag above\n'
    printf 'juceVersion\t8.0.8\njuceCommit\tbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n'
    printf 'zipName\tRinne-v%s-win64.zip\nshaName\tRinne-v%s-win64.zip.sha256\n' "$ver" "$ver"
    (cd "$repo" && find LICENSES third_party/notices -type f | LC_ALL=C sort) | while IFS= read -r p; do printf 'license\t%s\n' "$p"; done
    (cd "$repo" && find third_party/notices -type f | LC_ALL=C sort) | while IFS= read -r p; do printf 'notice\t%s\n' "$p"; done
    printf 'cited\tthird_party/notices/x.txt\n'
    printf 'ruleEn\tPlacement: Put Rinne Input first\nruleZh\t放置位置：先放 Input\n'
    grep -vE '^[[:space:]]*(#|$)' "$repo/$man" | while IFS= read -r p; do printf 'manifest\t%s\n' "$p"; done
} > "$out"
STUB
    cat > "$bin/node" <<'STUB'
#!/bin/bash
# 自测用的 node 桩：只认 --version 与 gen-docs.mjs 的两个 --emit。
if [ "${1:-}" = "--version" ]; then echo "v22.99.0"; exit 0; fi
case "$*" in
    *"--emit provenance"*)
        printf '| a | b |\n| --- | --- |\n| `x` | y |\n'
        ;;
    *"--emit install-txt"*)
        json=""
        while [ $# -gt 0 ]; do
            [ "$1" = "--vars" ] && json="$2"
            shift
        done
        g() { printf '%s' "$json" | sed -n 's/.*"'"$1"'":"\([^"]*\)".*/\1/p'; }
        printf 'Rinne %s - macOS arm64\n\n' "$(g VERSION)"
        read_block() {
            printf 'READ THIS FIRST / 使用前必读\n\n'
            [ -n "${RINNE_STUB_NODE_DROPRULES:-}" ] || printf 'Placement: Put Rinne Input first\n放置位置：先放 Input\n'
        }
        [ -z "${RINNE_STUB_NODE_READFIRST:-}" ] || { read_block; printf '\n'; }
        printf 'Corresponding Source for this build (GPLv3 section 6 (d)):\n'
        printf '  Rinne  %s: https://example.invalid/rinne/tree/%s  (commit %s)\n' "$(g VERSION)" "$(g TAG)" "$(g COMMIT)"
        printf '  JUCE   %s (stub): https://example.invalid/juce/tree/%s\n' "$(g JUCE_VERSION)" "$(g JUCE_COMMIT)"
        [ -n "${RINNE_STUB_NODE_DROP:-}" ] || printf '  Build steps: docs/BUILDING.md at the tag above\n'
        [ -n "${RINNE_STUB_NODE_READFIRST:-}" ] || { printf '\n'; read_block; }
        ;;
    *) exit 2 ;;
esac
STUB
    cat > "$bin/codesign" <<'STUB'
#!/bin/bash
# 自测用的 codesign 桩：每次调用记一行日志；RINNE_STUB_CODESIGN_FAIL 非空时验签判红。
printf '%s\n' "$*" >> "${RINNE_STUB_SCRATCH:-/tmp}/codesign.log"
[ -z "${RINNE_STUB_CODESIGN_FAIL:-}" ] || { echo "stub codesign: invalid signature" >&2; exit 1; }
STUB
    chmod 0755 "$bin/ditto" "$bin/file" "$bin/lipo" "$bin/otool" "$bin/strings" "$bin/unzip" "$bin/pwsh" "$bin/node" "$bin/codesign"
}

# 假仓库：把本脚本、两个安装脚本和真的打包清单复制进去，再写最小的 CMakeLists / 合规文件 / NOTICES。
st_make_fake_repo() { # $1 目录
    local r="$1"
    mkdir -p "$r/scripts" "$r/packaging/macos" "$r/LICENSES" "$r/third_party/notices" "$ST_WORK/home"
    cp "$ST_SELF" "$r/scripts/package-macos.sh"
    cp "$REPO_ROOT/packaging/macos/install-macos.sh" "$r/packaging/macos/install-macos.sh"
    cp "$REPO_ROOT/packaging/macos/uninstall-macos.sh" "$r/packaging/macos/uninstall-macos.sh"
    cp "$REPO_ROOT/$MANIFEST_FILE" "$r/$MANIFEST_FILE"
    printf 'cmake_minimum_required(VERSION 3.22)\nproject(Rinne VERSION 0.9.0 LANGUAGES CXX)\n' > "$r/CMakeLists.txt"
    printf '22.23.3\n' > "$r/.node-version"
    printf 'license text\n' > "$r/LICENSE"
    printf 'gpl text\n' > "$r/LICENSES/GPL-3.0-or-later.txt"
    printf 'note x\n' > "$r/third_party/notices/x.txt"
    printf '# Notices\n\n<!-- section:scvb-derived -->\nderived text\n<!-- /section:scvb-derived -->\n' > "$r/THIRD-PARTY-NOTICES.md"
}

# 在假仓库里跑一次本脚本。$1 输出目录，其余是传给脚本的参数。ST_ENV 里放这一次要加的环境变量。
st_run_pkg() {
    local out="$1"
    shift
    mkdir -p "$out"
    (
        cd -- "$ST_WORK/fake" && env PATH="$ST_WORK/bin:$PATH" RINNE_NODE="$ST_WORK/bin/node" \
            RINNE_STUB_REPO="$ST_WORK/fake" RINNE_STUB_SCRATCH="$ST_WORK/scratch" SOURCE_DATE_EPOCH=1700000000 \
            HOME="$ST_WORK/home" TMPDIR="$ST_WORK" $ST_ENV \
            bash "$ST_WORK/fake/scripts/package-macos.sh" --out-dir "$out" --source-commit "$ST_C40" "$@"
    ) > "$ST_WORK/last.log" 2>&1
}

# 往假仓库里那份脚本的副本里注入一行（在整行逐字相等的锚点之前或之后）。锚点必须恰好命中一次，否则注入失败、用例本身没意义。
# 用于「脚本内部出错必须让整个脚本非 0」的反例：被测的是出错之后的退出行为，不是注入点本身。
st_inject() { # $1 锚点（整行逐字） $2 要插入的行 $3 before|after
    local f="$ST_WORK/fake/scripts/package-macos.sh" tmp="$ST_WORK/inject.tmp"
    if awk -v anchor="$1" -v line="$2" -v where="$3" '
        $0 == anchor { n++; if (where == "before") print line; print; if (where == "after") print line; next }
        { print }
        END { exit(n == 1 ? 0 : 3) }' "$f" > "$tmp"; then
        cp "$tmp" "$f"
    else
        return 1
    fi
}
# 把假仓库里的副本恢复成正在跑的这一份。
st_inject_reset() { cp "$ST_SELF" "$ST_WORK/fake/scripts/package-macos.sh"; }

# 该红的整条流水线用例：跑出非 0，且输出目录里没有 zip（旧的也不许留着冒充）。
# ST_EXPECT 非空时，日志里还必须有这句话：红得有理由，不能因为别的原因碰巧红了。
st_red_run() { # 描述 输出目录 zip 名 参数…
    local d="$1" out="$2" zipn="$3" want="$ST_EXPECT"
    ST_EXPECT=""
    shift 3
    if st_run_pkg "$out" "$@"; then
        st_fail "${d}（本该红却绿了）"
    elif [ -e "$out/$zipn" ]; then
        st_fail "${d}（红了，但输出目录里留下了 zip）"
    elif [ -n "$want" ] && ! grep -qF -- "$want" "$ST_WORK/last.log"; then
        st_fail "${d}（红了，但不是预期的原因：日志里没有 '$want'）"
        sed 's/^/        | /' "$ST_WORK/last.log"
    else
        st_pass "$d"
    fi
}

# 第 1–14 组：纯函数与临时目录里的小用例（不起子进程流水线）。
st_units() {
    local work="$ST_WORK" real_pwsh="$ST_REAL_PWSH" fs_chmod="$ST_FS_CHMOD"
    local rel d i k

    # ── 1. 版本回落：与 package.ps1 同一口径，两个解析器不一致必须红 ──────────
    local cm="$work/cm"
    mkdir -p "$cm"
    st_cm() { printf '%b' "$2" > "$cm/$1.txt"; }
    st_cm ok 'cmake_minimum_required(VERSION 3.22)\nproject(Rinne VERSION 0.9.0 LANGUAGES CXX)\n'
    st_cm commented '# project(Rinne VERSION 9.9.9)\nproject(Rinne VERSION 0.9.0)\n'
    st_cm minonly 'cmake_minimum_required(VERSION 3.22)\n'
    st_cm other 'project(Other VERSION 1.2.3)\n'
    st_cm rinnex 'project(RinneX VERSION 1.2.3)\n'
    st_cm myver 'project(Rinne MYVERSION 1.2.3)\n'
    st_cm twopart 'project(Rinne VERSION 0.9)\n'
    st_cm anyorder 'project(Rinne LANGUAGES CXX VERSION 1.2.3)\n'
    st_cm multiline 'project(Rinne\n  VERSION 1.2.3\n  LANGUAGES CXX)\n'
    st_cm indented '  project( Rinne VERSION 2.0.1 )\n'
    st_cm outside 'project(Rinne LANGUAGES CXX)\nset(VERSION 5.5.5)\n'
    st_cm prerelease 'project(Rinne VERSION 1.2.3-rc.1)\n'
    st_cm empty ''
    for k in "ok=0.9.0" "commented=0.9.0" "minonly=" "other=" "rinnex=" "myver=" "twopart=" "anyorder=1.2.3" \
        "multiline=1.2.3" "indented=2.0.1" "outside=" "prerelease=1.2.3" "empty="; do
        st_eq "CMake 版本解析 ${k%%=*}" "$(read_cmake_version "$cm/${k%%=*}.txt")" "${k#*=}"
    done
    for k in "0.9.0" "0.9.0-rc.1" "10.20.30" "1.2.3-rc.12"; do
        st_ok "版本形态：$k 合法" version_shape_problem "$k"
    done
    for k in "" "0.9" "v0.9.0" "../x" "0.9.0 " " 0.9.0" "0.9.0-rc." "0.9.0-" "0.9.0+build" "0.9.0
1.0.0" "0.9.0/x"; do
        st_red "版本形态：'$(printf '%s' "$k" | tr '\n' '|')' 不合法" version_shape_problem "$k"
    done
    st_ok "版本一致：两个解析器读到同一个版本" version_pair_error "0.9.0" "0.9.0"
    st_red "版本不一致：本脚本与 package.ps1 读到的不同" version_pair_error "0.9.0" "0.9.1"
    st_red "版本不一致：本脚本读不到版本" version_pair_error "" "0.9.0"
    st_red "版本不一致：package.ps1 的清单里没有版本" version_pair_error "0.9.0" ""
    if [ -n "$real_pwsh" ]; then
        cat > "$work/agree.ps1" <<'PS'
param([string]$Lib, [string]$Dir)
. $Lib
foreach ($f in (Get-ChildItem -LiteralPath $Dir -Filter '*.txt' | Sort-Object Name)) {
    $v = Get-RinneCMakeVersion ([IO.File]::ReadAllText($f.FullName))
    [Console]::Out.Write($f.BaseName + [string][char]9 + [string]$v + [string][char]10)
}
PS
        "$real_pwsh" -NoProfile -File "$(native_path "$work/agree.ps1")" \
            -Lib "$(native_path "$REPO_ROOT/scripts/lib/release-version.ps1")" -Dir "$(native_path "$cm")" > "$work/agree.tsv" 2>/dev/null
        for k in ok commented minonly other rinnex myver twopart anyorder multiline indented outside prerelease empty; do
            st_eq "两个解析器一致（${k}）：sh 与 release-version.ps1" \
                "$(read_cmake_version "$cm/$k.txt")" "$(awk -F'\t' -v n="$k" '$1 == n { print $2 }' "$work/agree.tsv")"
        done
    else
        st_skip "两个解析器逐夹具比对：没有 pwsh"
    fi

    # ── 2. preflight 清单契约 ──────────────────────────────────────────────────
    local pf="$work/pf.tsv" pfx="$work/pf-x.tsv"
    st_make_pf "$pf"
    st_ok "preflight 清单：合法的清单 0 条问题" preflight_problems "$pf"
    { printf 'format\trinne-package-preflight/2\n'; sed -n '2,$p' "$pf"; } > "$pfx"
    st_red "preflight 清单：format 版本不对" preflight_problems "$pfx"
    { sed -n '2,$p' "$pf"; sed -n '1p' "$pf"; } > "$pfx"
    st_red "preflight 清单：首行不是 format" preflight_problems "$pfx"
    for k in $REQUIRED_PF_KEYS; do
        awk -F'\t' -v k="$k" '$1 != k' "$pf" > "$pfx"
        st_red "preflight 清单：缺必需的键 $k" preflight_problems "$pfx"
    done
    awk -F'\t' -v OFS='\t' '$1 == "version" { $0 = $0 "\textra" } { print }' "$pf" > "$pfx"
    st_red "preflight 清单：值里有 TAB" preflight_problems "$pfx"
    awk '{ if ($1 == "tag") printf "%s\r\n", $0; else print }' "$pf" > "$pfx"
    st_red "preflight 清单：行尾是 CRLF" preflight_problems "$pfx"
    { cat "$pf"; printf 'sourceLine\tfoo\nbar\n'; } > "$pfx"
    st_red "preflight 清单：值里有 LF（拆成了没有 TAB 的一行）" preflight_problems "$pfx"
    awk -F'\t' -v OFS='\t' '$1 == "version" { $0 = "version\t" } { print }' "$pf" > "$pfx"
    st_red "preflight 清单：值是空的" preflight_problems "$pfx"
    { cat "$pf"; printf 'bad key\tx\n'; } > "$pfx"
    st_red "preflight 清单：键不合法" preflight_problems "$pfx"
    : > "$pfx"
    st_red "preflight 清单：空文件" preflight_problems "$pfx"
    st_red "preflight 清单：文件不存在" preflight_problems "$work/no-such-pf.tsv"
    st_eq "preflight 清单取值：同名键按顺序累积" "$(mf_all "$pf" license | sed -n 2p)" "third_party/notices/x.txt"
    st_eq "preflight 清单取值：唯一键" "$(mf_one "$pf" version)" "0.9.0"
    st_red "preflight 清单取值：不是恰好一行的键" mf_one "$pf" sourceLine

    # ── 3. bundle 集合：恰好 2 个 .vst3 + 2 个 .component，名字逐字相符 ────────
    local good_names="Rinne Input.vst3
Rinne Output.vst3
Rinne Input.component
Rinne Output.component"
    st_ok "bundle 集合：恰好四个且名字相符" bundle_set_error "$good_names" "$EXPECTED_BUNDLES"
    st_ok "bundle 集合：顺序不同也行" bundle_set_error "Rinne Output.component
Rinne Input.vst3
Rinne Input.component
Rinne Output.vst3" "$EXPECTED_BUNDLES"
    st_red "bundle 集合：多一个探针" bundle_set_error "$good_names
Rinne Probe Input.vst3" "$EXPECTED_BUNDLES"
    st_contains "bundle 集合：探针的报错点名探针" "$(bundle_set_error "$good_names
Rinne Probe Input.vst3" "$EXPECTED_BUNDLES")" "探针"
    st_red "bundle 集合：多一个别的 .vst3" bundle_set_error "$good_names
Rinne Monitor.vst3" "$EXPECTED_BUNDLES"
    st_red "bundle 集合：少一个 .component" bundle_set_error "Rinne Input.vst3
Rinne Output.vst3
Rinne Input.component" "$EXPECTED_BUNDLES"
    st_red "bundle 集合：混进别的身份" bundle_set_error "Rinne Input.vst3
Rinne Output.vst3
Rinne Input.component
Other Output.component" "$EXPECTED_BUNDLES"
    st_red "bundle 集合：大小写不对" bundle_set_error "rinne input.vst3
Rinne Output.vst3
Rinne Input.component
Rinne Output.component" "$EXPECTED_BUNDLES"
    st_red "bundle 集合：两个同名、缺另一个" bundle_set_error "Rinne Input.vst3
Rinne Input.vst3
Rinne Input.component
Rinne Output.component" "$EXPECTED_BUNDLES"
    st_red "bundle 集合：扩展名不对" bundle_set_error "Rinne Input.vst3
Rinne Output.vst3
Rinne Input.component
Rinne Output.appex" "$EXPECTED_BUNDLES"
    st_red "bundle 集合：一个都没有" bundle_set_error "" "$EXPECTED_BUNDLES"

    # ── 4. 架构：每个 Mach-O 只有 arm64 一个架构 ──────────────────────────────
    st_ok "架构：arm64" macho_arch_error "arm64"
    st_ok "架构：lipo 输出带换行" macho_arch_error "arm64
"
    st_red "架构：胖二进制 arm64 + x86_64" macho_arch_error "arm64 x86_64"
    st_red "架构：只有 x86_64" macho_arch_error "x86_64"
    st_red "架构：读不出" macho_arch_error ""
    st_red "架构：arm64e 也不算" macho_arch_error "arm64e"
    local ar="$work/arch"
    st_make_bundle "$ar/ok" "Rinne Input.vst3"
    mkdir -p "$ar/ok/Rinne Input.vst3/Contents/Frameworks"
    printf '#!MACHO arm64\ndylib\n' > "$ar/ok/Rinne Input.vst3/Contents/Frameworks/a.dylib"
    st_ok "bundle 架构：主程序与内嵌 dylib 都是 arm64" bundle_arch_problems "$ar/ok/Rinne Input.vst3"
    st_make_bundle "$ar/fat" "Rinne Input.vst3"
    printf '#!MACHO arm64 x86_64\nfat\n' > "$ar/fat/Rinne Input.vst3/Contents/MacOS/Rinne Input"
    st_red "bundle 架构：主程序是胖二进制" bundle_arch_problems "$ar/fat/Rinne Input.vst3"
    st_make_bundle "$ar/fatlib" "Rinne Input.vst3"
    mkdir -p "$ar/fatlib/Rinne Input.vst3/Contents/Frameworks"
    printf '#!MACHO arm64 x86_64\nfat\n' > "$ar/fatlib/Rinne Input.vst3/Contents/Frameworks/a.dylib"
    st_red "bundle 架构：内嵌 dylib 是胖二进制" bundle_arch_problems "$ar/fatlib/Rinne Input.vst3"
    st_make_bundle "$ar/x86" "Rinne Input.vst3"
    printf '#!MACHO x86_64\nx\n' > "$ar/x86/Rinne Input.vst3/Contents/MacOS/Rinne Input"
    st_red "bundle 架构：主程序只有 x86_64" bundle_arch_problems "$ar/x86/Rinne Input.vst3"
    st_make_bundle "$ar/none" "Rinne Input.vst3"
    printf 'just a script\n' > "$ar/none/Rinne Input.vst3/Contents/MacOS/Rinne Input"
    st_red "bundle 架构：bundle 里一个 Mach-O 都没有" bundle_arch_problems "$ar/none/Rinne Input.vst3"

    # ── 5. 可执行位：unzip -Z 读回，4 个 Contents/MacOS 条目全带 x 位 ─────────
    local zi_ok zi_nox zi_3 zi_5 zi_gx zi_link
    zi_ok="Archive:  Rinne-v0.9.0-macos-arm64.zip
drwxr-xr-x  2.1 unx        0 bx stor 23-Nov-14 22:13 Rinne Input.vst3/Contents/MacOS/
-rwxr-xr-x  2.1 unx     1234 bx defN 23-Nov-14 22:13 Rinne Input.vst3/Contents/MacOS/Rinne Input
-rw-r--r--  2.1 unx       12 tx defN 23-Nov-14 22:13 Rinne Input.vst3/Contents/Info.plist
-rwxr-xr-x  2.1 unx     1234 bx defN 23-Nov-14 22:13 Rinne Output.vst3/Contents/MacOS/Rinne Output
-rwxr-xr-x  2.1 unx     1234 bx defN 23-Nov-14 22:13 Rinne Input.component/Contents/MacOS/Rinne Input
-rwxr-xr-x  2.1 unx     1234 bx defN 23-Nov-14 22:13 Rinne Output.component/Contents/MacOS/Rinne Output
-rw-r--r--  2.1 unx       12 tx defN 23-Nov-14 22:13 install-macos.sh
10 files, 5000 bytes uncompressed, 2000 bytes compressed:  60.0%"
    st_ok "可执行位：4 个条目都带 x 位" zip_exec_bits_error "$zi_ok" 4
    zi_nox="$(printf '%s\n' "$zi_ok" | sed 's|^-rwxr-xr-x\(.*Rinne Output.component/Contents/MacOS/Rinne Output\)$|-rw-r--r--\1|')"
    st_red "可执行位：一个条目丢了 x 位" zip_exec_bits_error "$zi_nox" 4
    zi_gx="$(printf '%s\n' "$zi_ok" | sed 's|^-rwxr-xr-x\(.*Rinne Output.vst3/Contents/MacOS/Rinne Output\)$|-rwx------\1|')"
    st_red "可执行位：属组和其他人丢了 x 位" zip_exec_bits_error "$zi_gx" 4
    zi_3="$(printf '%s\n' "$zi_ok" | grep -v 'Rinne Output.component/Contents/MacOS/')"
    st_red "可执行位：只有 3 个条目" zip_exec_bits_error "$zi_3" 4
    zi_5="$zi_ok
-rwxr-xr-x  2.1 unx     1234 bx defN 23-Nov-14 22:13 Rinne Input.vst3/Contents/MacOS/helper"
    st_red "可执行位：多出第 5 个条目" zip_exec_bits_error "$zi_5" 4
    zi_link="$(printf '%s\n' "$zi_ok" | sed 's|^-rwxr-xr-x\(.*Rinne Input.component/Contents/MacOS/Rinne Input\)$|lrwxr-xr-x\1|')"
    st_ok "可执行位：符号链接条目（l 开头）也算" zip_exec_bits_error "$zi_link" 4

    # kit 的 tools/rinne_nulltest 要带 x 位。
    local zk_ok
    zk_ok="$zi_ok
-rwxr-xr-x  2.1 unx     1234 bx defN 23-Nov-14 22:13 tools/rinne_nulltest"
    st_ok "kit 工具：带 x 位" zip_kit_tool_error "$zk_ok"
    st_red "kit 工具：丢了 x 位" zip_kit_tool_error "$(printf '%s\n' "$zk_ok" | sed 's|^-rwxr-xr-x\(.*tools/rinne_nulltest\)$|-rw-r--r--\1|')"
    st_red "kit 工具：zip 里没有这个条目" zip_kit_tool_error "$zi_ok"

    # ── 6. 禁入条目与 third_party ─────────────────────────────────────────────
    st_eq "禁入条目：干净的清单没有命中" "$(forbidden_entry_lines "INSTALL.txt
Rinne Input.vst3/Contents/MacOS/Rinne Input
Rinne Input.vst3/Contents/Resources/a..b
x__MACOSX/readme
dir/.DS_Store.txt
LICENSES/MIT.txt")" ""
    for k in '__MACOSX/a' 'Rinne Input.vst3/__MACOSX/._x' '.DS_Store' 'Rinne Input.vst3/Contents/.DS_Store' \
        '/etc/passwd' '../x' 'a/../b' 'a/..'; do
        st_red "禁入条目：$k" test -z "$(forbidden_entry_lines "INSTALL.txt
$k")"
    done
    st_eq "third_party：只有 notices/ 没有越界" "$(third_party_stray_lines "third_party/notices/a.txt
third_party/notices/sub/b.txt
LICENSE")" ""
    st_eq "third_party：ditto 给目录写的 third_party/ 与 third_party/notices/ 条目不算越界" "$(third_party_stray_lines "third_party/
third_party/notices/
third_party/notices/sub/
third_party/notices/sub/b.txt")" ""
    st_red "third_party：别的目录条目 third_party/other/ 算越界" test -z "$(third_party_stray_lines "third_party/
third_party/other/")"
    st_red "third_party：notices/ 以外的子目录" test -z "$(third_party_stray_lines "third_party/other/a.txt")"
    st_red "third_party：直接放在 third_party/ 下的文件" test -z "$(third_party_stray_lines "third_party/a.txt")"
    st_ok "探针条目：干净" test -z "$(probe_entry_lines "INSTALL.txt
LICENSES/MIT.txt")"
    st_red "探针条目：zip 里出现 Rinne Probe" test -z "$(probe_entry_lines "INSTALL.txt
Rinne Probe Input.vst3/Contents/MacOS/Rinne Probe Input")"

    # ── 7. 根条目与打包清单（集合相等 + 数量相等） ────────────────────────────
    local man="$REPO_ROOT/$MANIFEST_FILE" manx="$work/manifest-x.txt" ze roots
    ze="Rinne Input.vst3/Contents/MacOS/Rinne Input
Rinne Output.vst3/Contents/Info.plist
Rinne Input.component/Contents/Info.plist
Rinne Output.component/Contents/MacOS/Rinne Output
Rinne Input.vst3/
INSTALL.txt
LICENSE
LICENSES/GPL-3.0-or-later.txt
LICENSES/MIT.txt
SHA256SUMS
THIRD-PARTY-NOTICES.md
install-macos.sh
third_party/notices/x.txt
third_party/notices/y.txt
uninstall-macos.sh"
    roots="$(zip_root_entries "$ze")"
    st_ok "根条目：与真的打包清单一致" manifest_root_error "$man" "$roots"
    st_red "根条目：zip 多出探针" manifest_root_error "$man" "$(zip_root_entries "$ze
Rinne Probe Input.vst3/Contents/x")"
    st_red "根条目：zip 多出一个顶层文件" manifest_root_error "$man" "$(zip_root_entries "$ze
README.txt")"
    st_red "根条目：zip 多出一个未登记的目录" manifest_root_error "$man" "$(zip_root_entries "$ze
foo/bar.txt")"
    st_red "根条目：zip 里少了 INSTALL.txt" manifest_root_error "$man" "$(zip_root_entries "$(printf '%s\n' "$ze" | grep -v '^INSTALL.txt$')")"
    st_red "根条目：zip 里少了一个 bundle" manifest_root_error "$man" "$(zip_root_entries "$(printf '%s\n' "$ze" | grep -v 'Output.component')")"
    { cat "$man"; printf 'extra.txt\n'; } > "$manx"
    st_red "打包清单多一项" manifest_root_error "$manx" "$roots"
    grep -v '^uninstall-macos.sh$' "$man" > "$manx"
    st_red "打包清单少一项" manifest_root_error "$manx" "$roots"
    { cat "$man"; printf 'tools/\n'; } > "$manx"
    st_red "kit 才有的 tools/：发行包的清单里没有，zip 里有就红" manifest_root_error "$man" "$(zip_root_entries "$ze
tools/rinne_nulltest")"
    st_ok "kit 才有的 tools/：kit 的清单里有它" manifest_root_error "$manx" "$(zip_root_entries "$ze
tools/rinne_nulltest")"
    st_eq "根条目：third_party 归到 third_party/notices/" "$(zip_root_entries "third_party/notices/x.txt")" "third_party/notices/"

    # ── 8. 本机路径扫描 ───────────────────────────────────────────────────────
    local ps="$work/pathscan"
    mkdir -p "$ps"
    # 夹具里的用户目录与盘符路径拼着写，免得公开文本检查把测试夹具当成真路径。
    local ub="/Use""rs" wb="C:""\\Use""rs"
    printf 'hello\n/usr/lib/libSystem.B.dylib\n/System/Library/Frameworks/AppKit.framework\n%s/Shared/\n' "$ub" > "$ps/clean.txt"
    printf 'built from %s/ci/work/rinne/build/x.cpp\n' "$ub" > "$ps/lit.txt"
    printf 'path %s/someone/Library/x\n' "$ub" > "$ps/home.txt"
    printf 'path %s\\someone\\build\\a.cpp\n' "$wb" > "$ps/drive.txt"
    st_eq "路径扫描：干净的文件没有命中" "$(local_path_hits "$ps/clean.txt" "$ub/ci/work/rinne/build" "$ub/ci")" ""
    st_red "路径扫描：命中给定的字面量" test -z "$(local_path_hits "$ps/lit.txt" "$ub/ci/work/rinne/build")"
    st_red "路径扫描：命中用户主目录形态" test -z "$(local_path_hits "$ps/home.txt")"
    st_red "路径扫描：命中盘符路径" test -z "$(local_path_hits "$ps/drive.txt")"
    st_eq "路径扫描：太短的字面量（如 /）不当成命中" "$(local_path_hits "$ps/clean.txt" "/")" ""
    st_make_bundle "$ps/ok" "Rinne Input.vst3"
    st_ok "bundle 路径扫描：干净的 bundle" bundle_path_problems "$ps/ok/Rinne Input.vst3" "$work/strings.txt" "$work/build-here"
    st_make_bundle "$ps/dirty" "Rinne Input.vst3" "compiled in $work/build-here/src/main.cpp"
    st_red "bundle 路径扫描：二进制里有构建目录" bundle_path_problems "$ps/dirty/Rinne Input.vst3" "$work/strings.txt" "$work/build-here"
    st_make_bundle "$ps/plist" "Rinne Input.vst3"
    printf '%s\n' "$work/build-here/in-a-resource" > "$ps/plist/Rinne Input.vst3/Contents/Resources/x.txt"
    st_ok "bundle 路径扫描：只扫 Mach-O，资源文件不算" bundle_path_problems "$ps/plist/Rinne Input.vst3" "$work/strings.txt" "$work/build-here"

    # ── 9. INSTALL.txt 的对应源码指引与使用前必读 ─────────────────────────────
    local it
    it="Rinne 0.9.0 - macOS arm64

Corresponding Source for this build (GPLv3 section 6 (d)):
  Rinne  0.9.0: https://example.invalid/rinne/tree/v0.9.0  (commit $ST_C40)
  JUCE   8.0.8 (bundles the SDKs): https://example.invalid/juce/tree/$ST_JUCE40
  Build steps: docs/BUILDING.md at the tag above

READ THIS FIRST / 使用前必读

Placement: Put Rinne Input first"
    st_ok "INSTALL.txt：对应源码指引齐全" install_source_problems "$it" 0.9.0 https://example.invalid/rinne/tree/v0.9.0 "$ST_C40" 8.0.8 "$ST_JUCE40"
    st_red "INSTALL.txt：缺 Rinne 版本行" install_source_problems "$(printf '%s\n' "$it" | grep -v '^  Rinne  ')" 0.9.0 https://example.invalid/rinne/tree/v0.9.0 "$ST_C40" 8.0.8 "$ST_JUCE40"
    st_red "INSTALL.txt：Rinne 行对应的链接不对" install_source_problems "$it" 0.9.0 https://example.invalid/rinne/tree/v0.9.1 "$ST_C40" 8.0.8 "$ST_JUCE40"
    st_red "INSTALL.txt：源码 commit 不对" install_source_problems "$it" 0.9.0 https://example.invalid/rinne/tree/v0.9.0 "cccccccccccccccccccccccccccccccccccccccc" 8.0.8 "$ST_JUCE40"
    st_red "INSTALL.txt：缺 JUCE 版本行" install_source_problems "$(printf '%s\n' "$it" | grep -v '^  JUCE  ')" 0.9.0 https://example.invalid/rinne/tree/v0.9.0 "$ST_C40" 8.0.8 "$ST_JUCE40"
    st_red "INSTALL.txt：JUCE commit 不对" install_source_problems "$it" 0.9.0 https://example.invalid/rinne/tree/v0.9.0 "$ST_C40" 8.0.8 "dddddddddddddddddddddddddddddddddddddddd"
    st_red "INSTALL.txt：缺构建说明路径" install_source_problems "$(printf '%s\n' "$it" | grep -v 'Build steps')" 0.9.0 https://example.invalid/rinne/tree/v0.9.0 "$ST_C40" 8.0.8 "$ST_JUCE40"
    st_red "INSTALL.txt：还有未替换的占位符" install_source_problems "$it
{{VERSION}}" 0.9.0 https://example.invalid/rinne/tree/v0.9.0 "$ST_C40" 8.0.8 "$ST_JUCE40"
    st_red "INSTALL.txt：JUCE 版本不对" install_source_problems "$it" 0.9.0 https://example.invalid/rinne/tree/v0.9.0 "$ST_C40" 8.0.7 "$ST_JUCE40"
    # 位置：源码块要在标题之后的第一段，各行都在 READ THIS FIRST 之前。
    local it_head it_block it_read
    it_head="Rinne 0.9.0 - macOS arm64"
    it_block="$(printf '%s\n' "$it" | sed -n '/^Corresponding Source/,/Build steps/p')"
    it_read="$(printf '%s\n' "$it" | sed -n '/^READ THIS FIRST/,$p')"
    st_ok "INSTALL.txt：拆开再拼回去的样例没有问题（拼法没有写错）" install_source_problems "$it_head

$it_block

$it_read" 0.9.0 https://example.invalid/rinne/tree/v0.9.0 "$ST_C40" 8.0.8 "$ST_JUCE40"
    st_red "INSTALL.txt：源码块被挪到 READ THIS FIRST 之后" install_source_problems "$it_head

$it_read

$it_block" 0.9.0 https://example.invalid/rinne/tree/v0.9.0 "$ST_C40" 8.0.8 "$ST_JUCE40"
    st_red "INSTALL.txt：标题之后先来了别的段落，源码块不是第一段" install_source_problems "$it_head

Some other paragraph first.

$it_block

$it_read" 0.9.0 https://example.invalid/rinne/tree/v0.9.0 "$ST_C40" 8.0.8 "$ST_JUCE40"
    st_red "INSTALL.txt：没有 READ THIS FIRST 段" install_source_problems "$it_head

$it_block" 0.9.0 https://example.invalid/rinne/tree/v0.9.0 "$ST_C40" 8.0.8 "$ST_JUCE40"
    # 位置判据单独盯一例：第一段确实是源码块，只有构建步骤那一行落在 READ THIS FIRST 之后。
    st_red "INSTALL.txt：构建步骤一行落在 READ THIS FIRST 之后（其余都对）" install_source_problems "$it_head

$(printf '%s\n' "$it_block" | grep -v 'Build steps')

$it_read
  Build steps: docs/BUILDING.md at the tag above" 0.9.0 https://example.invalid/rinne/tree/v0.9.0 "$ST_C40" 8.0.8 "$ST_JUCE40"
    # kit 没有 tag：链接钉在 commit 上，正文里不许有 tree/v<版本> 的死链。
    local itk
    itk="$(printf '%s\n' "$it" | sed "s|tree/v0.9.0|tree/$ST_C40|")"
    st_ok "INSTALL.txt：kit 的链接钉在 commit 上" install_source_problems "$itk" 0.9.0 "https://example.invalid/rinne/tree/$ST_C40" "$ST_C40" 8.0.8 "$ST_JUCE40" 1
    st_red "INSTALL.txt：kit 却还用 tag 链接" install_source_problems "$it" 0.9.0 https://example.invalid/rinne/tree/v0.9.0 "$ST_C40" 8.0.8 "$ST_JUCE40" 1
    # 链接不是 commit，但正文里没有 tree/v<版本> 的死链：只剩“钉在 commit 上”这一条判据在红。
    st_red "INSTALL.txt：kit 的源码链接不是 commit（正文里没有死链）" install_source_problems "$(printf '%s\n' "$it" | sed 's|tree/v0.9.0|tree/release-x|')" 0.9.0 https://example.invalid/rinne/tree/release-x "$ST_C40" 8.0.8 "$ST_JUCE40" 1
    st_red "INSTALL.txt：kit 的正文里有 tree/v<版本> 的死链" install_source_problems "$itk
see https://example.invalid/rinne/tree/v0.9.0" 0.9.0 "https://example.invalid/rinne/tree/$ST_C40" "$ST_C40" 8.0.8 "$ST_JUCE40" 1
    st_eq "kit 的源码链接：把 tag 换成 40 位 commit" "$(kit_source_url https://example.invalid/rinne/tree/v0.9.0 "$ST_C40")" "https://example.invalid/rinne/tree/$ST_C40"
    st_ok "INSTALL.txt：使用前必读的规则在" install_rule_problems "$it" "Placement: Put Rinne Input first"
    st_red "INSTALL.txt：缺一条使用前必读的规则" install_rule_problems "$it" "Placement: Put Rinne Input first
Start from 0: A new instance starts at 0"

    # ── 10. NOTICES 里插来源表 ────────────────────────────────────────────────
    local nt="$work/notices.md" tb="$work/table.md" got want_txt
    printf '# Notices\n\nintro\n\n<!-- section:scvb-derived -->\nText here.   \n\n\n<!-- /section:scvb-derived -->\n\n<!-- section:other -->\nkeep\n<!-- /section:other -->\n' > "$nt"
    printf '| a | b |\n| - | - |\n\n\n' > "$tb"
    got="$(provenance_inject "$nt" "$tb"; printf x)"
    got="${got%x}"
    want_txt="$(printf '# Notices\n\nintro\n\n<!-- section:scvb-derived -->\nText here.\n\n| a | b |\n| - | - |\n\n<!-- /section:scvb-derived -->\n\n<!-- section:other -->\nkeep\n<!-- /section:other -->\nx')"
    want_txt="${want_txt%x}"
    st_eq "来源表：插在结束标记之前，标记之后的内容原样" "$got" "$want_txt"
    printf '# Notices\n\nno sections\n' > "$work/n-none.md"
    st_red "来源表：NOTICES 里没有标记" provenance_inject "$work/n-none.md" "$tb"
    printf '<!-- section:scvb-derived -->\nonly open\n' > "$work/n-open.md"
    st_red "来源表：只有开始标记" provenance_inject "$work/n-open.md" "$tb"
    printf '<!-- /section:scvb-derived -->\nx\n<!-- section:scvb-derived -->\n' > "$work/n-rev.md"
    st_red "来源表：结束标记在开始标记之前" provenance_inject "$work/n-rev.md" "$tb"
    printf '<!-- /section:scvb-derived -->\n<!-- section:scvb-derived -->\nbody\n<!-- /section:scvb-derived -->\n' > "$work/n-rev2.md"
    st_red "来源表：结束标记先出现一次、开始标记在它之后" provenance_inject "$work/n-rev2.md" "$tb"
    printf '\n\n' > "$work/table-empty.md"
    st_red "来源表：表是空的" provenance_inject "$nt" "$work/table-empty.md"

    # ── 11. 确定性：规整暂存区 + 压缩 ─────────────────────────────────────────
    local epoch=1700000000 sa="$work/stage-a" sb="$work/stage-b" sc="$work/stage-c"
    ( umask 022; st_make_stage "$sa" )
    ( umask 077; st_make_stage "$sb"; find "$sb" -exec touch -h -t 200101010101 {} + )
    cp -R "$sb" "$sc"
    st_eq "确定性：规整前，不同 umask 与时间的两份暂存区指纹不同" \
        "$([ "$(st_fingerprint "$sa")" = "$(st_fingerprint "$sb")" ] && echo same || echo different)" different
    normalize_stage "$sa" "$epoch"
    normalize_stage "$sb" "$epoch"
    st_eq "确定性：规整后，两份暂存区指纹相同" \
        "$([ "$(st_fingerprint "$sa")" = "$(st_fingerprint "$sb")" ] && echo same || echo different)" same
    st_eq "规整：可执行文件 0755" "$(st_mode "$sa/Rinne Input.vst3/Contents/MacOS/Rinne Input")" "-rwxr-xr-x"
    st_eq "规整：bundle 里的普通文件 0644" "$(st_mode "$sa/Rinne Input.vst3/Contents/Info.plist")" "-rw-r--r--"
    st_eq "规整：合规文件 0644" "$(st_mode "$sa/LICENSES/GPL-3.0-or-later.txt")" "-rw-r--r--"
    if [ "$fs_chmod" -eq 1 ]; then
        st_eq "规整：安装脚本不论原权限一律 0644" "$(st_mode "$sa/install-macos.sh")" "-rw-r--r--"
    else
        st_skip "规整：安装脚本一律 0644：这个文件系统去不掉可执行位"
    fi
    st_eq "规整：目录 0755" "$(st_mode "$sa/LICENSES")" "drwxr-xr-x"
    local off=0 seen=0
    while IFS= read -r rel; do
        [ -n "$rel" ] || continue
        seen=$((seen + 1))
        [ "$(st_mtime "$sa/$rel")" = "$epoch" ] || off=$((off + 1))
    done <<EOF
$(cd "$sa" && find . ! -name . -print | sed 's|^\./||')
EOF
    st_eq "规整：每个条目的 mtime 都是 epoch" "$off" "0"
    st_eq "规整：确实枚举到了条目（不是空循环）" "$([ "$seen" -ge 10 ] && echo yes || echo no)" yes
    st_eq "时间戳：SOURCE_DATE_EPOCH 优先" "$(SOURCE_DATE_EPOCH=1234567890 stage_epoch "$work")" "1234567890"
    st_eq "时间戳：SOURCE_DATE_EPOCH 不是数字时落到 1980（非仓库目录里）" "$(SOURCE_DATE_EPOCH=abc stage_epoch "$work")" "315532800"
    st_eq "时间戳：SOURCE_DATE_EPOCH=0 抬到 1980 的下限（DOS 时间不能更早）" "$(SOURCE_DATE_EPOCH=0 stage_epoch "$work")" "315532800"
    st_eq "时间戳：低于下限的值抬到下限" "$(SOURCE_DATE_EPOCH=100000 stage_epoch "$work")" "315532800"
    st_eq "时间戳：奇数秒取整到偶数秒（与 package.ps1 同一口径）" "$(SOURCE_DATE_EPOCH=1234567891 stage_epoch "$work")" "1234567890"
    st_eq "时间戳：位数过多的当作无效" "$(SOURCE_DATE_EPOCH=99999999999999999999 stage_epoch "$work")" "315532800"
    pack_zip "$sa" "$work/a1.zip"
    pack_zip "$sa" "$work/a2.zip"
    pack_zip "$sb" "$work/b1.zip"
    local h_a1 h_a2 h_b1 h_c
    h_a1="$("$(shasum_cmd)" -a 256 "$work/a1.zip" | awk '{ print $1 }')"
    h_a2="$("$(shasum_cmd)" -a 256 "$work/a2.zip" | awk '{ print $1 }')"
    h_b1="$("$(shasum_cmd)" -a 256 "$work/b1.zip" | awk '{ print $1 }')"
    st_eq "确定性：同一份暂存区连打两次，sha256 相同" "$h_a1" "$h_a2"
    st_eq "确定性：内容相同、构建环境不同的暂存区，打出来 sha256 相同" "$h_a1" "$h_b1"
    printf 'changed\n' >> "$sa/LICENSES/GPL-3.0-or-later.txt"
    touch -h -t 202311142213.20 "$sa/LICENSES/GPL-3.0-or-later.txt"
    pack_zip "$sa" "$work/a3.zip"
    h_c="$("$(shasum_cmd)" -a 256 "$work/a3.zip" | awk '{ print $1 }')"
    st_eq "确定性：改一个文件后 sha256 不同（坏例）" "$([ "$h_c" != "$h_a1" ] && echo different || echo same)" different
    # 没有规整的暂存区（sc 还是 umask 077 + 旧时间）打出来的包与规整过的不同：规整这一步不是摆设。
    pack_zip "$sc" "$work/c1.zip"
    st_eq "确定性：没规整的暂存区打出来的包与规整过的不同（坏例）" \
        "$([ "$("$(shasum_cmd)" -a 256 "$work/c1.zip" | awk '{ print $1 }')" != "$h_a1" ] && echo different || echo same)" different

    # ── 12. SHA256SUMS 与 .sha256 的格式 ──────────────────────────────────────
    write_sha256sums "$sb" "$work/sums.txt"
    st_contains "SHA256SUMS：每行 <64 位 hex>␣␣<相对路径>，路径带空格也行" "$(cat "$work/sums.txt")" "  Rinne Input.vst3/Contents/MacOS/Rinne Input"
    st_eq "SHA256SUMS：不含 CR" "$(file_has_cr "$work/sums.txt" && echo yes || echo no)" no

    # ── 13. kit 名字 ──────────────────────────────────────────────────────────
    st_ok "kit 名字：合法" kit_name_problem "Rinne-kit-macos-arm64-0.9.0-preview.0-$ST_C40"
    st_red "kit 名字：空" kit_name_problem ""
    st_red "kit 名字：含路径分隔符" kit_name_problem "a/b"
    st_red "kit 名字：含反斜杠" kit_name_problem 'a\b'
    st_red "kit 名字：含空格" kit_name_problem "a b"
    st_red "kit 名字：含换行" kit_name_problem "a
b"
    st_red "kit 名字：以点开头" kit_name_problem ".hidden"
    st_red "kit 名字：两个点" kit_name_problem ".."

    # ── 13b. gate：判据函数的统一收口 ─────────────────────────────────────────
    t_quiet_ok() { return 0; }
    t_quiet_fail() { return 3; }
    t_loud() { echo "问题一条"; return 0; }
    st_ok "gate：判据函数无输出、退出码 0 → 通过" gate "x" t_quiet_ok
    st_red "gate：判据函数有输出（问题清单）→ 红" gate "x" t_loud
    st_red "gate：判据函数没输出但退出码非 0（中途意外退出）→ 红" gate "x" t_quiet_fail

    # ── 14. 输出目录与临时目录的删除纪律（只做纯函数断言 + 临时目录里的单文件） ─
    mkdir -p "$work/safe-out" "$work/gitdir/.git"
    st_ok "输出目录：普通目录放行" assert_safe_out_dir "$work/safe-out"
    st_red "输出目录：不存在" assert_safe_out_dir "$work/no-such-dir"
    st_red "输出目录：根目录" assert_safe_out_dir "/"
    st_red "输出目录：仓根" assert_safe_out_dir "$REPO_ROOT"
    st_red "输出目录：仓根的上级目录" assert_safe_out_dir "$(dirname -- "$REPO_ROOT")"
    ( HOME="$work/safe-out"; assert_safe_out_dir "$work/safe-out" ) >/dev/null 2>&1
    st_eq "输出目录：用户目录被拒" "$?" "1"
    st_red "输出目录：仓库目录（有 .git）" assert_safe_out_dir "$work/gitdir"
    mkdir -p "$work/rm-out"
    printf 'old\n' > "$work/rm-out/a.zip"
    printf 'keep\n' > "$work/rm-out/other.txt"
    printf 'outer\n' > "$work/outer.txt"
    remove_out_file "$work/rm-out" "a.zip"
    st_eq "删旧产物：按文件名删了输出目录里的那一个" "$([ -e "$work/rm-out/a.zip" ] && echo there || echo gone)" gone
    st_eq "删旧产物：别的文件原样留下" "$(cat "$work/rm-out/other.txt")" keep
    remove_out_file "$work/rm-out" "../outer.txt"
    st_eq "删旧产物：带路径分隔符的名字不动" "$([ -e "$work/outer.txt" ] && echo there || echo gone)" there
    remove_out_file "$work/rm-out" ""
    st_eq "删旧产物：空名字不动输出目录" "$([ -d "$work/rm-out" ] && echo there || echo gone)" there
    mkdir -p "$work/rm-out/sub"
    st_red "删旧产物：同名的是目录，拒绝（不删目录）" remove_out_file "$work/rm-out" "sub"
    st_eq "删旧产物：目录原样留下" "$([ -d "$work/rm-out/sub" ] && echo there || echo gone)" there
    st_ok "临时目录：本进程建的认得出" temp_dir_is_ours "$work"
    st_red "临时目录：临时根本身不是" temp_dir_is_ours "${TMPDIR:-/tmp}"
    st_red "临时目录：临时根下面的子目录不是（不在一层）" temp_dir_is_ours "$work/safe-out"
    mkdir -p "$work/other.123" "$work/$TEMP_PREFIX.abc" "$work/$TEMP_PREFIX.nested"
    st_red "临时目录：名字带前缀但不在临时根下一层，不认" temp_dir_is_ours "$work/$TEMP_PREFIX.nested"
    ( TMPDIR="$work"; temp_dir_is_ours "$work/other.123" ) >/dev/null 2>&1
    st_eq "临时目录：名字不带本进程前缀被拒" "$?" "1"
    ( TMPDIR="$work"; temp_dir_is_ours "$work/$TEMP_PREFIX.abc" ) >/dev/null 2>&1
    st_eq "临时目录：名字带本进程前缀且在一层下，认" "$?" "0"
    cleanup_temp "$work/safe-out" >/dev/null 2>&1
    st_eq "清理：不是本进程建的目录不删" "$([ -d "$work/safe-out" ] && echo there || echo gone)" there

}

# 第 15 组：把本脚本复制进假仓库，用桩整条跑通，含各种坏样例与 --kit 通路。
st_pipeline() {
    local work="$ST_WORK" fs_chmod="$ST_FS_CHMOD"
    local rel d i k

    # ── 15. 整条流水线（桩 + 假仓库） ─────────────────────────────────────────
    st_make_fake_repo "$work/fake"
    local b1="$work/b1" o1="$work/o1" zip0="Rinne-v0.9.0-macos-arm64.zip" sha1 sha2 o2 b2
    st_make_build "$b1"
    if st_run_pkg "$o1" --version 0.9.0 --build-dir "$b1"; then
        st_pass "流水线：干净的构建目录打包通过"
    else
        st_fail "流水线：干净的构建目录打包通过"
        sed 's/^/        | /' "$work/last.log"
    fi
    st_eq "流水线：产出 zip、.sha256、summary 三件" \
        "$(ls "$o1" | LC_ALL=C sort | paste -sd, -)" "$zip0,$zip0.sha256,package-summary-macos-arm64.md"
    sha1="$(cat "$o1/$zip0.sha256" 2>/dev/null)"
    st_eq "流水线：.sha256 是 <hex>␣␣<zip 名> + LF" \
        "$(printf '%s' "$sha1" | grep -cE "^[0-9a-f]{64}  $zip0\$")" "1"
    st_eq "流水线：.sha256 没有 CR" "$(file_has_cr "$o1/$zip0.sha256" && echo yes || echo no)" no
    st_contains "流水线：summary 里有 sha256" "$(cat "$o1/package-summary-macos-arm64.md")" "${sha1%% *}"
    st_contains "流水线：summary 带 JUCE 版本与 40 位 commit 的链接" "$(cat "$o1/package-summary-macos-arm64.md")" "JUCE 8.0.8: https://github.com/juce-framework/JUCE/tree/$ST_JUCE40"
    st_contains "流水线：summary 带构建步骤一行" "$(cat "$o1/package-summary-macos-arm64.md")" "Build steps: docs/BUILDING.md"
    st_eq "流水线：解出来的 4 个 bundle 都验过签（codesign --verify --deep --strict）" \
        "$(grep -c -- '--verify --deep --strict' "$work/scratch/codesign.log" 2>/dev/null || true)" "4"
    st_eq "流水线：zip 里恰好 4 个 Contents/MacOS 条目" "$(tar -tf "$o1/$zip0" | grep -c '/Contents/MacOS/[^/]')" "4"
    st_eq "流水线：_deps 里的 bundle 被跳过" "$(tar -tf "$o1/$zip0" | grep -c 'Stray')" "0"
    st_eq "流水线：发行包里没有 tools/" "$(tar -tf "$o1/$zip0" | grep -c '^tools/')" "0"
    st_eq "流水线：包里的安装脚本与仓里的逐字相同" \
        "$(tar -xOf "$o1/$zip0" install-macos.sh | cmp -s - "$REPO_ROOT/packaging/macos/install-macos.sh" && echo same || echo different)" same
    st_contains "流水线：包里的 NOTICES 带来源表" "$(tar -xOf "$o1/$zip0" THIRD-PARTY-NOTICES.md)" "| \`x\` | y |"
    st_contains "流水线：包里的 INSTALL.txt 带使用前必读" "$(tar -xOf "$o1/$zip0" INSTALL.txt)" "Placement: Put Rinne Input first"
    st_contains "流水线：SHA256SUMS 列了 INSTALL.txt" "$(tar -xOf "$o1/$zip0" SHA256SUMS)" "  INSTALL.txt"
    st_eq "流水线：SHA256SUMS 不含自己" "$(tar -xOf "$o1/$zip0" SHA256SUMS | grep -c '  SHA256SUMS$')" "0"
    # 确定性（整条流水线）：旧的文件时间、新的输出目录，sha256 不变。
    b2="$work/b2"
    o2="$work/o2"
    st_make_build "$b2"
    find "$b2" -exec touch -h -t 200001010101 {} +
    st_run_pkg "$o2" --version 0.9.0 --build-dir "$b2"
    sha2="$(cat "$o2/$zip0.sha256" 2>/dev/null)"
    st_eq "流水线：同样的内容、不同的文件时间与目录，两次打包 sha256 相同" "${sha2%% *}" "${sha1%% *}"
    printf 'one more byte\n' >> "$b2/Rinne Input.vst3/Contents/Resources/x.txt"
    st_run_pkg "$work/o3" --version 0.9.0 --build-dir "$b2"
    st_eq "流水线：bundle 里改一个文件，sha256 不同（坏例）" \
        "$([ "$(cut -d' ' -f1 "$work/o3/$zip0.sha256")" != "${sha1%% *}" ] && echo different || echo same)" different

    local n=0 bx
    nb() { n=$((n + 1)); bx="$work/bad$n"; st_make_build "$bx"; }
    nb
    printf '#!MACHO arm64 x86_64\nfat\n' > "$bx/Rinne Input.vst3/Contents/MacOS/Rinne Input"
    ST_EXPECT="架构不合格"
    st_red_run "流水线：胖二进制 → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    nb
    mkdir -p "$bx/Rinne Input.vst3/Contents/Frameworks"
    printf '#!MACHO arm64 x86_64\nfat\n' > "$bx/Rinne Input.vst3/Contents/Frameworks/a.dylib"
    ST_EXPECT="架构不合格"
    st_red_run "流水线：内嵌 dylib 是胖二进制（主程序是好的） → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    nb
    st_make_bundle "$bx" "Rinne Probe Input.vst3"
    ST_EXPECT="bundle 集合不对"
    st_red_run "流水线：构建目录里混进探针 bundle → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    nb
    # rinne-delete-ok: 自测在自己 mktemp -d 建的临时目录里造的假构建目录；路径由自测拼出，不是参数
    rm -rf "$bx/Rinne Output.component"
    ST_EXPECT="bundle 集合不对"
    st_red_run "流水线：少一个 bundle → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    nb
    printf '#!MACHO arm64\n%s/src/a.cpp\n' "$bx" > "$bx/Rinne Input.vst3/Contents/MacOS/Rinne Input"
    ST_EXPECT="本机路径"
    st_red_run "流水线：二进制里有构建目录路径 → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    nb
    printf 'junk\n' > "$bx/Rinne Input.vst3/Contents/Resources/.DS_Store"
    ST_EXPECT="不该发的条目"
    st_red_run "流水线：bundle 里有 .DS_Store → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    nb
    mkdir -p "$bx/Rinne Input.vst3/__MACOSX"
    printf 'junk\n' > "$bx/Rinne Input.vst3/__MACOSX/._x"
    ST_EXPECT="不该发的条目"
    st_red_run "流水线：bundle 里有 __MACOSX/ → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    nb
    chmod -x "$bx/Rinne Output.vst3/Contents/MacOS/Rinne Output"
    if [ "$fs_chmod" -eq 1 ]; then
        ST_EXPECT="没有可执行位"
        st_red_run "流水线：构建出来的主程序没有可执行位 → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    else
        st_skip "流水线：构建出来的主程序没有可执行位：这个文件系统去不掉可执行位"
    fi
    nb
    ST_ENV="RINNE_STUB_STRIPX=1"
    ST_EXPECT="Contents/MacOS 条目不合格"
    st_red_run "流水线：zip 里丢了可执行位 → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    ST_ENV=""
    nb
    ST_ENV="RINNE_STUB_CODESIGN_FAIL=1"
    ST_EXPECT="codesign --verify 没过"
    st_red_run "流水线：重打包后解出来的 bundle 验签不过 → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    ST_ENV=""
    nb
    ST_ENV="RINNE_STUB_NODE_READFIRST=1"
    ST_EXPECT="INSTALL.txt 的判据没过"
    st_red_run "流水线：INSTALL.txt 的对应源码块被挪到 READ THIS FIRST 之后 → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    ST_ENV=""
    nb
    cp "$work/fake/$MANIFEST_FILE" "$work/manifest.orig"
    printf 'extra.txt\n' >> "$work/fake/$MANIFEST_FILE"
    ST_EXPECT="根条目与打包清单不一致"
    st_red_run "流水线：打包清单多一项 → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    cp "$work/manifest.orig" "$work/fake/$MANIFEST_FILE"
    nb
    cp "$work/fake/THIRD-PARTY-NOTICES.md" "$work/notices.orig"
    printf '# no sections here\n' > "$work/fake/THIRD-PARTY-NOTICES.md"
    ST_EXPECT="来源表插不进去"
    st_red_run "流水线：NOTICES 里没有来源表小节 → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    cp "$work/notices.orig" "$work/fake/THIRD-PARTY-NOTICES.md"
    nb
    ST_ENV="RINNE_STUB_NODE_DROPRULES=1"
    ST_EXPECT="缺使用前必读"
    st_red_run "流水线：INSTALL.txt 缺使用前必读的规则 → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    nb
    ST_ENV="RINNE_STUB_NODE_DROP=1"
    ST_EXPECT="INSTALL.txt 的判据没过"
    st_red_run "流水线：INSTALL.txt 缺对应源码指引的一行 → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    ST_ENV="RINNE_STUB_PWSH_FAIL=1"
    ST_EXPECT="-Preflight 没过"
    st_red_run "流水线：package.ps1 -Preflight 没过 → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    ST_ENV="RINNE_STUB_PWSH_BADFORMAT=1"
    ST_EXPECT="preflight 清单不合契约"
    st_red_run "流水线：preflight 清单 format 不对 → 红" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    ST_ENV=""
    ST_EXPECT="给了空串"
    st_red_run "流水线：--version 给了空串 → 红（不回落）" "$work/ob$n" "$zip0" --version "" --build-dir "$bx"
    ST_EXPECT="版本号形态不对"
    st_red_run "流水线：--version 形态不对（含路径分隔符） → 红" "$work/ob$n" "x.zip" --version "../x" --build-dir "$bx"
    ST_ENV="RINNE_STUB_CMAKE_VERSION=9.9.9"
    ST_EXPECT="版本回落与 package.ps1 不一致"
    st_red_run "流水线：版本回落时两个解析器不一致 → 红" "$work/ob$n" "$zip0" --build-dir "$bx"
    ST_ENV=""
    if st_run_pkg "$work/ofb" --build-dir "$bx"; then
        st_eq "流水线：没传 --version 时从 CMakeLists.txt 回落" "$([ -e "$work/ofb/$zip0" ] && echo there || echo gone)" there
    else
        st_fail "流水线：没传 --version 时从 CMakeLists.txt 回落"
        sed 's/^/        | /' "$work/last.log"
    fi
    # 输出目录
    ST_EXPECT="输出目录不能是仓根"
    st_red_run "流水线：输出目录是仓根 → 红" "$work/fake" "package-summary-macos-arm64.md" --version 0.9.0 --build-dir "$bx"
    ST_EXPECT="输出目录不能是用户目录"
    st_red_run "流水线：输出目录是用户目录 → 红" "$work/home" "$zip0" --version 0.9.0 --build-dir "$bx"
    mkdir -p "$work/ostale"
    printf 'stale\n' > "$work/ostale/$zip0"
    printf 'keep\n' > "$work/ostale/unrelated.txt"
    printf '#!MACHO arm64 x86_64\nfat\n' > "$bx/Rinne Input.vst3/Contents/MacOS/Rinne Input"
    st_red_run "流水线：红了的这一次不留旧 zip 冒充（旧的被清掉）" "$work/ostale" "$zip0" --version 0.9.0 --build-dir "$bx"
    st_eq "流水线：输出目录里别的文件原样留下" "$(cat "$work/ostale/unrelated.txt")" keep
    st_run_pkg "$work/ofb" --dry-run --version 0.9.0 --build-dir "$work/no-build-dir-needed"
    st_contains "流水线：--dry-run 只打印计划" "$(cat "$work/last.log")" "[DryRun] 好了"
    # kit
    local kitn="Rinne-kit-macos-arm64-0.9.0-preview.0-$ST_C40" bk="$work/bkit"
    st_make_build "$bk"
    if st_run_pkg "$work/okit" --version 0.9.0 --build-dir "$bk" --kit --kit-name "$kitn"; then
        st_pass "kit：--kit --kit-name 通过"
        st_eq "kit：包里有 tools/rinne_nulltest" "$(tar -tf "$work/okit/$kitn.zip" | grep -c '^tools/rinne_nulltest$')" "1"
        st_contains "kit：INSTALL.txt 的对应源码链接钉在 40 位 commit 上" "$(tar -xOf "$work/okit/$kitn.zip" INSTALL.txt)" "https://example.invalid/rinne/tree/$ST_C40"
        st_eq "kit：INSTALL.txt 里没有 tree/v<版本> 的死链" "$(tar -xOf "$work/okit/$kitn.zip" INSTALL.txt | grep -c 'tree/v0.9.0' || true)" "0"
        st_contains "kit：summary 的 Corresponding source 钉在 commit 上" "$(cat "$work/okit/package-summary-$kitn.md")" "Corresponding source: https://example.invalid/rinne/tree/$ST_C40"
        st_eq "kit：summary 里没有 tree/v<版本> 的死链" "$(grep -c 'tree/v0.9.0' "$work/okit/package-summary-$kitn.md" || true)" "0"
        st_eq "kit：产出 kit 的 zip、.sha256 与带 kit 名字的 summary" \
            "$(ls "$work/okit" | LC_ALL=C sort | paste -sd, -)" "$kitn.zip,$kitn.zip.sha256,package-summary-$kitn.md"
    else
        st_fail "kit：--kit --kit-name 通过"
        sed 's/^/        | /' "$work/last.log"
    fi
    ST_ENV="RINNE_STUB_STRIPTOOLX=1"
    ST_EXPECT="kit 的工具条目不合格"
    st_red_run "kit：zip 里 tools/rinne_nulltest 丢了可执行位 → 红" "$work/okit6" "$kitn.zip" --version 0.9.0 --build-dir "$bk" --kit --kit-name "$kitn"
    ST_ENV=""
    ST_EXPECT="必须同时给 --kit-name"
    st_red_run "kit：--kit 没给 --kit-name → 红" "$work/okit2" "$zip0" --version 0.9.0 --build-dir "$bk" --kit
    ST_EXPECT="只能与 --kit 一起用"
    st_red_run "kit：--kit-name 没带 --kit → 红" "$work/okit3" "$kitn.zip" --version 0.9.0 --build-dir "$bk" --kit-name "$kitn"
    ST_EXPECT="--kit-name 里只许"
    st_red_run "kit：--kit-name 含路径分隔符 → 红" "$work/okit4" "x.zip" --version 0.9.0 --build-dir "$bk" --kit --kit-name "a/b"
    # rinne-delete-ok: 自测在自己 mktemp -d 建的临时目录里造的假构建目录；路径由自测拼出，不是参数
    rm -rf "$bk/tools-bin"
    ST_EXPECT="没有 rinne_nulltest"
    st_red_run "kit：构建目录里没有 rinne_nulltest → 红" "$work/okit5" "$kitn.zip" --version 0.9.0 --build-dir "$bk" --kit --kit-name "$kitn"

    # ── 脚本内部出错，整个脚本必须非 0，且输出目录里不留 zip ─────────────────────
    # macOS 自带的 bash 3.2 在 set -u 下遇到未定义的变量会直接结束整个脚本；装了 EXIT trap 时退出码常常是 0
    # （CI 里「Package kit」就这样绿着、上传步骤才报没有文件）。往副本里注入一个未定义变量的展开，看整条流水线的退出行为：
    #   a. 注入在 bundle 检查函数里（经 gate 的命令替换跑，只结束那个子 shell，gate 看到非 0 后判红）；
    #   b. 注入在 main 的主流程里（CI 里红过的形状：直接结束整个脚本）。b 在 bash 3.2 上靠 EXIT trap 里的「没走完」判据拦住；
    #      bash 4 以上本来就是 1，所以这条要在 mac（或别的 3.2）上才真起作用，本机 bash 5 上只证明「注入确实让它红了」；
    #   c. 注入在自测自己里：自测半路出错也必须非 0，不能静默结束还报通过。
    local injected_line='    : "$rinne_test_unset_var"'
    nb
    if st_inject '    local bundle="$1" f archs n=0 macho=0' "$injected_line" after; then
        ST_EXPECT="架构不合格"
        st_red_run "出错：bundle 检查函数里变量展开出错 → 整个脚本非 0，不留 zip" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    else
        st_fail "出错：bundle 检查函数里变量展开出错 → 整个脚本非 0（注入点没找到，用例失效）"
    fi
    st_inject_reset
    nb
    if st_inject '    local stage="$work/stage"' "$injected_line" before; then
        ST_EXPECT="unbound variable"
        st_red_run "出错：主流程里变量展开出错 → 整个脚本非 0，不留 zip" "$work/ob$n" "$zip0" --version 0.9.0 --build-dir "$bx"
    else
        st_fail "出错：主流程里变量展开出错 → 整个脚本非 0（注入点没找到，用例失效）"
    fi
    st_inject_reset
    # 正例：注入还原之后同一份构建目录照样打得出来；--help 正常返回也是 0（「走完了」的标记不能把正常的提前返回拦成失败）。
    if st_run_pkg "$work/ob-reset" --version 0.9.0 --build-dir "$b1"; then
        st_eq "出错：注入还原后恢复正常打包" "$([ -e "$work/ob-reset/$zip0" ] && echo there || echo gone)" there
    else
        st_fail "出错：注入还原后恢复正常打包"
        sed 's/^/        | /' "$work/last.log"
    fi
    if st_run_pkg "$work/ob-help" --help; then
        st_contains "出错：--help 提前返回仍然是 0" "$(cat "$work/last.log")" "用法：package-macos.sh"
    else
        st_fail "出错：--help 提前返回仍然是 0"
        sed 's/^/        | /' "$work/last.log"
    fi
    if st_inject 'st_eq() { # 描述 实际 期望' "$injected_line" after; then
        if (cd -- "$ST_WORK/fake" && bash "$ST_WORK/fake/scripts/package-macos.sh" --self-test units) > "$work/self-inject.log" 2>&1; then
            st_fail "出错：自测半路变量展开出错 → 自测整体非 0（本该红却绿了）"
        elif grep -q "package-macos 自测通过" "$work/self-inject.log"; then
            st_fail "出错：自测半路变量展开出错 → 自测整体非 0（红了，但还打印了「自测通过」）"
        else
            st_pass "出错：自测半路变量展开出错 → 自测整体非 0，也没打印「自测通过」"
        fi
    else
        st_fail "出错：自测半路变量展开出错 → 自测整体非 0（注入点没找到，用例失效）"
    fi
    st_inject_reset

}

# 第 16 组：对着真的 package.ps1 与文档生成器核一遍（有 pwsh 才做）。
st_real() {
    local work="$ST_WORK" real_pwsh="$ST_REAL_PWSH"
    local rel

    # ── 16. 对着真的 package.ps1 与文档生成器核一遍（有 pwsh 才做） ───────────
    if [ -n "$real_pwsh" ]; then
        local rpf="$work/real-pf.tsv"
        if "$real_pwsh" -NoProfile -File "$(native_path "$REPO_ROOT/scripts/package.ps1")" -Preflight \
            -PreflightOut "$(native_path "$rpf")" -Manifest "$MANIFEST_FILE" >"$work/real-pf.log" 2>&1; then
            st_pass "真 package.ps1 -Preflight 通过并写出清单"
            st_ok "真清单合 rinne-package-preflight/1 的契约" preflight_problems "$rpf"
            st_eq "真清单的 cmakeVersion 与本脚本读 CMakeLists.txt 的一致" \
                "$(mf_one "$rpf" cmakeVersion)" "$(read_cmake_version)"
            st_eq "真清单里的打包清单与清单文件逐行相同" \
                "$(mf_all "$rpf" manifest | paste -sd, -)" "$(grep -vE '^[[:space:]]*(#|$)' "$REPO_ROOT/$MANIFEST_FILE" | paste -sd, -)"
            local missing=0
            while IFS= read -r rel; do
                [ -f "$REPO_ROOT/$rel" ] || missing=$((missing + 1))
            done <<EOF
$({ mf_all "$rpf" license; mf_all "$rpf" notice; mf_all "$rpf" cited; } | LC_ALL=C sort -u)
EOF
            st_eq "真清单里的随包文件与被点名的声明路径，仓里都有" "$missing" "0"
            if pinned_node >/dev/null 2>&1; then
                local rvars rtxt
                rvars="$(printf '{"VERSION":"%s","TAG":"%s","COMMIT":"%s","JUCE_VERSION":"%s","JUCE_COMMIT":"%s","ZIP":"%s"}' \
                    "$(mf_one "$rpf" version)" "$(mf_one "$rpf" tag)" "$(mf_one "$rpf" sourceCommit)" \
                    "$(mf_one "$rpf" juceVersion)" "$(mf_one "$rpf" juceCommit)" "Rinne-v$(mf_one "$rpf" version)-macos-arm64.zip")"
                rtxt="$(render_install_txt "$rvars")"
                st_ok "真 INSTALL.txt（mac 版）：对应源码指引齐全" install_source_problems "$rtxt" "$(mf_one "$rpf" version)" \
                    "$(mf_one "$rpf" sourceUrl)" "$(mf_one "$rpf" sourceCommit)" "$(mf_one "$rpf" juceVersion)" "$(mf_one "$rpf" juceCommit)"
                st_ok "真 INSTALL.txt（mac 版）：使用前必读的规则齐全" install_rule_problems "$rtxt" "$(mf_all "$rpf" ruleEn; mf_all "$rpf" ruleZh)"
                render_provenance > "$work/real-prov.md" 2>/dev/null
                st_ok "真来源表能插进真 NOTICES" provenance_inject "$REPO_ROOT/THIRD-PARTY-NOTICES.md" "$work/real-prov.md"
            else
                st_skip "真 INSTALL.txt / 来源表：没有主版本等于 .node-version 的 Node（设 RINNE_NODE 指到钉版 Node）"
            fi
        else
            st_fail "真 package.ps1 -Preflight 通过并写出清单"
            sed 's/^/        | /' "$work/real-pf.log"
        fi
    else
        st_skip "对着真 package.ps1 核一遍：没有 pwsh"
    fi

}

# 第 17 组：安装 / 卸载脚本各自的自测（它们自己用临时 HOME，绝不碰真实的资源库）。
st_subs() {
    # ── 17. 安装 / 卸载脚本各自的自测（临时 HOME，绝不碰真实的资源库） ─────────
    # 退出码 0 还不够：bash 3.2 下子脚本半路被变量展开出错结束时退出码可能是 0，所以还要看到它最后打印的「自测通过」。
    local sub out rc
    for sub in install-macos.sh uninstall-macos.sh; do
        out="$(bash "$REPO_ROOT/packaging/macos/$sub" --self-test 2>&1)"
        rc=$?
        case "$out" in
            *"${sub%.sh} 自测通过"*) ;;
            *) rc=99 ;;
        esac
        if [ "$rc" -eq 0 ]; then
            st_pass "$sub --self-test"
        else
            st_fail "$sub --self-test（要退出码 0，并且打印「自测通过」）"
            printf '%s\n' "$out" | sed 's/^/        | /'
        fi
    done

}

# 用法：--self-test [all|units|pipeline|real|subs]。默认全跑；分组只为调试时省时间，发版前看 all。
self_test() {
    set +e
    local work which="${1:-all}"
    case "$which" in
        all | units | pipeline | real | subs) ;;
        *) echo "$PROG: 自测分组只认 all / units / pipeline / real / subs：$which" >&2; return 2 ;;
    esac
    work="$(mktemp -d "${TMPDIR:-/tmp}/${TEMP_PREFIX}.XXXXXX")" || {
        echo "FAIL  建不出临时目录"
        return 1
    }
    TEMP_DIR="$work"
    ST_WORK="$work"
    trap 'exit 130' INT
    trap 'exit 143' TERM

    ST_REAL_PWSH="$(command -v pwsh || true)"
    # 被测的就是正在跑的这个文件（流水线用例把它复制进假仓库）。
    ST_SELF="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/$(basename -- "${BASH_SOURCE[0]}")"

    st_write_stubs "$work"
    # 文件系统认不认 chmod：Windows 的 Git Bash 按「有没有 #! / 扩展名」猜可执行位，chmod 去不掉；
    # 那样的机器上，依赖「能去掉可执行位」的两个用例跳过并写明（mac 与 Linux 上照跑）。
    ST_FS_CHMOD=1
    printf '#!/bin/sh\n' > "$work/probe-chmod"
    chmod 0644 "$work/probe-chmod"
    if [ -x "$work/probe-chmod" ]; then ST_FS_CHMOD=0; fi
    export RINNE_DITTO="$work/bin/ditto" RINNE_FILE="$work/bin/file" RINNE_LIPO="$work/bin/lipo"
    export RINNE_OTOOL="$work/bin/otool" RINNE_STRINGS="$work/bin/strings" RINNE_UNZIP="$work/bin/unzip" RINNE_CODESIGN="$work/bin/codesign"

    if [ "$which" = all ] || [ "$which" = units ]; then st_units; fi
    if [ "$which" = all ] || [ "$which" = pipeline ]; then st_pipeline; fi
    if [ "$which" = all ] || [ "$which" = real ]; then st_real; fi
    if [ "$which" = all ] || [ "$which" = subs ]; then st_subs; fi

    if [ "$ST_BAD" -ne 0 ]; then
        echo "package-macos 自测失败：$ST_BAD / $ST_TOTAL 项"
        return 1
    fi
    echo "package-macos 自测通过（$ST_TOTAL 项）"
    return 0
}

case "${1:-}" in
    --self-test)
        shift
        self_test_rc=0
        self_test "$@" || self_test_rc=$?
        RINNE_RAN_TO_END=1
        exit "$self_test_rc"
        ;;
esac
main "$@"
RINNE_RAN_TO_END=1
