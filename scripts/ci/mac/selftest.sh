#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# mac CI 脚本的判据自测：不需要 mac，不需要构建产物。build-mac 一开头跑它，本机也能跑。
#   bash scripts/ci/mac/selftest.sh

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
bash "$here/expect-red.sh" --self-test
bash "$here/au-gate.sh" --self-test
bash "$here/check-cache.sh" --self-test
bash "$here/shm-leftover.sh" --self-test
bash "$here/alloc-canary.sh" --self-test
node "$here/check-bundles.mjs" --self-test
