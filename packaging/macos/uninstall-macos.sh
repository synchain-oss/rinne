#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 从 macOS 上卸掉 Rinne 的四个插件 bundle。
# 默认处理当前登录用户的 ~/Library/Audio/Plug-Ins/...；加 --system 处理 /Library/Audio/Plug-Ins/...。
# 卸完还要让 macOS 重新登记 Audio Unit，宿主再扫一次就看不到它们了。
#
# 用法：在「终端」里输入 `bash ` 和一个空格，把这个文件拖进窗口，按回车。也可以直接：
#   bash uninstall-macos.sh              卸掉 ~/Library/Audio/Plug-Ins/{VST3,Components} 里的
#   bash uninstall-macos.sh --system     卸掉 /Library/Audio/Plug-Ins/{VST3,Components} 里的
#   bash uninstall-macos.sh --dry-run    只打印会删什么，不动任何文件
#   bash uninstall-macos.sh --self-test  判据自测（用临时目录，绝不碰真实的资源库）
#
# 删除纪律：删的目标只有四个写死的名字
# （Rinne Input.vst3、Rinne Output.vst3、Rinne Input.component、Rinne Output.component），
# 没有通配，插件目录里的别的文件原样留下。删之前逐项核对：名字在白名单里、目标不是符号链接、
# 解析出来的真实路径正好是「目标根目录 + 那个名字」、目标根目录既不是 / 也不是 HOME 自己。
# 任何一条不成立就拒绝删，并让用户自己处理。判据本身是一个纯函数，自测只用纯函数断言它，
# 不在真实路径上试删。
#
# 外部命令（killall / xattr）的名字可以用环境变量换掉（RINNE_KILLALL、RINNE_XATTR）：
# 那是给自测换桩用的。命令不存在时脚本只告警、不中断。

set -uo pipefail

PROG="$(basename -- "$0")"

# 四个 bundle 的名字：白名单只有这四个。别的文件一律不动。
BUNDLES="Rinne Input.vst3
Rinne Output.vst3
Rinne Input.component
Rinne Output.component"

# 外部命令：默认用系统自带的；自测用环境变量换成桩。
# 命令名在调用时现读（不是脚本开头展开的）：自测里换桩之后同一进程里也要生效。
killall_cmd() { printf '%s' "${RINNE_KILLALL:-killall}"; }
xattr_cmd() { printf '%s' "${RINNE_XATTR:-xattr}"; }

# 临时目录的名字前缀带本进程号：删临时目录时据此确认「这是本进程建的」。
TEMP_PREFIX="rinne-uninstall-macos.$$"
TEMP_DIR=""

warn() { echo "$PROG: 提醒：$*" >&2; }
die() { echo "$PROG: 错误：$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
用法：uninstall-macos.sh [--system] [--dry-run] [--self-test] [--help]

  --system    处理 /Library/Audio/Plug-Ins/{VST3,Components}（这台 Mac 的所有用户）
  --dry-run   只打印会删什么，不动任何文件
  --self-test 判据自测：全部在临时目录里做，不碰真实的资源库
  --help      显示这段说明
EOF
}

# ── 目标目录 ────────────────────────────────────────────────────────────────
# 用户级在 ~/Library，系统级在 /Library。名字在脚本里写死，不从环境变量或参数推。
target_dirs() { # $1 user|system；把两个目录打印出来
    if [ "$1" = "system" ]; then
        printf '%s\n%s\n' "/Library/Audio/Plug-Ins/VST3" "/Library/Audio/Plug-Ins/Components"
    else
        printf '%s\n%s\n' "$HOME/Library/Audio/Plug-Ins/VST3" "$HOME/Library/Audio/Plug-Ins/Components"
    fi
}

# ── 删除守卫 ────────────────────────────────────────────────────────────────
# bundle_removable 是纯函数：只看观测到的事实（名字、目标根、解析后的目标根、目标是不是
# 符号链接、解析后的目标路径），不碰文件系统。返回 0 = 可以删；返回 1 = 拒绝，理由打印出来。
bundle_removable() { # $1 bundle 名 $2 目标根 $3 解析后的目标根 $4 目标是符号链接(yes|no) $5 解析后的目标路径
    local name="$1" root="$2" root_real="$3" is_link="$4" real="$5"
    case "$name" in
        "Rinne Input.vst3" | "Rinne Output.vst3" | "Rinne Input.component" | "Rinne Output.component") ;;
        *) echo "名字不在白名单里" >&2; return 1 ;;
    esac
    case "$root" in
        "" | "/") echo "目标根目录就是根目录" >&2; return 1 ;;
    esac
    if [ "$root" = "$HOME" ]; then echo "目标根目录就是 HOME 自己" >&2; return 1; fi
    case "$root_real" in
        "" | "/") echo "解析后的目标根目录就是根目录" >&2; return 1 ;;
    esac
    if [ "$root_real" = "$HOME" ]; then echo "解析后的目标根目录就是 HOME 自己" >&2; return 1; fi
    if [ "$is_link" = "yes" ]; then echo "目标是符号链接" >&2; return 1; fi
    if [ "$real" != "$root_real/$name" ]; then echo "解析后的目标路径不是期望的那一个" >&2; return 1; fi
    return 0
}

# 真正去删：先把事实收集齐，交给纯函数判；判过才删。删的是解析后的真实路径。
# 目标本来就不在，返回 0（没东西可删不算错）。
remove_bundle() { # $1 目标根目录 $2 bundle 名
    local root="$1" name="$2" root_real path real is_link=no
    [ -d "$root" ] || return 0
    root_real="$(cd -- "$root" 2>/dev/null && pwd -P)" || return 1
    path="$root/$name"
    # 悬空的符号链接 -e 为假，但它占着名字：要走下面的判据（会被拒），不能当「没有」放过去。
    { [ -e "$path" ] || [ -L "$path" ]; } || return 0
    if [ -L "$path" ]; then is_link=yes; fi
    real="$(cd -- "$path" 2>/dev/null && pwd -P)" || real=""
    if ! bundle_removable "$name" "$root" "$root_real" "$is_link" "$real"; then
        return 1
    fi
    rm -rf -- "$real" || return 1
    return 0
}

# ── 卸载 ────────────────────────────────────────────────────────────────────
# 只删四个名字锚定的 bundle；插件目录里别的文件原样留下。
uninstall_bundles() { # $1 VST3 目录 $2 Components 目录 $3 dry-run(yes|no)
    local vst3="$1" comp="$2" dry="${3:-no}" name target
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        case "$name" in
            *.vst3) target="$vst3/$name" ;;
            *) target="$comp/$name" ;;
        esac
        if [ ! -e "$target" ] && [ ! -L "$target" ]; then
            echo "没有装：$target"
            continue
        fi
        if [ "$dry" = "yes" ]; then
            echo "（演练）会删：$target"
            continue
        fi
        if ! remove_bundle "$(dirname -- "$target")" "$name"; then
            die "$target 不能自动删（它不是可以直接删除的普通目录）。请手动处理。"
        fi
        echo "删掉了：$target"
    done <<EOF
$BUNDLES
EOF
    [ "$dry" = "yes" ] && return 0
    restart_audio_registration
    return 0
}

# 让 macOS 重新登记 Audio Unit，宿主再扫一次就看不到卸掉的 Audio Unit 了。
# killall 不在（或进程本来就没在跑）都只告警。
restart_audio_registration() {
    local killall
    killall="$(killall_cmd)"
    if ! command -v "$killall" >/dev/null 2>&1; then
        warn "找不到 killall，跳过 Audio Unit 重新登记。请在宿主里手动重新扫描插件。"
        return 0
    fi
    if ! "$killall" AudioComponentRegistrar >/dev/null 2>&1; then
        warn "Audio Unit 登记服务没有在运行（可以忽略）；若宿主里还看得到插件，请手动重新扫描一次。"
    fi
    return 0
}

# ── 临时目录 ────────────────────────────────────────────────────────────────
# 临时目录的删除守卫：解析后的真实路径必须在临时根下一层，名字要带本进程的前缀。
temp_dir_is_ours() { # $1 目录
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

cleanup_temp() { # $1 本进程建的临时目录
    local dir="${1:-$TEMP_DIR}"
    [ -n "$dir" ] || return 0
    if temp_dir_is_ours "$dir"; then
        rm -rf -- "$dir"
    fi
    if [ "$dir" = "$TEMP_DIR" ]; then TEMP_DIR=""; fi
    return 0
}

# ── 自测 ────────────────────────────────────────────────────────────────────
# 全部在临时目录里做：HOME 指到临时目录，绝不对真实的资源库做删除测试。
self_test() {
    local bad=0 work home_root saved_home
    say() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; bad=1; fi; }
    ok() { if [ "$1" = 0 ]; then say 0 "$2"; else say 1 "$2"; fi; }

    work="$(mktemp -d "${TMPDIR:-/tmp}/${TEMP_PREFIX}.XXXXXX")" || {
        echo "FAIL  建不出临时目录"
        return 1
    }
    TEMP_DIR="$work"
    trap 'cleanup_temp "$TEMP_DIR"' EXIT

    saved_home="$HOME"
    home_root="$work/home"
    mkdir -p "$home_root"
    HOME="$home_root"

    # killall 换成桩：自测不能真的去杀这台机器上的 AudioComponentRegistrar。
    mkdir -p "$work/stub"
    cat > "$work/stub/killall" <<'STUB'
#!/bin/sh
printf '%s %s\n' "$(basename "$0")" "$*" >> "${RINNE_STUB_LOG:-/dev/null}"
exit 0
STUB
    chmod 0755 "$work/stub/killall"
    RINNE_KILLALL="$work/stub/killall"
    RINNE_STUB_LOG="$work/stub/calls.log"
    export RINNE_KILLALL RINNE_STUB_LOG

    # ── 删除守卫的纯函数断言（不动文件系统）──────────────────────────────
    local fake_root="$work/fake-root" fake_real="$work/fake-root/Rinne Input.vst3"
    if bundle_removable "Rinne Input.vst3" "$fake_root" "$fake_root" no "$fake_real" 2>/dev/null; then
        say 0 "白名单里的名字、路径对得上：可以删"
    else
        say 1 "白名单里的名字、路径对得上：可以删"
    fi
    if ! bundle_removable "Rinne Probe Input.vst3" "$fake_root" "$fake_root" no "$fake_real" 2>/dev/null; then
        say 0 "名字不在白名单里：拒绝删"
    else
        say 1 "名字不在白名单里：拒绝删"
    fi
    if ! bundle_removable "Rinne Input.vst3" "/" "/" no "/Rinne Input.vst3" 2>/dev/null; then
        say 0 "目标根是根目录：拒绝删"
    else
        say 1 "目标根是根目录：拒绝删"
    fi
    if ! bundle_removable "Rinne Input.vst3" "$HOME" "$HOME" no "$HOME/Rinne Input.vst3" 2>/dev/null; then
        say 0 "目标根是 HOME 自己：拒绝删"
    else
        say 1 "目标根是 HOME 自己：拒绝删"
    fi
    if ! bundle_removable "Rinne Input.vst3" "$fake_root" "$fake_root" yes "$fake_real" 2>/dev/null; then
        say 0 "目标是符号链接：拒绝删"
    else
        say 1 "目标是符号链接：拒绝删"
    fi
    if ! bundle_removable "Rinne Input.vst3" "$fake_root" "$fake_root" no "$work/somewhere-else" 2>/dev/null; then
        say 0 "解析后的路径不是期望的那一个：拒绝删"
    else
        say 1 "解析后的路径不是期望的那一个：拒绝删"
    fi

    # ── 目标目录：默认用户级，--system 系统级 ─────────────────────────────
    local user_v user_c sys_v sys_c
    user_v="$(target_dirs user | sed -n 1p)"
    user_c="$(target_dirs user | sed -n 2p)"
    sys_v="$(target_dirs system | sed -n 1p)"
    sys_c="$(target_dirs system | sed -n 2p)"
    [ "$user_v" = "$HOME/Library/Audio/Plug-Ins/VST3" ] && [ "$user_c" = "$HOME/Library/Audio/Plug-Ins/Components" ]
    ok $? "默认处理 ~/Library/Audio/Plug-Ins/{VST3,Components}"
    [ "$sys_v" = "/Library/Audio/Plug-Ins/VST3" ] && [ "$sys_c" = "/Library/Audio/Plug-Ins/Components" ]
    ok $? "--system 处理 /Library/Audio/Plug-Ins/{VST3,Components}"

    # ── 装一份假的，再卸掉：四个 bundle 都没了，别的文件还在 ──────────────
    local vst3="$home_root/Library/Audio/Plug-Ins/VST3"
    local comp="$home_root/Library/Audio/Plug-Ins/Components"
    mkdir -p "$vst3" "$comp"
    local name
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        local dir
        case "$name" in
            *.vst3) dir="$vst3/$name" ;;
            *) dir="$comp/$name" ;;
        esac
        mkdir -p "$dir/Contents/MacOS"
        printf 'x\n' > "$dir/Contents/MacOS/${name%.*}"
    done <<EOF
$BUNDLES
EOF
    # 目录里别的插件与别的文件：卸载不许碰
    mkdir -p "$vst3/别的插件.vst3" "$comp/别的插件.component"
    printf 'x\n' > "$vst3/别的插件.vst3/marker"
    printf 'x\n' > "$comp/别的插件.component/marker"
    printf 'x\n' > "$vst3/说明.txt"

    uninstall_bundles "$vst3" "$comp" no >"$work/uninstall.log" 2>&1
    local left=0 name2
    while IFS= read -r name2; do
        [ -n "$name2" ] || continue
        case "$name2" in
            *.vst3) [ -e "$vst3/$name2" ] && left=1 ;;
            *) [ -e "$comp/$name2" ] && left=1 ;;
        esac
    done <<EOF
$BUNDLES
EOF
    [ "$left" = 0 ]
    ok $? "卸载后四个 bundle 都没了"
    grep -q -- "killall AudioComponentRegistrar" "$work/stub/calls.log"
    ok $? "卸完让系统重新登记 Audio Unit（killall AudioComponentRegistrar）"
    [ -f "$vst3/别的插件.vst3/marker" ] && [ -f "$comp/别的插件.component/marker" ] && [ -f "$vst3/说明.txt" ]
    ok $? "插件目录里别的文件原样留下"

    # ── 演练：只打印，不删 ────────────────────────────────────────────────
    mkdir -p "$vst3/Rinne Input.vst3"
    printf 'x\n' > "$vst3/Rinne Input.vst3/marker"
    local dry_out
    dry_out="$(uninstall_bundles "$vst3" "$comp" yes 2>&1)"
    [ -f "$vst3/Rinne Input.vst3/marker" ]
    ok $? "--dry-run 不删任何文件"
    case "$dry_out" in
        *Rinne*) say 0 "--dry-run 打印了会删的 bundle" ;;
        *) say 1 "--dry-run 打印了会删的 bundle" ;;
    esac

    # ── 名字不在白名单里的文件不会被删 ────────────────────────────────────
    mkdir -p "$vst3/Rinne Probe Input.vst3"
    printf 'x\n' > "$vst3/Rinne Probe Input.vst3/marker"
    uninstall_bundles "$vst3" "$comp" no >"$work/uninstall2.log" 2>&1
    [ -f "$vst3/Rinne Probe Input.vst3/marker" ]
    ok $? "名字不在白名单里的 bundle 不会被删"

    # ── 删除守卫的补充：解析后的目标根是 / 或 HOME 自己（目标根本身是个指过去的链接）也拒绝 ──
    if ! bundle_removable "Rinne Input.vst3" "$fake_root" "/" no "//Rinne Input.vst3" 2>/dev/null; then
        say 0 "解析后的目标根是根目录：拒绝删"
    else
        say 1 "解析后的目标根是根目录：拒绝删"
    fi
    if ! bundle_removable "Rinne Input.vst3" "$fake_root" "$HOME" no "$HOME/Rinne Input.vst3" 2>/dev/null; then
        say 0 "解析后的目标根是 HOME 自己：拒绝删"
    else
        say 1 "解析后的目标根是 HOME 自己：拒绝删"
    fi
    if ! bundle_removable "Rinne Input.vst3" "/" "$fake_root" no "$fake_root/Rinne Input.vst3" 2>/dev/null; then
        say 0 "目标根写的是根目录（解析后指到别处）：拒绝删"
    else
        say 1 "目标根写的是根目录（解析后指到别处）：拒绝删"
    fi
    if ! bundle_removable "Rinne Input.vst3" "$HOME" "$fake_root" no "$fake_root/Rinne Input.vst3" 2>/dev/null; then
        say 0 "目标根写的是 HOME 自己（解析后指到别处）：拒绝删"
    else
        say 1 "目标根写的是 HOME 自己（解析后指到别处）：拒绝删"
    fi

    # ── 真的文件系统上：符号链接、悬空链接、白名单外的名字都拒绝，被指向的东西和链接本身都原样留下 ──
    local sl_root="$work/symlink-root" sl_target="$work/symlink-target"
    mkdir -p "$sl_root" "$sl_target/Rinne Output.vst3" "$sl_root/别的插件.vst3"
    printf 'x\n' > "$sl_target/Rinne Output.vst3/marker"
    printf 'x\n' > "$sl_root/别的插件.vst3/marker"
    ln -s "$sl_target/Rinne Output.vst3" "$sl_root/Rinne Output.vst3" 2>/dev/null
    ln -s "$work/nowhere-at-all" "$sl_root/Rinne Input.vst3" 2>/dev/null
    if [ -L "$sl_root/Rinne Output.vst3" ]; then
        remove_bundle "$sl_root" "Rinne Output.vst3" 2>/dev/null
        if [ $? -ne 0 ] && [ -L "$sl_root/Rinne Output.vst3" ] && [ -f "$sl_target/Rinne Output.vst3/marker" ]; then
            say 0 "符号链接的 bundle：拒绝删，链接和它指向的目录都原样留下"
        else
            say 1 "符号链接的 bundle：拒绝删，链接和它指向的目录都原样留下"
        fi
    fi
    if [ -L "$sl_root/Rinne Input.vst3" ]; then
        remove_bundle "$sl_root" "Rinne Input.vst3" 2>/dev/null
        if [ $? -ne 0 ] && [ -L "$sl_root/Rinne Input.vst3" ]; then
            say 0 "悬空的符号链接占着名字：拒绝删，不当成「没装」放过去"
        else
            say 1 "悬空的符号链接占着名字：拒绝删，不当成「没装」放过去"
        fi
    else
        echo "SKIP  这个系统建不了符号链接：符号链接的用例跳过"
    fi
    remove_bundle "$sl_root" "别的插件.vst3" 2>/dev/null
    if [ $? -ne 0 ] && [ -f "$sl_root/别的插件.vst3/marker" ]; then
        say 0 "名字不在白名单里：remove_bundle 拒绝删，目录原样留下"
    else
        say 1 "名字不在白名单里：remove_bundle 拒绝删，目录原样留下"
    fi

    # ── HOME 指到别处时也不越界 ───────────────────────────────────────────
    local other_home="$work/other-home"
    mkdir -p "$other_home/Library/Audio/Plug-Ins/VST3/Rinne Input.vst3"
    printf 'x\n' > "$other_home/Library/Audio/Plug-Ins/VST3/Rinne Input.vst3/marker"
    local outside="$work/outside"
    mkdir -p "$outside/Rinne Input.vst3"
    printf 'x\n' > "$outside/Rinne Input.vst3/marker"
    remove_bundle "$other_home/Library/Audio/Plug-Ins/VST3" "Rinne Input.vst3"
    if [ $? -eq 0 ] && [ ! -e "$other_home/Library/Audio/Plug-Ins/VST3/Rinne Input.vst3" ]; then
        say 0 "HOME 指到别处时，只删那个 HOME 下面的 bundle"
    else
        say 1 "HOME 指到别处时，只删那个 HOME 下面的 bundle"
    fi
    [ -f "$outside/Rinne Input.vst3/marker" ]
    ok $? "HOME 之外的同名 bundle 没被碰"

    # ── --system：只做 dry-run 打印，不删 /Library 下的任何东西；脚本复制出来，从入口整条跑 ──
    cp "$0" "$work/uninstall-copy.sh"
    local sys_out sys_rc
    sys_out="$(bash "$work/uninstall-copy.sh" --system --dry-run 2>&1)"
    sys_rc=$?
    [ "$sys_rc" -eq 0 ]
    ok $? "--system --dry-run 正常退出"
    case "$sys_out" in
        *"/Library/Audio/Plug-Ins/VST3/Rinne Input.vst3"*"/Library/Audio/Plug-Ins/Components/Rinne Output.component"*) say 0 "--system --dry-run 打印的是 /Library 下的目标" ;;
        *) say 1 "--system --dry-run 打印的是 /Library 下的目标" ;;
    esac
    case "$sys_out" in
        *"$HOME/Library"*) say 1 "--system --dry-run 的输出里没有 ~/Library" ;;
        *) say 0 "--system --dry-run 的输出里没有 ~/Library" ;;
    esac
    bash "$work/uninstall-copy.sh" --no-such-option >/dev/null 2>&1
    [ $? -ne 0 ]
    ok $? "不认识的参数：非 0 退出"
    HOME="" bash "$work/uninstall-copy.sh" --dry-run >/dev/null 2>&1
    [ $? -ne 0 ]
    ok $? "HOME 是空的：用户级拒绝运行（不会拼出 /Library）"
    HOME="/" bash "$work/uninstall-copy.sh" --dry-run >/dev/null 2>&1
    [ $? -ne 0 ]
    ok $? "HOME 就是 /：用户级拒绝运行"
    HOME="" bash "$work/uninstall-copy.sh" --system --dry-run >/dev/null 2>&1
    ok $? "--system 不依赖 HOME：HOME 是空的也能 dry-run"

    # ── 缺 killall：只告警，不中断 ────────────────────────────────────────
    local warn_out
    warn_out="$(RINNE_KILLALL=rinne-no-such-killall uninstall_bundles "$vst3" "$comp" no 2>&1; echo "RC=$?")"
    case "$warn_out" in
        *RC=0*) say 0 "缺 killall 时退出码仍是 0（只告警）" ;;
        *) say 1 "缺 killall 时退出码仍是 0（只告警）" ;;
    esac
    case "$warn_out" in
        *killall*) say 0 "缺 killall 时给出了告警" ;;
        *) say 1 "缺 killall 时给出了告警" ;;
    esac

    # ── 临时目录：删之前要核对真实路径在临时根下一层、名字带本进程前缀 ────
    if temp_dir_is_ours "$work"; then say 0 "本进程建的临时目录认得出"; else say 1 "本进程建的临时目录认得出"; fi
    if temp_dir_is_ours "${TMPDIR:-/tmp}"; then say 1 "临时根目录本身不该被当成可删目录"; else say 0 "临时根目录本身不该被当成可删目录"; fi
    if temp_dir_is_ours "$work/nested"; then say 1 "临时根下面的子目录不该被当成可删目录"; else say 0 "临时根下面的子目录不该被当成可删目录"; fi
    mkdir -p "$work/${TEMP_PREFIX}.nested"
    if temp_dir_is_ours "$work/${TEMP_PREFIX}.nested"; then say 1 "名字带前缀但不在临时根下一层的目录不该被当成可删目录"; else say 0 "名字带前缀但不在临时根下一层的目录不该被当成可删目录"; fi

    HOME="$saved_home"

    [ "$bad" = 0 ] || { echo "uninstall-macos 自测失败"; return 1; }
    echo "uninstall-macos 自测通过"
    return 0
}

# ── 入口 ────────────────────────────────────────────────────────────────────
main() {
    local scope=user dry=no
    while [ $# -gt 0 ]; do
        case "$1" in
            --system) scope=system; shift ;;
            --dry-run) dry=yes; shift ;;
            --self-test) self_test; return $? ;;
            -h | --help)
                usage
                return 0
                ;;
            *) usage >&2; die "不认识的参数：$1" ;;
        esac
    done

    # 用户级要靠 HOME 拼目录：HOME 是空的或就是 / 时会拼出 /Library/…（系统目录），直接拒绝。
    if [ "$scope" = user ]; then
        { [ -n "${HOME:-}" ] && [ "$HOME" != "/" ] && [ -d "$HOME" ]; } \
            || die "HOME 是空的、就是 / 或不是目录（\"${HOME:-}\"）：不能当用户目录用。"
    fi

    local vst3 comp
    vst3="$(target_dirs "$scope" | sed -n 1p)"
    comp="$(target_dirs "$scope" | sed -n 2p)"

    echo "$PROG：从下面两个目录里删掉 Rinne 的四个插件 bundle"
    echo "  $vst3"
    echo "  $comp"
    if [ "$scope" = system ] && [ "$dry" != yes ] && [ "$(id -u)" != "0" ]; then
        if command -v sudo >/dev/null 2>&1 && [ -t 0 ]; then
            echo "处理 /Library 需要管理员密码，接下来会问一次。"
            # 用绝对路径重新跑自己（相对路径在 sudo 下找不到）。
            exec sudo -- bash "$(cd -- "$(dirname -- "$0")" && pwd -P)/$(basename -- "$0")" --system
        fi
        die "处理 /Library 需要管理员密码：请用 sudo 运行本脚本。"
    fi

    uninstall_bundles "$vst3" "$comp" "$dry"
    if [ "$dry" = yes ]; then
        echo "（演练结束，没有删掉任何东西。）"
    else
        echo "卸好了。打开宿主并重新扫描插件。"
    fi
    return 0
}

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi
main "$@"
