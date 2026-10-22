# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/build.json
#
# 第三方依赖：JUCE（守卫已核过提交）与 Windows 上 WebView2 的 NuGet 包。新增依赖时同一个 PR 登记许可，见 CLAUDE.md。

add_subdirectory("${_rinne_juce_dir}" "${CMAKE_BINARY_DIR}/juce")

# WebView2 NuGet 包：JUCE 的 NEEDS_WEBVIEW2 只在本机找已经装好的包、链接里面的静态 loader，不负责下载。这里在配置期
# 把钉版的包直接从 nuget.org 取到构建目录，按 sha256 核对后解开，再让 JUCE 的 FindWebView2 到那里找，不依赖 nuget.exe
# 和用户目录里的包缓存。sha256 是 nuget.org 上这个版本的 .nupkg 的哈希（与 NuGet 目录记录的 SHA512 交叉核过）。
# 解开的目录带一个记下版本与哈希的标记文件，标记对不上（换了版本或上次没解完）就整个重来。
if(WIN32)
  set(WEBVIEW2_VERSION "1.0.2957.106")
  set(WEBVIEW2_SHA256 "4c35a54835b63954159eac1d5b7a60ae617a41dbb5b73bfdb11c4870a891080a")
  set(_rinne_wv2_root "${CMAKE_BINARY_DIR}/packages")
  set(_rinne_wv2_dir "${_rinne_wv2_root}/Microsoft.Web.WebView2")
  set(_rinne_wv2_stamp "${_rinne_wv2_dir}/.rinne-package")
  set(_rinne_wv2_want "${WEBVIEW2_VERSION} ${WEBVIEW2_SHA256}")
  set(_rinne_wv2_have "")
  if(EXISTS "${_rinne_wv2_stamp}")
    file(READ "${_rinne_wv2_stamp}" _rinne_wv2_have)
  endif()
  if(NOT _rinne_wv2_have STREQUAL _rinne_wv2_want)
    # 下载放在 .download/ 里：FindWebView2 只看 packages/ 下一层名字带 Microsoft.Web.WebView2 的条目，多一个就告警
    set(_rinne_wv2_pkg "${_rinne_wv2_root}/.download/Microsoft.Web.WebView2.${WEBVIEW2_VERSION}.nupkg")
    # rinne-delete-ok: 构建目录 packages/ 下写死的 WebView2 包目录与 .download，换版本时整个重下
    file(REMOVE_RECURSE "${_rinne_wv2_dir}" "${_rinne_wv2_root}/.download")
    message(STATUS "下载 Microsoft.Web.WebView2 ${WEBVIEW2_VERSION}")
    file(DOWNLOAD "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${WEBVIEW2_VERSION}" "${_rinne_wv2_pkg}"
      EXPECTED_HASH SHA256=${WEBVIEW2_SHA256}
      TLS_VERIFY ON
      STATUS _rinne_wv2_status)
    list(GET _rinne_wv2_status 0 _rinne_wv2_rc)
    if(NOT _rinne_wv2_rc EQUAL 0)
      message(FATAL_ERROR "下载 WebView2 包失败：${_rinne_wv2_status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_rinne_wv2_pkg}" DESTINATION "${_rinne_wv2_dir}" PATTERNS "build/native/*")
    # rinne-delete-ok: 构建目录 packages/ 下写死的下载暂存目录，刚在上面建出来
    file(REMOVE_RECURSE "${_rinne_wv2_root}/.download")
    if(NOT EXISTS "${_rinne_wv2_dir}/build/native/include/WebView2.h")
      message(FATAL_ERROR "WebView2 包解开后缺 build/native/include/WebView2.h")
    endif()
    file(WRITE "${_rinne_wv2_stamp}" "${_rinne_wv2_want}")
  endif()
  set(JUCE_WEBVIEW2_PACKAGE_LOCATION "${_rinne_wv2_root}")
endif()
