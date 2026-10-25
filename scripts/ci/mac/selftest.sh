#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# mac CI 脚本的判据自测：不需要 mac，不需要构建产物，也不联网。build-mac 在最后跑它（前面的步骤红了也照跑），本机也能跑。
# sign-and-verify.sh 与 stress.sh 的反例要真 mac 和构建产物，在各自的步骤里实跑，不在这里。
#   bash scripts/ci/mac/selftest.sh

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
bash "$here/expect-red.sh" --self-test
bash "$here/au-gate.sh" --self-test
bash "$here/pluginval-gate.sh" --self-test
bash "$here/check-cache.sh" --self-test
bash "$here/shm-leftover.sh" --self-test
bash "$here/alloc-canary.sh" --self-test
node "$here/check-bundles.mjs" --self-test
