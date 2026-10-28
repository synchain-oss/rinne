# SPDX-License-Identifier: GPL-3.0-or-later
# Derived from synchain-oss/scvb; see upstream/plugin-common.json
#
# rinne_add_web_assets：把 web/ 的界面资源经 juce_add_binary_data 编进插件二进制。
#
# 为什么必须有这一步：WebViewHost 的资源提供者只从二进制数据取资源。ResourceProvider::Source 为空时它恒返回空，
# WebView 拿不到 index.html，页面永远空白，前端也就永远调不到 requestInitialState，表现为「看门狗超时」的兜底面板，
# 而真因是资源根本没进包。
#
# 口径：按文件名扁平匹配。资源提供者按二进制数据里的原始文件名（只有 basename，没有目录）反查，所以：
#   - 目录结构不进包，页面里的相对路径（../shared/x.js、./canvas/y.js）照样能命中；
#   - 但同一个插件的资源集内文件名必须全局唯一，否则先命中者赢，后者永远取不到。
# 本函数在配置期就断言唯一性，重名即 FATAL_ERROR：让它在 cmake 阶段炸，而不是等到真机上表现为一个查不出来的空白面板。
#
# 打包集合与 web-preview/tests/smoke-embedded-resources.mjs 的 packagedFiles 同口径，改一处要同批改另一处：
#   web/<角色>/ 递归的 html、js、css；web/shared 的 js、css、png、json；web/i18n 的 js；web/js/juce 的 js；web/fonts 的 woff2。
#
# 用法：
#   rinne_add_web_assets(TARGET RinneInputWebAssets NAMESPACE RinneInputWebData ROLE_DIR input
#                        [EXTRA_DIRS <相对 web/ 的目录名>...] [LICENSE_FILE <路径>])
# LICENSE_FILE：随包嵌入的许可证文本（文件名同样参与唯一性断言），「查看许可证」从这里取。

function(rinne_add_web_assets)
  set(one_value_args TARGET NAMESPACE ROLE_DIR LICENSE_FILE)
  set(multi_value_args EXTRA_DIRS)
  cmake_parse_arguments(ARG "" "${one_value_args}" "${multi_value_args}" ${ARGN})

  if(NOT ARG_TARGET OR NOT ARG_NAMESPACE OR NOT ARG_ROLE_DIR)
    message(FATAL_ERROR "rinne_add_web_assets: TARGET / NAMESPACE / ROLE_DIR 三者都必填")
  endif()

  set(web_root "${CMAKE_SOURCE_DIR}/web")

  # CONFIGURE_DEPENDS：新增 web 文件会触发重新配置。显式列表会在有人加一个新模块时静默漏掉它
  # （症状同样是运行期空白），glob 加唯一性断言比手工清单可靠。
  file(GLOB_RECURSE role_files CONFIGURE_DEPENDS
    "${web_root}/${ARG_ROLE_DIR}/*.html"
    "${web_root}/${ARG_ROLE_DIR}/*.js"
    "${web_root}/${ARG_ROLE_DIR}/*.css")

  file(GLOB shared_files CONFIGURE_DEPENDS
    "${web_root}/shared/*.js"
    "${web_root}/shared/*.css"
    "${web_root}/shared/*.png"
    "${web_root}/shared/*.json")

  # 三语词条表
  file(GLOB i18n_files CONFIGURE_DEPENDS "${web_root}/i18n/*.js")

  # JUCE 官方前端 helper（桥脚本惰性 import 的 ../js/juce/index.js）
  file(GLOB helper_js_files CONFIGURE_DEPENDS "${web_root}/js/juce/*.js")

  # 字体（样式表的 @font-face 经 ../fonts/*.woff2 引用）
  file(GLOB font_files CONFIGURE_DEPENDS "${web_root}/fonts/*.woff2")

  # EXTRA_DIRS：本角色的 import 闭包越出上面几个目录时补进来（相对 web/ 的目录名）。
  # 真正兜底的是嵌入资源回归里的 import 闭包断言。
  set(extra_files)
  foreach(d IN LISTS ARG_EXTRA_DIRS)
    file(GLOB d_files CONFIGURE_DEPENDS
      "${web_root}/${d}/*.js"
      "${web_root}/${d}/*.css"
      "${web_root}/${d}/*.png")
    if(NOT d_files)
      message(FATAL_ERROR "rinne_add_web_assets: EXTRA_DIRS 里的 'web/${d}' 下没扫到任何资源（路径写错？）")
    endif()
    list(APPEND extra_files ${d_files})
  endforeach()

  set(license_files)
  if(ARG_LICENSE_FILE)
    if(NOT EXISTS "${ARG_LICENSE_FILE}")
      message(FATAL_ERROR "rinne_add_web_assets: LICENSE_FILE '${ARG_LICENSE_FILE}' 不存在")
    endif()
    list(APPEND license_files "${ARG_LICENSE_FILE}")
  endif()

  set(all_files ${role_files} ${shared_files} ${i18n_files} ${helper_js_files} ${font_files} ${extra_files}
      ${license_files})

  if(NOT role_files)
    message(FATAL_ERROR "rinne_add_web_assets: web/${ARG_ROLE_DIR} 下没扫到任何资源")
  endif()

  # 文件名唯一性断言（见文件头「按文件名扁平匹配」）
  set(seen_names)
  foreach(f IN LISTS all_files)
    get_filename_component(base "${f}" NAME)
    if(base IN_LIST seen_names)
      message(FATAL_ERROR
        "rinne_add_web_assets: 资源文件名重复 '${base}'（${ARG_ROLE_DIR} 侧）。"
        " ResourceProvider 按 basename 匹配，重名会让其中一个永远取不到，请改名。")
    endif()
    list(APPEND seen_names "${base}")
  endforeach()

  juce_add_binary_data(${ARG_TARGET}
    NAMESPACE   ${ARG_NAMESPACE}
    HEADER_NAME ${ARG_NAMESPACE}.h
    SOURCES     ${all_files})
endfunction()
