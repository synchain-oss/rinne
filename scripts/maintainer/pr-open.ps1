#!/usr/bin/env pwsh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 开 PR（只开到 feature 线）。拒绝：base 不是 feature/**（dev、prod 必拒）、head 不是 feat/<slug>、draft
# （draft 不触发 bot）。正文必须有 PR 模板的全部栏目、末行是维护者签名；标题与正文过 check-commits pr-text
# （标题格式、隐私、AI 署名、末行签名）与公开文本检查，首行自动加 agent 隐藏标记。同一 head 已有打开的 PR 时只报编号。
#   pwsh scripts/maintainer/pr-open.ps1 -Head feat/<slug> [-Base feature/v1] -Title "<type(scope): 描述>" -BodyFile <f>
#        [-Role impl] [-Round 0]
#   pwsh scripts/maintainer/pr-open.ps1 -SelfTest

[CmdletBinding()]
param(
    [switch]$SelfTest, [string]$Head, [string]$Base = 'feature/v1', [string]$Title, [string]$BodyFile,
    [switch]$Draft, [string]$Role = 'impl', [int]$Round = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/RinneGh.psm1') -Force

<# PR 模板（.github/pull_request_template.md）里的二级标题：正文必须逐个都有。 #>
function Get-TemplateHeadings {
    $t = Get-Content -Raw -LiteralPath (Join-Path (Get-RinneRepoRoot) '.github/pull_request_template.md')
    [regex]::Matches($t, '(?m)^## .+$') | ForEach-Object { $_.Value.TrimEnd() }
}

<# 开 PR 前的全部判据；通过返回要发出的正文（已加标记），否则抛错。 #>
function Test-OpenRequest {
    param([string]$Head, [string]$Base, [string]$Title, [string]$Body, [bool]$Draft, [string]$Role, [int]$Round, [string]$HeadSha)
    if ($Base -notmatch '^feature/[a-z0-9][a-z0-9.-]*$') { throw "base 只能是 feature/**：$Base（不向 dev / prod 开 PR）" }
    if ($Head -notmatch '^feat/[a-z0-9][a-z0-9-]*$') { throw "head 只能是 feat/<slug>：$Head" }
    if ($Draft) { throw '不开 draft：draft 不会触发 bot' }
    $b = Invoke-RinneNodeScript 'scripts/check-commits.mjs' @('branch', '--head', $Head, '--base', $Base) -AllowFail
    if ($b.Code -ne 0) { throw "分支名没过检查：$(($b.Out + ' ' + $b.Err).Trim())" }
    $lines = @($Body -split "`r?`n" | ForEach-Object { $_.TrimEnd() })
    $missing = @(Get-TemplateHeadings | Where-Object { $lines -notcontains $_ })
    if ($missing.Count) { throw "正文缺 PR 模板的栏目：$($missing -join '、')" }
    $full = Format-AgentText -Text $Body -Role $Role -Kind 'pr-body' -Round $Round -Head $HeadSha -Label 'PR 正文'
    Assert-PublicText $Title 'PR 标题'
    $file = [IO.Path]::GetTempFileName()
    try {
        [IO.File]::WriteAllText($file, $full, [Text.UTF8Encoding]::new($false))
        $t = Invoke-RinneNodeScript 'scripts/check-commits.mjs' @('pr-text', '--title', $Title, '--body-file', $file) -AllowFail
        if ($t.Code -ne 0) { throw "PR 标题或正文没过检查：$(($t.Out + ' ' + $t.Err).Trim() -replace '\s+', ' ')" }
    } finally { Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue }
    $full
}

function Invoke-Open {
    param([string]$Head, [string]$Base, [string]$Title, [string]$Body, [bool]$Draft, [string]$Role, [int]$Round)
    $repo = Get-RinneRepo
    $owner = $repo.Split('/')[0]
    $branch = Get-RinneApi "repos/$repo/branches/$([uri]::EscapeDataString($Head))" -AllowFail
    if (-not $branch) { throw "远端没有分支 $Head（先用 rn-push.ps1 推送）" }
    $sha = [string]$branch.commit.sha
    $existing = @(Get-RinneApi "repos/$repo/pulls?state=open&head=$($owner):$([uri]::EscapeDataString($Head))&per_page=10" -Paginate)
    if ($existing.Count) { return "已有打开的 PR #$($existing[0].number)：$($existing[0].html_url)" }
    $full = Test-OpenRequest -Head $Head -Base $Base -Title $Title -Body $Body -Draft $Draft -Role $Role -Round $Round -HeadSha $sha
    $r = Invoke-RinneApiWrite -Method POST -Path "repos/$repo/pulls" -Body @{
        title = $Title; head = $Head; base = $Base; body = $full; draft = $false; maintainer_can_modify = $false
    }
    "已开 PR #$($r.number)：$($r.html_url)（head $($sha.Substring(0, 7))）"
}

function Invoke-SelfTest {
    $cases = New-RinneCases
    $oldPriv = $env:PRIVACY_NEEDLES
    try {
        $env:PRIVACY_NEEDLES = 'rinne-selftest-fake-needle-7c1e'
        Set-RinneNeedlesMode 'ci'
        $template = Get-Content -Raw -LiteralPath (Join-Path (Get-RinneRepoRoot) '.github/pull_request_template.md')
        $good = (($template -replace '(?s)<!--.*?-->', '') -replace '(?m)^Signed-off-by: .*$', (Invoke-RinneMaint -Command 'signoff' -Content '').signoff).Trim()
        $sha = 'a' * 40
        $posted = [System.Collections.Generic.List[object]]::new()
        $open = @()
        $routes = [ordered]@{
            'branches/feat%2Fexample-card' = { @{ commit = @{ sha = $sha } } }
            'branches/'                    = { @{ Code = 1; Out = ''; Err = 'Branch not found' } }
            'pulls\?state=open'            = { , $open }
            'api -X POST \S+/pulls --input' = { param($argv) $posted.Add((Get-Content -Raw ($argv[-1]) | ConvertFrom-Json)); @{ number = 9; html_url = 'https://github.com/x/y/pull/9' } }
        }
        Set-RinneFake (New-RinneFake -Routes $routes)
        $title = 'ci(example): 加入示例'
        $ok = { param($h, $b, $t, $body, $d) -not (Test-Throws { Test-OpenRequest -Head $h -Base $b -Title $t -Body $body -Draft $d -Role impl -Round 0 -HeadSha $sha }) }
        Add-RinneCase $cases '合格的开 PR 请求' $false (-not (& $ok 'feat/example-card' 'feature/v1' $title $good $false))
        Add-RinneCase $cases 'base=dev' $true (-not (& $ok 'feat/example-card' 'dev' $title $good $false))
        Add-RinneCase $cases 'base=prod' $true (-not (& $ok 'feat/example-card' 'prod' $title $good $false))
        Add-RinneCase $cases 'head 不是 feat/' $true (-not (& $ok 'fix/example' 'feature/v1' $title $good $false))
        Add-RinneCase $cases 'head 带卡号' $true (-not (& $ok ('feat/rn' + '-03a-x') 'feature/v1' $title $good $false))
        Add-RinneCase $cases 'draft' $true (-not (& $ok 'feat/example-card' 'feature/v1' $title $good $true))
        Add-RinneCase $cases '正文缺模板栏目' $true (-not (& $ok 'feat/example-card' 'feature/v1' $title ($good -replace '(?m)^## DCO\s*$', '') $false))
        Add-RinneCase $cases '正文末行不是签名' $true (-not (& $ok 'feat/example-card' 'feature/v1' $title "$good`n补一句" $false))
        Add-RinneCase $cases '正文含 Co-Authored-By' $true (-not (& $ok 'feat/example-card' 'feature/v1' $title ($good -replace '(?m)^## DCO', "## DCO`nCo-Authored-By: a <a@b.c>") $false))
        Add-RinneCase $cases '正文含 Generated with' $true (-not (& $ok 'feat/example-card' 'feature/v1' $title ($good -replace '(?m)^## DCO', ("## DCO`nGenerated with [Cla" + 'ude Code](x)')) $false))
        Add-RinneCase $cases '正文含本机路径' $true (-not (& $ok 'feat/example-card' 'feature/v1' $title ($good -replace '(?m)^## DCO', ('## DCO' + "`n" + 'C:' + '\Users\someone')) $false))
        Add-RinneCase $cases '标题格式不对' $true (-not (& $ok 'feat/example-card' 'feature/v1' '加入示例' $good $false))
        Add-RinneCase $cases '标题带卡号' $true (-not (& $ok 'feat/example-card' 'feature/v1' ('ci(example): 完成 RN' + '-03a') $good $false))

        $msg = Invoke-Open -Head 'feat/example-card' -Base 'feature/v1' -Title $title -Body $good -Draft $false -Role impl -Round 0
        $p = $posted[-1]
        Add-RinneCase $cases '发出的 PR：base、head、非 draft、首行标记、末行签名' $false (-not (
                $msg -match '#9' -and $p.base -eq 'feature/v1' -and $p.head -eq 'feat/example-card' -and $p.draft -eq $false -and
                $p.body -match '^<!-- rinne-agent v1 role=impl kind=pr-body round=0 head=aaaaaaa -->' -and $p.body.TrimEnd() -match 'Signed-off-by: [^\n]+$'))
        $open = @([pscustomobject]@{ number = 7; html_url = 'u' })
        $n = $posted.Count
        $again = Invoke-Open -Head 'feat/example-card' -Base 'feature/v1' -Title $title -Body $good -Draft $false -Role impl -Round 0
        Add-RinneCase $cases '同一 head 已有打开的 PR 时不重开' $false (-not ($again -match '#7' -and $posted.Count -eq $n))
        Add-RinneCase $cases '远端没有这个分支' $true (Test-Throws { Invoke-Open -Head 'feat/missing' -Base 'feature/v1' -Title $title -Body $good -Draft $false -Role impl -Round 0 })
    } finally {
        Set-RinneFake $null
        Set-RinneNeedlesMode 'local'
        $env:PRIVACY_NEEDLES = $oldPriv
    }
    Complete-RinneSelfTest $cases 'pr-open'
}

if ($SelfTest) { exit (Invoke-SelfTest) }
if (-not $Head -or -not $Title -or -not $BodyFile) { throw '需要 -Head、-Title 与 -BodyFile' }
Initialize-RinneIdentity
$body = [IO.File]::ReadAllText($BodyFile, [Text.UTF8Encoding]::new($false))
Write-Output (Invoke-Open -Head $Head -Base $Base -Title $Title -Body $body -Draft $Draft.IsPresent -Role $Role -Round $Round)
