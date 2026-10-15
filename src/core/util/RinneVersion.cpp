// SPDX-License-Identifier: GPL-3.0-or-later

#include "util/RinneVersion.h"

namespace rinne::build
{

// RINNE_BUILD_LABEL_STRING 只加在本文件的编译参数上（见 src/core/CMakeLists.txt），换标签只重编这一个文件
const char* buildLabel() noexcept
{
    return RINNE_BUILD_LABEL_STRING;
}

} // namespace rinne::build
