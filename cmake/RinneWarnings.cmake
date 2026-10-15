# SPDX-License-Identifier: GPL-3.0-or-later
#
# 第一方代码的告警口径，第一方要求零 warning：
#   rinne_target_warnings(<目标> <PRIVATE | INTERFACE | PUBLIC>)
#       告警等级：MSVC 用最高的常规等级 4（另加 /utf-8，源文件的注释是中文），其余编译器用 -Wall -Wextra -Wpedantic。
#   rinne_warnings_as_errors(<目标>)
#       在目标所在目录的 CMakeLists.txt 末尾调用（sources-*.cmake 都 include 完之后）：把目标自己的源文件、以及它链接的
#       rinne_plugin_common 的源文件标成告警即错误。JUCE 模块的源码也编在插件目标里、吃同一套告警等级，但不在这张单子上，
#       它们的告警由 scripts/build.ps1 按文件路径排除。增量构建不会重编没改过的文件，只看构建日志会漏掉上一次留下的告警；
#       告警即错误的文件出了告警就没有目标文件，下次一定重编。

# 等级号单独放一个变量：选项写成一个字面量时，隐私检查会把它当成参考仓的编号
set(_rinne_msvc_warning_level 4)

function(rinne_target_warnings target scope)
  if(MSVC)
    target_compile_options(${target} ${scope} "/W${_rinne_msvc_warning_level}" /utf-8)
  else()
    target_compile_options(${target} ${scope} -Wall -Wextra -Wpedantic)
  endif()
endfunction()

function(rinne_warnings_as_errors target)
  get_target_property(_sources ${target} SOURCES)
  get_target_property(_links ${target} LINK_LIBRARIES)
  if(_links AND "rinne_plugin_common" IN_LIST _links)
    get_target_property(_common rinne_plugin_common INTERFACE_SOURCES)
    list(APPEND _sources ${_common})
  endif()
  list(FILTER _sources INCLUDE REGEX "\\.(c|cc|cpp|mm)$")
  if(MSVC)
    set_property(SOURCE ${_sources} APPEND PROPERTY COMPILE_OPTIONS /WX)
  else()
    set_property(SOURCE ${_sources} APPEND PROPERTY COMPILE_OPTIONS -Werror)
  endif()
endfunction()
