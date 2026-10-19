// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipc/PlatformSegmentBackend.h"

namespace rinne::ipc
{

const char* platformSegmentBackendName() noexcept
{
#if defined(_WIN32)
    return "win32";
#elif defined(__APPLE__)
    return "posix";
#else
    return "inprocess";
#endif
}

} // namespace rinne::ipc
