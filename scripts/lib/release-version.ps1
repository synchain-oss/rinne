# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/release.json
#
# 发布版本号与 tag 的两条判据（dot-source 用）：scripts/package.ps1 与 scripts/check-release-tag.ps1 共用，
# 别在调用方再抄一份。
#
# 版本真源 = 顶层 CMakeLists.txt 的 project(Rinne ... VERSION x.y.z)。
# **只认 project(Rinne ...) 那一行**：第 1 行 cmake_minimum_required(VERSION 3.22) 也含 VERSION，
# 「取第一条含 VERSION 的行」会拿到 3.22。行首锚定，所以被 # 注释掉的 project(...) 不算。
#
# tag 形态（发行只认两种）：
#   vX.Y.Z        正式版，X.Y.Z 必须等于 CMake 的版本
#   vX.Y.Z-rc.N   预发布，X.Y.Z 同样必须等于 CMake 的版本（rc 不改 CMakeLists）
# 其余一律拒：vX.Y.Z-rc（少了 N）、vX.Y.Z-rc.1.2、vX.Y.Z-RC.1（大小写）、vX.Y.Z-beta.1、vX.Y.Z-alpha.1、
# v0.0.0-test（不做演练 tag）、1.2.3（没有 v）、v1.2、v1.2.3 后跟一个空格、v1.2.3 后跟一个换行、
# v1.2.3 与 v1.2.4 拼在一起（多行）与空串。
#
# 末尾用 \z 而不是 $：$ 在 .NET 的正则里允许匹配到结尾换行之前，v1.2.3 加一个换行会混过去。
# [regex]::Match 区分大小写（PowerShell 的 -match 默认不区分，RC.1 会混进来），所以这一份用前者。

# 版本真源：从 CMakeLists.txt 的文本里取 project(Rinne ... VERSION x.y.z) 的那一段。取不到返回 $null。
function Get-RinneCMakeVersion([string]$CMakeText) {
    if (-not $CMakeText) { return $null }
    $m = [regex]::Match($CMakeText, '(?m)^[ \t]*project[ \t]*\([ \t]*Rinne\b[^)]*?\bVERSION[ \t]+([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $m.Success) { return $null }
    return $m.Groups[1].Value
}

# tag 与版本真源的对应关系。返回 hashtable：Ok / Version（去掉 v 的整串）/ Core（x.y.z）/ Kind（final | rc）/
# Prerelease / Message。判据是纯函数：输入 tag 与 CMake 版本，不碰仓库、不联网，自测直接喂夹具。
function Test-RinneReleaseTag([string]$Tag, [string]$CMakeVersion) {
    $r = @{ Ok = $false; Version = $null; Core = $null; Kind = $null; Prerelease = $false; Message = '' }
    if (-not $Tag) { $r.Message = 'tag 是空的'; return $r }
    if ($Tag -match '[\r\n]') { $r.Message = "tag 含换行：$(($Tag -replace '[\r\n]', '<换行>'))"; return $r }
    if ($Tag -match '\s') { $r.Message = "tag 含空白：$Tag"; return $r }
    $m = [regex]::Match($Tag, '^v([0-9]+\.[0-9]+\.[0-9]+)(?:-(rc\.[0-9]+))?\z')
    if (-not $m.Success) {
        $r.Message = "tag '$Tag' 形态不对：只接受 vX.Y.Z 与 vX.Y.Z-rc.N"
        return $r
    }
    $r.Core = $m.Groups[1].Value
    $r.Version = $Tag.Substring(1)
    $pre = $m.Groups[2].Value
    # 每种形态显式各占一支，没有兜底分支：正则里多放行一种形态却忘了在这里分派时，它会被当成正式版（不比对版本）。
    if (-not $pre) { $r.Kind = 'final' }
    elseif ($pre.StartsWith('rc.', [StringComparison]::Ordinal)) { $r.Kind = 'rc'; $r.Prerelease = $true }
    else {
        $r.Message = "tag '$Tag' 的预发布段 '$pre' 过了正则却没有对应的 Kind（release-version.ps1 的正则与分派不同步）"
        return $r
    }
    if (-not $CMakeVersion) { $r.Message = 'CMakeLists.txt 里没找到 project(Rinne ... VERSION x.y.z)'; return $r }
    if ($r.Core -ne $CMakeVersion) {
        $r.Message = "tag '$Tag' 的 $($r.Core) 与 CMakeLists.txt 的 project(Rinne VERSION $CMakeVersion) 不一致"
        return $r
    }
    $r.Ok = $true
    $r.Message = "tag '$Tag' 与 CMake 版本 $CMakeVersion 一致（$($r.Kind)）"
    return $r
}
