# Install Rinne / 安装 Rinne

**EN** Rinne is two VST® plug-ins (plus Audio Unit on macOS): Rinne Input and Rinne Output. Install both, always the
same version. The builds are not signed with an Apple Developer ID and not notarized, so macOS needs one extra step
before the host opens them: unlock first, then open your DAW. The `INSTALL.txt` inside the zip is rendered from this
file; `{{ZIP}}` in the commands below stands for the file name of the zip you downloaded.

**中文** Rinne 是两个 VST® 插件（macOS 上另有 Audio Unit）：Rinne Input 与 Rinne Output。两个都要装，版本必须相同。
macOS 版没有 Apple Developer ID 签名、未经公证，所以打开宿主之前要先多做一步：先解锁，再打开 DAW。zip 里的
`INSTALL.txt` 由本文件渲染而成；下面命令里的 `{{ZIP}}` 指你下载的 zip 文件名。

Before you start: read "Read this first" in the [user guide](USER_GUIDE.zh-CN.md#使用前必读). / 开始之前，请先读[用户指南](USER_GUIDE.zh-CN.md#使用前必读)里的「使用前必读」。

## Windows

**EN**

<!-- BEGIN install:win:en -->

Requirements: Windows 10 or 11 (x64) and the Microsoft Edge WebView2 Runtime (without it the plug-ins show a simple fallback panel).

1. Compare the file hash with the SHA-256 on the Release page.

   ```powershell
   Get-FileHash -Algorithm SHA256 .\{{ZIP}}
   ```

2. Unblock the zip, then extract it.

   ```powershell
   Unblock-File .\{{ZIP}}
   Expand-Archive .\{{ZIP}} -DestinationPath .\Rinne
   ```

3. In a PowerShell window opened as administrator, copy both plug-in folders (the whole `.vst3` folders).

   ```powershell
   Copy-Item -Recurse -Force ".\Rinne\Rinne Input.vst3", ".\Rinne\Rinne Output.vst3" "C:\Program Files\Common Files\VST3\"
   ```

4. Rescan plug-ins in your DAW.

<!-- END install:win:en -->

**中文**

<!-- BEGIN install:win:zh -->

系统要求：Windows 10 或 11（x64），并安装 Microsoft Edge WebView2 运行时（没有它时插件只显示简易面板）。

1. 把文件哈希与 Release 页面上的 SHA-256 对照。

   ```powershell
   Get-FileHash -Algorithm SHA256 .\{{ZIP}}
   ```

2. 解除 zip 的锁定，再解压。

   ```powershell
   Unblock-File .\{{ZIP}}
   Expand-Archive .\{{ZIP}} -DestinationPath .\Rinne
   ```

3. 以管理员身份打开 PowerShell，把两个插件文件夹（整个 `.vst3` 文件夹）复制过去。

   ```powershell
   Copy-Item -Recurse -Force ".\Rinne\Rinne Input.vst3", ".\Rinne\Rinne Output.vst3" "C:\Program Files\Common Files\VST3\"
   ```

4. 在 DAW 里重新扫描插件。

<!-- END install:win:zh -->

## macOS

**EN**

<!-- BEGIN install:mac:en -->

Unlock first, then open your DAW. Requirements: macOS 11 or later on an Apple Silicon Mac, and a host that runs natively
(not under Rosetta). Intel Macs and GarageBand are not supported.

1. Compare the file hash with the SHA-256 on the Release page.

   ```sh
   shasum -a 256 ~/Downloads/{{ZIP}}
   ```

2. Extract the zip into a new folder. If `~/Downloads/Rinne` is already there (an earlier version), move it to the Trash
   first: `ditto` merges into an existing folder and keeps its old files.

   ```sh
   ditto -x -k ~/Downloads/{{ZIP}} ~/Downloads/Rinne
   ```

3. Remove the download quarantine flag from the extracted folder. The plug-ins are not signed, so macOS has to be told
   to allow them.

   ```sh
   xattr -dr com.apple.quarantine ~/Downloads/Rinne
   ```

4. Run the installer script. It copies the plug-ins to your Library folder, clears the download quarantine flag and
   restarts the audio-component service. In Terminal type `bash` and a space, drag `install-macos.sh` into the window,
   press Return.

   ```sh
   bash ~/Downloads/Rinne/install-macos.sh
   ```

5. Open your DAW and rescan plug-ins. In LUNA, VST3 scanning is off by default.

If macOS says it cannot verify the plug-in, do not click "Move to Trash": click "Done", run step 4 and try again.

<!-- END install:mac:en -->

**中文**

<!-- BEGIN install:mac:zh -->

先解锁，再打开 DAW。系统要求：macOS 11 或更高、Apple Silicon 的 Mac，宿主必须原生运行（不能经 Rosetta）。不支持
Intel Mac 与 GarageBand。

1. 把文件哈希与 Release 页面上的 SHA-256 对照。

   ```sh
   shasum -a 256 ~/Downloads/{{ZIP}}
   ```

2. 解压 zip 到一个新文件夹。如果 `~/Downloads/Rinne` 已经在（旧版本解压的），先把它移到废纸篓：`ditto` 会合并进已有的文件夹，
   旧文件会留着。

   ```sh
   ditto -x -k ~/Downloads/{{ZIP}} ~/Downloads/Rinne
   ```

3. 清除解压出来的文件夹上的下载隔离标记。插件没有签名，要明确告诉 macOS 放行。

   ```sh
   xattr -dr com.apple.quarantine ~/Downloads/Rinne
   ```

4. 运行安装脚本。它会把插件复制到你的资源库文件夹、清除下载隔离标记并重启音频组件服务。在终端里输入 `bash` 和一个空格，
   把 `install-macos.sh` 拖进窗口，按回车。

   ```sh
   bash ~/Downloads/Rinne/install-macos.sh
   ```

5. 打开 DAW，重新扫描插件。LUNA 默认没有启用 VST3 扫描。

如果 macOS 提示无法验证插件，不要点「移到废纸篓」：点「完成」，执行第 4 步后重试。

<!-- END install:mac:zh -->

## Upgrade

**EN** Install the new version over the old one, both plug-ins together. A project saved with a newer version is
refused by an older one, and its bytes are kept untouched.

**中文** 直接用新版覆盖旧版，两个插件一起升级。用新版保存的工程，旧版会拒绝载入，并保持原字节不动。

## Uninstall

**EN** Windows: delete the two `.vst3` folders you copied. macOS: run `uninstall-macos.sh` from the zip the same way as
`install-macos.sh`.

**中文** Windows：删除你复制过去的两个 `.vst3` 文件夹。macOS：用与 `install-macos.sh` 相同的方式运行 zip 里的
`uninstall-macos.sh`。

## Build from source (short)

**EN** See `docs/BUILDING.md` at the tag you want to build.

**中文** 见你要构建的那个 tag 上的 `docs/BUILDING.md`。
