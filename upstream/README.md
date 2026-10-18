# upstream/：参考仓来源登记

从参考仓 `synchain-oss/scvb` 复制进来的文件，都登记在这里的 `<area>.json` 里：登记哪个参考仓提交、每个文件对应参考仓里的哪个文件的哪个 blob。
为什么这样做、为什么不用共享库或子模块，见 [ADR-0008](../docs/adr/0008-shared-code.md)。

## 片段格式

一个区域（`build`、`ci`、`gates`、`web`、`tests` 等）一个片段，并行的工作各写各的，互不冲突。

```json
{
  "sources": {
    "scvb-dev": { "repo": "synchain-oss/scvb", "ref": "dev", "sha": "<40 位提交>" }
  },
  "files": [
    {
      "path": "scripts/foo.ps1",
      "source": "scvb-dev",
      "from": "scripts/foo.ps1",
      "fromBlob": "<40 位 blob>",
      "mode": "adapted"
    }
  ]
}
```

- `sources`：来源名到参考仓、分支和提交的映射；`sha` 是复制当时来源分支的完整提交号。
- `files[].path` 是本仓的文件，`from` 是参考仓里的路径，`fromBlob` 是复制当时那个文件的 blob。
  一个文件合并自参考仓的几个文件时，同一个 `path` 写几项，`from` 各不相同，必须都在同一个片段里。
- `mode`：`adapted` 是只改名、改命名空间、改写注释，跟踪上游；`forked` 是大幅改写，不逐行跟踪。没有逐字副本：参考仓的注释带内部编号，复制时必须改写。

## 钉 sha 的规程

先 fetch，再 rev-parse，两步缺一不可。不要凭记忆或网页上看到的短 sha 填：

```sh
git -C <参考仓克隆> fetch origin <ref>
git -C <参考仓克隆> rev-parse origin/<ref>                     # 来源的 sha
git -C <参考仓克隆> rev-parse origin/<ref>:<参考仓里的路径>    # 文件的 fromBlob
```

- 参考仓的克隆在本机私有目录里，只读使用；不要在里面提交或切换分支。
- 同一个片段里的 `sha` 是这次复制所依据的提交；之后再往同一区域加文件，要么用同一个 sha，要么另起一个来源名，不要悄悄改旧来源的 sha。
- 复制完把文件头改成唯一的一行（下一节），注释里的内部编号逐处改写成自含的理由，不能只删编号。

## 文件头

复制来的文件，前 8 行内恰好有一行（前面可以带该语言的注释符号）：

```text
Derived from synchain-oss/scvb; see upstream/<area>.json
```

`<area>` 就是登记它的那个片段。头里不写 sha，免得出现第二个真源。不能写注释的格式（`.json`、字体、图片、单行钉版文件等）照常登记，不要求文件头。

## 校验与漂移

```sh
node scripts/check-upstream.mjs --self-test       # 判据自测（坏样例与正例）
node scripts/run-checks.mjs --only upstream       # 合并校验全部片段，docs-truth 里每次都跑
node scripts/check-upstream.mjs --drift --ref-dir <参考仓克隆>
node scripts/check-upstream.mjs --drift           # 不给克隆就经 gh api 读来源分支
```

校验离线进行：片段结构、40 位 sha 与 blob、同一个 (path, from) 只登记一次、登记的文件存在、文件头恰好一行且指向登记它的片段；
反过来，文件头写了却没登记的文件也判红。

`--drift` 联网（或读本地克隆），把登记的 `fromBlob` 与来源分支现在的 blob 逐个比对，blob 相同就是没有漂移。它只给维护者手动跑，不进 CI。
只在三种情况同步：上游修了安全、崩溃或实时安全问题；来源分支已合进上游主干、需要改指；首个正式版之前核对仍指向未合并分支的来源。一次同步一个 PR。
