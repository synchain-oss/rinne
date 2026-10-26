#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 把发行包里的四个插件 bundle 装到 macOS 的插件目录。
# 默认装给当前登录的用户（~/Library/Audio/Plug-Ins/...）；加 --system 装给这台 Mac 的
# 所有用户（/Library/Audio/Plug-Ins/...），那需要管理员密码。
# Rinne 不签名也不公证，所以还要去掉下载隔离标记，并让 macOS 重新登记 Audio Unit。
#
# 用法：在「终端」里输入 `bash ` 和一个空格，把这个文件拖进窗口，按回车。也可以直接：
#   bash install-macos.sh                装到 ~/Library/Audio/Plug-Ins/{VST3,Components}
#   bash install-macos.sh --system       装到 /Library/Audio/Plug-Ins/{VST3,Components}
#   bash install-macos.sh --dry-run      只打印要做什么，不动任何文件
#   bash install-macos.sh --self-test    判据自测（用临时目录，绝不碰真实的资源库）
#
# 外部命令（ditto / xattr / killall）：正常运行时一律用写死的系统路径（/usr/bin/ditto、/usr/bin/xattr、
# /usr/bin/killall），不读任何环境变量——随包发出去的脚本不能被环境里的变量换掉要执行的命令
# （比如 sudo -E 配 --system 时，那些命令会以 root 身份执行）。只有 --self-test 路径认 RINNE_DITTO、
# RINNE_XATTR、RINNE_KILLALL：那是给自测换桩用的，让自测不依赖某台机器上装没装这些命令。
# 命令不存在时脚本只告警、不中断，用户自己用 xattr 补一次也能继续。
#
# 删除纪律：升级时会删掉同名旧 bundle。删的目标只有四个写死的名字
# （Rinne Input.vst3、Rinne Output.vst3、Rinne Input.component、Rinne Output.component），
# 没有通配；删之前逐项核对：名字在白名单里、目标不是符号链接、解析出来的真实路径正好是
# 「目标根目录 + 那个名字」、目标根目录既不是 / 也不是 HOME 自己。任何一条不成立就拒绝删，
# 并让用户自己处理。判据本身是一个纯函数，自测只用纯函数断言它，不在真实路径上试删。
# 这是维护者批准的例外：本脚本删的是产品自己的安装目标，不是本进程新建的临时目录；守卫就是上面这几条。

set -uo pipefail

PROG="$(basename -- "$0")"

# 四个 bundle 的名字：白名单只有这四个。删旧版本时也只碰这四个，不动目录里的别的文件。
BUNDLES="Rinne Input.vst3
Rinne Output.vst3
Rinne Input.component
Rinne Output.component"

# 外部命令：正常路径写死系统自带的绝对路径，不读环境变量；只有自测路径（SELF_TEST_MODE=1，由 self_test 自己置位，
# 这里无条件先置 0，所以环境里同名的变量改不了它）才认 RINNE_* 覆盖，换成桩。
# 命令名在调用时现读（不是脚本开头展开的）：自测里换桩之后同一进程里也要生效。
SELF_TEST_MODE=0
xattr_cmd() { if [ "$SELF_TEST_MODE" = 1 ]; then printf '%s' "${RINNE_XATTR:-/usr/bin/xattr}"; else printf '%s' "/usr/bin/xattr"; fi; }
killall_cmd() { if [ "$SELF_TEST_MODE" = 1 ]; then printf '%s' "${RINNE_KILLALL:-/usr/bin/killall}"; else printf '%s' "/usr/bin/killall"; fi; }
ditto_cmd() { if [ "$SELF_TEST_MODE" = 1 ]; then printf '%s' "${RINNE_DITTO:-/usr/bin/ditto}"; else printf '%s' "/usr/bin/ditto"; fi; }

# 临时目录的名字前缀带本进程号：删临时目录时据此确认「这是本进程建的」。
TEMP_PREFIX="rinne-install-macos.$$"
TEMP_DIR=""

warn() { echo "$PROG: 提醒：$*" >&2; }
die() { echo "$PROG: 错误：$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
用法：install-macos.sh [--system] [--dry-run] [--self-test] [--help]

  --system    装到 /Library/Audio/Plug-Ins/{VST3,Components}（这台 Mac 的所有用户，需要管理员密码）
  --dry-run   只打印要做什么，不复制、不删除、不改任何文件
  --self-test 判据自测：全部在临时目录里做，不碰真实的资源库
  --help      显示这段说明
EOF
}

# ── 目标目录 ────────────────────────────────────────────────────────────────
# 用户级装到 ~/Library，系统级装到 /Library。名字在脚本里写死，不从环境变量或参数推。
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
# 拒绝的判据都在这里，删之前逐条过一遍。
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

# 判：把事实收集齐，交给纯函数判；判过就把解析后的真实路径打印出来。这一步不删任何东西。
# 返回 0 = 可以删（路径已打印）；1 = 拒绝；2 = 目标本来就不在（没东西可删）。
# 悬空的符号链接 -e 为假，但它占着名字：要走判据（会被拒），不能当「没有」放过去。
bundle_verdict() { # $1 目标根目录 $2 bundle 名
    local root="$1" name="$2" root_real path real is_link=no
    [ -d "$root" ] || return 2
    root_real="$(cd -- "$root" 2>/dev/null && pwd -P)" || return 1
    path="$root/$name"
    { [ -e "$path" ] || [ -L "$path" ]; } || return 2
    if [ -L "$path" ]; then is_link=yes; fi
    real="$(cd -- "$path" 2>/dev/null && pwd -P)" || real=""
    bundle_removable "$name" "$root" "$root_real" "$is_link" "$real" || return 1
    printf '%s\n' "$real"
}

# 真正去删：先过 bundle_verdict，判过才删。删的是解析后的真实路径。
# 目标本来就不在，返回 0（没东西可删不算错）。
remove_bundle() { # $1 目标根目录 $2 bundle 名
    local real rc=0
    real="$(bundle_verdict "$1" "$2")" || rc=$?
    case "$rc" in
        0) ;;
        2) return 0 ;;
        *) return 1 ;;
    esac
    # 这里是维护者批准的例外：删的是产品自己的安装目标（用户机器上的插件目录），不是本进程新建的目录，
    # 也不是仓内写死的输出目录，所以不靠「临时目录」这条理由放行，靠下面的守卫。
    # rinne-delete-ok: 维护者批准的例外：名字锚定的四个 bundle 之一；bundle_removable 已核真实路径等于期望路径、不是符号链接、根不是 / 与 HOME 本身
    rm -rf -- "$real" || return 1
    return 0
}

# ── 安装 ────────────────────────────────────────────────────────────────────
# 复制用 ditto：它保住可执行位与符号链接，cp -r 不保（cp -r 出来的插件宿主加载不了）。
# 升级时先删掉同名旧 bundle，免得新旧文件混在一起；删的是名字锚定的那四个，别的文件不动。
install_bundles() { # $1 源目录 $2 VST3 目标目录 $3 Components 目标目录 $4 dry-run(yes|no)
    local src="$1" vst3="$2" comp="$3" dry="${4:-no}" name dest
    [ -d "$src" ] || die "找不到插件目录：$src"
    if [ "$dry" = "yes" ]; then
        echo "（演练）会把下面四个 bundle 复制到目标目录："
    else
        mkdir -p -- "$vst3" "$comp" || die "建不出目标目录：$vst3"
    fi
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        [ -d "$src/$name" ] || die "发行包里没有 ${name}（在 $src 里找不到）"
        case "$name" in
            *.vst3) dest="$vst3" ;;
            *) dest="$comp" ;;
        esac
        if [ "$dry" = "yes" ]; then
            echo "  $src/$name  →  $dest/$name"
            continue
        fi
        if [ -e "$dest/$name" ] || [ -L "$dest/$name" ]; then
            if ! remove_bundle "$dest" "$name"; then
                die "$dest/$name 已经在，但它不是可以直接替换的普通目录（可能是软链接）。请手动处理后再运行。"
            fi
        fi
        if ! "$(ditto_cmd)" --norsrc --noextattr --noacl -- "$src/$name" "$dest/$name"; then
            die "复制 $name 失败：ditto 没能把它放进 $dest"
        fi
        if [ ! -x "$dest/$name/Contents/MacOS/${name%.*}" ]; then
            die "$dest/$name 里的主程序没有可执行位，复制没成功"
        fi
        echo "装好了：$dest/$name"
    done <<EOF
$BUNDLES
EOF
    [ "$dry" = "yes" ] && return 0
    clear_quarantine "$vst3" "$comp"
    restart_audio_registration
    return 0
}

# 去掉下载隔离标记：Rinne 不签名也不公证，不去掉的话宿主会拒绝加载。
# 命令不存在（或者系统上没这个属性）都只告警：用户可以自己用 xattr 补一次。
clear_quarantine() { # $1 VST3 目录 $2 Components 目录
    local vst3="$1" comp="$2" name target xattr
    xattr="$(xattr_cmd)"
    if ! command -v "$xattr" >/dev/null 2>&1; then
        warn "找不到 xattr，跳过隔离标记清除。宿主若拒绝加载，请手动运行：xattr -dr com.apple.quarantine <插件目录>"
        return 0
    fi
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        case "$name" in
            *.vst3) target="$vst3/$name" ;;
            *) target="$comp/$name" ;;
        esac
        [ -e "$target" ] || continue
        if ! "$xattr" -dr com.apple.quarantine "$target" 2>/dev/null; then
            warn "清除 $target 的隔离标记失败：可能本来就没有这个属性（可以忽略）"
        fi
    done <<EOF
$BUNDLES
EOF
    return 0
}

# 让 macOS 重新登记 Audio Unit，宿主再扫一次就能看到新的 Audio Unit。
# killall 不在（或进程本来就没在跑）都只告警。
restart_audio_registration() {
    local killall
    killall="$(killall_cmd)"
    if ! command -v "$killall" >/dev/null 2>&1; then
        warn "找不到 killall，跳过 Audio Unit 重新登记。请在宿主里手动重新扫描插件。"
        return 0
    fi
    if ! "$killall" AudioComponentRegistrar >/dev/null 2>&1; then
        warn "Audio Unit 登记服务没有在运行（可以忽略）；若宿主里看不到新插件，请手动重新扫描一次。"
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
        # rinne-delete-ok: 本进程 mktemp -d 建的临时目录；temp_dir_is_ours 已核真实路径在临时根下一层且带本进程前缀
        rm -rf -- "$dir"
    fi
    if [ "$dir" = "$TEMP_DIR" ]; then TEMP_DIR=""; fi
    return 0
}

# 「走完了」标记（约定写法，scripts/ci/mac/selftest.sh 与本机 gate 会扫）。macOS 自带的 bash 3.2 在 set -u 下遇到变量展开出错，
# 会直接结束整个脚本；装了 EXIT trap 时进入 trap 的 $? 常常是 0，自测就会在半路死掉之后以 0 退出，调用它的 CI 步骤是绿的。
# 所以自测只有走到末尾才把标记置 1，trap 里退出码为 0 而标记没置位，就改成非 0。
# 这个 trap 只由 self_test 装（正常安装 / 卸载不装），所以标记只在自测那条路上起作用；
# 自测里的 trap 是整个脚本唯一的 EXIT trap，没有别处会覆盖它。
RINNE_RAN_TO_END=0
on_exit() {
    local rc=$?
    cleanup_temp "$TEMP_DIR"
    if [ "$rc" -eq 0 ] && [ "$RINNE_RAN_TO_END" != 1 ]; then
        echo "::error::$PROG: 脚本没有走完就退出了（bash 3.2 下变量展开出错会静默结束脚本，退出码是 0）：按失败处理" >&2
        exit 1
    fi
}

# ── 自测 ────────────────────────────────────────────────────────────────────
# 全部在临时目录里做：HOME 指到临时目录，绝不对真实的资源库做删除测试。
# ditto / xattr / killall 全部换成桩：自测不依赖某台机器上装没装这些命令。
self_test() {
    local bad=0 work home_root saved_home
    say() { if [ "$1" = 0 ]; then echo "PASS  $2"; else echo "FAIL  $2"; bad=1; fi; }
    ok() { if [ "$1" = 0 ]; then say 0 "$2"; else say 1 "$2"; fi; }

    work="$(mktemp -d "${TMPDIR:-/tmp}/${TEMP_PREFIX}.XXXXXX")" || {
        echo "FAIL  建不出临时目录"
        return 1
    }
    TEMP_DIR="$work"
    SELF_TEST_MODE=1
    trap on_exit EXIT

    # 自测期间 HOME 指到临时目录：后面所有「默认装到 ~/Library」的断言都对着它，
    # 真实的 ~/Library 全程没被碰过。
    saved_home="$HOME"
    home_root="$work/home"
    mkdir -p "$home_root"
    HOME="$home_root"

    # ditto 的桩：只做「把源目录的内容复制到目标目录」，保留可执行位（cp -R 保留文件的权限位）。
    local stub="$work/stub"
    mkdir -p "$stub"
    cat > "$stub/ditto" <<'STUB'
#!/bin/sh
# 自测用的 ditto 桩：忽略选项，把源目录的内容复制到目标目录。
printf '%s\n' "$*" >> "${RINNE_DITTO_LOG:-/dev/null}"
src=""
dst=""
for a in "$@"; do
    case "$a" in
        -*) ;;
        *) if [ -z "$src" ]; then src="$a"; else dst="$a"; fi ;;
    esac
done
[ -n "$src" ] && [ -n "$dst" ] || exit 2
mkdir -p -- "$dst" || exit 1
cp -R -- "$src/." "$dst/" || exit 1
exit 0
STUB
    chmod 0755 "$stub/ditto"
    RINNE_DITTO="$stub/ditto"
    RINNE_DITTO_LOG="$stub/ditto.log"
    export RINNE_DITTO_LOG
    # xattr / killall 也换成桩：自测不能真的去杀这台机器上的 AudioComponentRegistrar。
    local tool
    for tool in xattr killall; do
        cat > "$stub/$tool" <<'STUB'
#!/bin/sh
printf '%s %s\n' "$(basename "$0")" "$*" >> "${RINNE_STUB_LOG:-/dev/null}"
exit 0
STUB
        chmod 0755 "$stub/$tool"
    done
    RINNE_XATTR="$stub/xattr"
    RINNE_KILLALL="$stub/killall"
    RINNE_STUB_LOG="$stub/calls.log"
    export RINNE_XATTR RINNE_KILLALL RINNE_STUB_LOG
    export RINNE_DITTO

    # ── 删除守卫的纯函数断言（不动文件系统）────────────────────────────────
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
    ok $? "默认装到 ~/Library/Audio/Plug-Ins/{VST3,Components}"
    [ "$sys_v" = "/Library/Audio/Plug-Ins/VST3" ] && [ "$sys_c" = "/Library/Audio/Plug-Ins/Components" ]
    ok $? "--system 装到 /Library/Audio/Plug-Ins/{VST3,Components}"
    case "$sys_v" in
        /Library/*) say 0 "--system 的目标在 /Library 下" ;;
        *) say 1 "--system 的目标在 /Library 下" ;;
    esac

    # ── 造一份假的发行包：四个 bundle，各带一个可执行的主程序 ─────────────
    local src="$work/pkg"
    mkdir -p "$src" "$home_root"
    local name
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        mkdir -p "$src/$name/Contents/MacOS"
        printf '#!/bin/sh\nexit 0\n' > "$src/$name/Contents/MacOS/${name%.*}"
        chmod 0755 "$src/$name/Contents/MacOS/${name%.*}"
    done <<EOF
$BUNDLES
EOF

    # ── 演练：不写任何文件 ────────────────────────────────────────────────
    local dry_out dry_v dry_c
    dry_v="$home_root/Library/Audio/Plug-Ins/VST3"
    dry_c="$home_root/Library/Audio/Plug-Ins/Components"
    dry_out="$(install_bundles "$src" "$dry_v" "$dry_c" yes 2>&1)"
    [ ! -e "$dry_v" ] && [ ! -e "$dry_c" ]
    ok $? "--dry-run 只打印，不建目录也不复制"
    case "$dry_out" in
        *Rinne*) say 0 "--dry-run 打印了要装的 bundle" ;;
        *) say 1 "--dry-run 打印了要装的 bundle" ;;
    esac

    # ── 真装：四个 bundle 都在，别的文件原样留下 ──────────────────────────
    local keep="$dry_v/别的插件.vst3"
    mkdir -p "$keep"
    printf 'x\n' > "$keep/keep.txt"
    install_bundles "$src" "$dry_v" "$dry_c" no >"$work/install.log" 2>&1
    local all_there=0 name2
    while IFS= read -r name2; do
        [ -n "$name2" ] || continue
        case "$name2" in
            *.vst3) [ -d "$dry_v/$name2" ] || all_there=1 ;;
            *) [ -d "$dry_c/$name2" ] || all_there=1 ;;
        esac
    done <<EOF
$BUNDLES
EOF
    [ "$all_there" = 0 ]
    ok $? "安装后四个 bundle 都在目标目录里"
    [ -f "$keep/keep.txt" ]
    ok $? "目标目录里别的文件原样留下"
    [ -x "$dry_v/Rinne Input.vst3/Contents/MacOS/Rinne Input" ]
    ok $? "装好的 bundle 里的主程序还有可执行位"
    grep -q -- "--norsrc --noextattr --noacl" "$stub/ditto.log"
    ok $? "复制走 ditto，且带 --norsrc --noextattr --noacl"
    grep -q -- "xattr -dr com.apple.quarantine" "$stub/calls.log"
    ok $? "装完去掉隔离标记（xattr -dr com.apple.quarantine）"
    grep -q -- "killall AudioComponentRegistrar" "$stub/calls.log"
    ok $? "装完让系统重新登记 Audio Unit（killall AudioComponentRegistrar）"

    # ── 升级：旧 bundle 被换掉，别的文件不动 ──────────────────────────────
    printf 'old\n' > "$dry_v/Rinne Input.vst3/Contents/MacOS/旧文件"
    install_bundles "$src" "$dry_v" "$dry_c" no >"$work/reinstall.log" 2>&1
    [ ! -e "$dry_v/Rinne Input.vst3/Contents/MacOS/旧文件" ]
    ok $? "重装时同名旧 bundle 被换掉了"
    [ -f "$keep/keep.txt" ]
    ok $? "重装时目录里别的文件仍然原样留下"

    # ── 卸载要删的那几个：只删名字锚定的那四个，别的留下 ──────────────────
    local u_root="$work/uninstall-root"
    mkdir -p "$u_root/Rinne Input.vst3" "$u_root/别的插件.vst3"
    printf 'x\n' > "$u_root/Rinne Input.vst3/marker"
    printf 'x\n' > "$u_root/别的插件.vst3/marker"
    remove_bundle "$u_root" "Rinne Input.vst3"
    if [ $? -eq 0 ] && [ ! -e "$u_root/Rinne Input.vst3" ] && [ -f "$u_root/别的插件.vst3/marker" ]; then
        say 0 "删掉白名单里的一个 bundle，别的文件不动"
    else
        say 1 "删掉白名单里的一个 bundle，别的文件不动"
    fi

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
    # “应被拒”的用例只调 bundle_verdict（判，不删）：守卫一旦坏了，这里也不会真的去递归删除任何东西。
    local sl_root="$work/symlink-root" sl_target="$work/symlink-target"
    mkdir -p "$sl_root" "$sl_target/Rinne Output.vst3" "$sl_root/别的插件.vst3"
    printf 'x\n' > "$sl_target/Rinne Output.vst3/marker"
    printf 'x\n' > "$sl_root/别的插件.vst3/marker"
    ln -s "$sl_target/Rinne Output.vst3" "$sl_root/Rinne Output.vst3" 2>/dev/null
    ln -s "$work/nowhere-at-all" "$sl_root/Rinne Input.vst3" 2>/dev/null
    if [ -L "$sl_root/Rinne Output.vst3" ]; then
        bundle_verdict "$sl_root" "Rinne Output.vst3" >/dev/null 2>&1
        if [ $? -eq 1 ] && [ -L "$sl_root/Rinne Output.vst3" ] && [ -f "$sl_target/Rinne Output.vst3/marker" ]; then
            say 0 "符号链接的 bundle：拒绝删，链接和它指向的目录都原样留下"
        else
            say 1 "符号链接的 bundle：拒绝删，链接和它指向的目录都原样留下"
        fi
    fi
    if [ -L "$sl_root/Rinne Input.vst3" ]; then
        bundle_verdict "$sl_root" "Rinne Input.vst3" >/dev/null 2>&1
        if [ $? -eq 1 ] && [ -L "$sl_root/Rinne Input.vst3" ]; then
            say 0 "悬空的符号链接占着名字：拒绝删，不当成「没装」放过去"
        else
            say 1 "悬空的符号链接占着名字：拒绝删，不当成「没装」放过去"
        fi
    else
        echo "SKIP  这个系统建不了符号链接：符号链接的用例跳过"
    fi
    bundle_verdict "$sl_root" "别的插件.vst3" >/dev/null 2>&1
    if [ $? -eq 1 ] && [ -f "$sl_root/别的插件.vst3/marker" ]; then
        say 0 "名字不在白名单里：bundle_verdict 拒绝，目录原样留下"
    else
        say 1 "名字不在白名单里：bundle_verdict 拒绝，目录原样留下"
    fi

    # 判的正例与“不在”：普通的白名单 bundle 给出解析后的真实路径；目标不在返回 2；都不删任何东西。
    mkdir -p "$work/verdict-root/Rinne Input.vst3"
    local vd
    vd="$(bundle_verdict "$work/verdict-root" "Rinne Input.vst3" 2>/dev/null)"
    if [ $? -eq 0 ] && [ "$vd" = "$(cd -- "$work/verdict-root" && pwd -P)/Rinne Input.vst3" ] && [ -d "$work/verdict-root/Rinne Input.vst3" ]; then
        say 0 "bundle_verdict：普通的白名单 bundle 给出解析后的真实路径，并且没有删它"
    else
        say 1 "bundle_verdict：普通的白名单 bundle 给出解析后的真实路径，并且没有删它"
    fi
    bundle_verdict "$work/verdict-root" "Rinne Output.vst3" >/dev/null 2>&1
    if [ $? -eq 2 ]; then
        say 0 "bundle_verdict：目标不在时返回 2（没东西可删）"
    else
        say 1 "bundle_verdict：目标不在时返回 2（没东西可删）"
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

    # ── --system：只做 dry-run 打印，不写 /Library；脚本复制到假的发行包目录里，从入口整条跑 ──
    local sys_pkg="$work/pkg-sys"
    mkdir -p "$sys_pkg"
    cp -R "$src/." "$sys_pkg/"
    cp "$0" "$sys_pkg/install-macos.sh"
    local sys_out
    sys_out="$(bash "$sys_pkg/install-macos.sh" --system --dry-run 2>&1)"
    local sys_rc=$?
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
    bash "$sys_pkg/install-macos.sh" --no-such-option >/dev/null 2>&1
    [ $? -ne 0 ]
    ok $? "不认识的参数：非 0 退出"
    HOME="" bash "$sys_pkg/install-macos.sh" --dry-run >/dev/null 2>&1
    [ $? -ne 0 ]
    ok $? "HOME 是空的：用户级拒绝运行（不会拼出 /Library）"
    HOME="/" bash "$sys_pkg/install-macos.sh" --dry-run >/dev/null 2>&1
    [ $? -ne 0 ]
    ok $? "HOME 就是 /：用户级拒绝运行"
    HOME="" bash "$sys_pkg/install-macos.sh" --system --dry-run >/dev/null 2>&1
    ok $? "--system 不依赖 HOME：HOME 是空的也能 dry-run"

    # ── 缺 xattr / killall：只告警，不中断 ────────────────────────────────
    local warn_out
    warn_out="$(RINNE_XATTR=rinne-no-such-xattr RINNE_KILLALL=rinne-no-such-killall \
        install_bundles "$src" "$work/x1" "$work/x2" no 2>&1; echo "RC=$?")"
    [ -d "$work/x1/Rinne Input.vst3" ]
    ok $? "缺 xattr 与 killall 时仍然装完，没有中断"
    case "$warn_out" in
        *RC=0*) say 0 "缺 xattr 与 killall 时退出码仍是 0（只告警）" ;;
        *) say 1 "缺 xattr 与 killall 时退出码仍是 0（只告警）" ;;
    esac
    case "$warn_out" in
        *xattr*) say 0 "缺 xattr 时给出了告警" ;;
        *) say 1 "缺 xattr 时给出了告警" ;;
    esac
    case "$warn_out" in
        *killall*) say 0 "缺 killall 时给出了告警" ;;
        *) say 1 "缺 killall 时给出了告警" ;;
    esac

    # ── 外部命令：正常路径写死系统路径，环境变量改不了；只有自测路径认 RINNE_* 覆盖 ────
    # 随包发出去的脚本不能被环境里的变量换掉要执行的命令（sudo -E 配 --system 时那是 root 身份执行任意命令）。
    local marker="$work/injected-ran" inj="$work/inject-cmd" got
    printf '#!/bin/sh\necho ran >> "%s"\nexit 0\n' "$marker" > "$inj"
    chmod 0755 "$inj"
    got="$( SELF_TEST_MODE=0; RINNE_DITTO="$inj"; ditto_cmd )"
    [ "$got" = "/usr/bin/ditto" ]
    ok $? "正常路径：ditto 写死 /usr/bin/ditto，环境里的 RINNE_DITTO 不生效"
    got="$( SELF_TEST_MODE=0; RINNE_XATTR="$inj"; xattr_cmd )"
    [ "$got" = "/usr/bin/xattr" ]
    ok $? "正常路径：xattr 写死 /usr/bin/xattr，环境里的 RINNE_XATTR 不生效"
    got="$( SELF_TEST_MODE=0; RINNE_KILLALL="$inj"; killall_cmd )"
    [ "$got" = "/usr/bin/killall" ]
    ok $? "正常路径：killall 写死 /usr/bin/killall，环境里的 RINNE_KILLALL 不生效"
    got="$( SELF_TEST_MODE=1; RINNE_DITTO="$inj"; RINNE_XATTR="$inj"; RINNE_KILLALL="$inj"; ditto_cmd; xattr_cmd; killall_cmd )"
    [ "$got" = "$inj$inj$inj" ]
    ok $? "自测路径：RINNE_DITTO / RINNE_XATTR / RINNE_KILLALL 才生效（上面所有桩调用靠它）"
    got="$( SELF_TEST_MODE=1; unset RINNE_DITTO RINNE_XATTR RINNE_KILLALL; ditto_cmd; xattr_cmd; killall_cmd )"
    [ "$got" = "/usr/bin/ditto/usr/bin/xattr/usr/bin/killall" ]
    ok $? "自测路径没设覆盖变量时回到写死的系统路径"
    # 反例（整条）：另起一个进程走正常路径，环境里把三个覆盖变量都指到会留痕的假命令，它们一个都不许被执行。
    # 让这个进程在 ditto 复制完第一个 bundle 之后、还没碰 xattr 与 killall 之前就停下（主程序没有可执行位）：
    # 自测不能真的去杀这台机器上的 AudioComponentRegistrar。HOME 指到临时目录。
    local inj_home="$work/inject-home" inj_pkg="$work/pkg-inject" inj_out
    mkdir -p "$inj_home" "$inj_pkg"
    cp -R "$src/." "$inj_pkg/"
    cp "$0" "$inj_pkg/install-macos.sh"
    chmod -x "$inj_pkg/Rinne Input.vst3/Contents/MacOS/Rinne Input"
    if [ -x "$inj_pkg/Rinne Input.vst3/Contents/MacOS/Rinne Input" ]; then
        echo "SKIP  这个文件系统去不掉可执行位：正常路径整条跑一遍的反例跳过（上面的函数用例照常做）"
    else
        rm -f "$marker"
        inj_out="$(HOME="$inj_home" RINNE_DITTO="$inj" RINNE_XATTR="$inj" RINNE_KILLALL="$inj" bash "$inj_pkg/install-macos.sh" 2>&1)"
        [ ! -e "$marker" ]
        ok $? "正常路径整条跑一遍：环境里指向假命令的 RINNE_DITTO / RINNE_XATTR / RINNE_KILLALL 一个都没被执行"
        case "$inj_out" in
            *"没有可执行位"* | *"复制"*"失败"*) say 0 "正常路径整条跑一遍：确实走到了复制那一步才停（反例不是空转）" ;;
            *) say 1 "正常路径整条跑一遍：确实走到了复制那一步才停（反例不是空转）" ;;
        esac
    fi

    # ── 临时目录：删之前要核对真实路径在临时根下一层、名字带本进程前缀 ────
    if temp_dir_is_ours "$work"; then say 0 "本进程建的临时目录认得出"; else say 1 "本进程建的临时目录认得出"; fi
    if temp_dir_is_ours "${TMPDIR:-/tmp}"; then say 1 "临时根目录本身不该被当成可删目录"; else say 0 "临时根目录本身不该被当成可删目录"; fi
    if temp_dir_is_ours "$work/nested"; then say 1 "临时根下面的子目录不该被当成可删目录"; else say 0 "临时根下面的子目录不该被当成可删目录"; fi
    mkdir -p "$work/${TEMP_PREFIX}.nested"
    if temp_dir_is_ours "$work/${TEMP_PREFIX}.nested"; then say 1 "名字带前缀但不在临时根下一层的目录不该被当成可删目录"; else say 0 "名字带前缀但不在临时根下一层的目录不该被当成可删目录"; fi

    HOME="$saved_home"

    [ "$bad" = 0 ] || { echo "install-macos 自测失败"; return 1; }
    echo "install-macos 自测通过"
    return 0
}

# ── 入口 ────────────────────────────────────────────────────────────────────
main() {
    local scope=user dry=no
    while [ $# -gt 0 ]; do
        case "$1" in
            --system) scope=system; shift ;;
            --dry-run) dry=yes; shift ;;
            --self-test) self_test; self_rc=$?; RINNE_RAN_TO_END=1; return "$self_rc" ;;
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

    local script_dir
    script_dir="$(cd -- "$(dirname -- "$0")" 2>/dev/null && pwd -P)" || die "算不出脚本所在目录"
    [ -n "$script_dir" ] || die "算不出脚本所在目录"

    local vst3 comp
    vst3="$(target_dirs "$scope" | sed -n 1p)"
    comp="$(target_dirs "$scope" | sed -n 2p)"

    echo "${PROG}：把 Rinne 的四个插件 bundle 装到下面两个目录"
    echo "  $vst3"
    echo "  $comp"
    if [ "$scope" = system ] && [ "$dry" != yes ] && [ "$(id -u)" != "0" ]; then
        if command -v sudo >/dev/null 2>&1 && [ -t 0 ]; then
            echo "装到 /Library 需要管理员密码，接下来会问一次。"
            # 用绝对路径重新跑自己（相对路径在 sudo 下找不到）。
            exec sudo -- bash "$script_dir/$(basename -- "$0")" --system
        fi
        die "装到 /Library 需要管理员密码：请用 sudo 运行本脚本。"
    fi

    install_bundles "$script_dir" "$vst3" "$comp" "$dry"
    if [ "$dry" = yes ]; then
        echo "（演练结束，没有安装任何东西。）"
    else
        echo "装好了。打开宿主并重新扫描插件；Audio Unit 需要重新登记时会自动做一次。"
    fi
    return 0
}

if [ "${1:-}" = "--self-test" ]; then
    self_test_rc=0
    self_test || self_test_rc=$?
    RINNE_RAN_TO_END=1
    exit "$self_test_rc"
fi
main_rc=0
main "$@" || main_rc=$?
RINNE_RAN_TO_END=1
exit "$main_rc"
