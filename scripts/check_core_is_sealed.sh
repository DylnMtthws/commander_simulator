#!/usr/bin/env bash
#
# src/core/ must contain no I/O and nothing whose iteration order or value is
# not fully determined by the seed. CMake enforces the coarse half of this: a
# core file that includes an io/ header fails to link, because cs_core does not
# link that target.
#
# This script catches what still compiles and links. Every pattern below maps
# to a specific requirement in SIM_PLAN.md:
#
#   iostream/fstream/cstdio  - section 12.5, core does no I/O, so bindings are additive
#   unordered_map/set        - section 6.5, unspecified iteration order across
#                              libc++ versions; a tiebreak resolved by bucket
#                              order is a reproducibility bug that looks like variance
#   random_device / chrono   - section 6.5, entropy or wall-clock in the core
#                              breaks INVARIANT S1 (pure-function seeding), which
#                              silently disables common random numbers (10.4)
#   rand/srand               - global mutable state, so not thread-safe and not
#                              reproducible under concurrency
#
# The point is not that these are bad in general - they are all fine in cli/ and
# io/. They are banned HERE.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE="${ROOT}/src/core"
status=0

check() {
    local pattern="$1" why="$2"
    # -n for line numbers, -w so "rand" does not match "operand".
    if matches=$(grep -rnw --include='*.hpp' --include='*.cpp' -E "${pattern}" "${CORE}" 2>/dev/null); then
        echo "FAIL: ${why}"
        echo "${matches}" | sed 's/^/    /'
        status=1
    fi
}

check '#include <(iostream|fstream|sstream|cstdio|print)>' \
      'core does no I/O (SIM_PLAN 12.5) - move it to io/ or cli/'
check 'std::unordered_(map|set|multimap|multiset)' \
      'unspecified iteration order breaks determinism (SIM_PLAN 6.5) - use a sorted vector'
check 'std::random_device|std::chrono::(system_clock|steady_clock|high_resolution_clock)' \
      'entropy or wall-clock reads break INVARIANT S1 (SIM_PLAN 7.3)'
check 'srand|std::rand' \
      'global RNG state is neither reproducible nor thread-safe (SIM_PLAN 7.3)'

if [[ ${status} -eq 0 ]]; then
    echo "core is sealed: no I/O, no unordered containers, no ambient entropy."
fi
exit "${status}"
