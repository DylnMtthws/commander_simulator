#!/usr/bin/env bash
#
# Every authored field must have a consumer.
#
# THE FAILURE THIS EXISTS FOR, three instances and counting:
#
#   integer_mana_value  a guard, fully tested, on no code path
#   has_land_face       a loaded column, required at load, never read
#   CARD_COST           an authored effect kind with ZERO readers, so Chrome Mox
#                       and Mox Diamond were free for four phases
#
# All three were found by a person asking. Three instances means asking is not
# working, and the replacement has to be mechanical: it must detect the shape
# without anyone knowing which field they are hunting.
#
# Two stages, because "unread" happens at two different seams:
#
#   1. A KEY IN THE DATA THAT THE LOADER IGNORES. Silently makes an authored
#      property do nothing. The loader already rejects unknown effect KINDS; it
#      does not reject unknown keys inside one.
#   2. A FIELD THE LOADER FILLS THAT NOTHING READS. This is the CARD_COST shape,
#      and the expensive one, because the data is right, the load is right, the
#      test suite is green, and the effect simply never happens.
#
# Stage 2 deliberately does not count effects.hpp (the declaration) or
# effects_load.cpp (the write). A field is "read" only where something acts on it.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HEADER="${ROOT}/src/core/effects.hpp"
LOADER="${ROOT}/src/io/effects_load.cpp"
DATA="${ROOT}/data/effects.toml"
status=0

# ---------------------------------------------------------------------------
# Stage 1: every key used in the data is one the loader looks for.
# ---------------------------------------------------------------------------
if [[ -f "${DATA}" ]]; then
    # COMMENTS ARE STRIPPED FIRST. Without this the check reported `on_the_play`,
    # which appears only in a comment explaining Gemstone Caverns - a false
    # positive, and the kind that gets a check switched off.
    keys=$(sed 's/#.*//' "${DATA}" | grep -oE '^[a-z_]+ *=|[{,] *[a-z_]+ *=' \
        | grep -oE '[a-z_]+' | sort -u)
    for key in ${keys}; do
        # `kind` and `status` steer the parse rather than being read as values.
        case "${key}" in kind|status) continue;; esac
        # The C++ loader OR the Python exporter: `authored_from` is consumed by
        # the drift check in export/, which is a real consumer in another
        # language. "Has a reader" is not "has a C++ reader".
        if grep -q "\"${key}\"" "${LOADER}"; then continue; fi
        if grep -rq "${key}" "${ROOT}/export/src" 2>/dev/null; then continue; fi
        echo "FAIL: data/effects.toml sets '${key}' and nothing looks for it -"
        echo "      not the C++ loader, not the exporter. An authored property that"
        echo "      nothing reads does nothing, silently."
        status=1
    done
fi

# ---------------------------------------------------------------------------
# Stage 2: every field the loader fills is read by something that acts on it.
# ---------------------------------------------------------------------------
fields=$(sed -n '/^struct /,/^};/p' "${HEADER}" \
    | grep -E '^ +(bool|int|std::uint8_t|std::string|ColourMask|CardFilter|RitualZone|TutorFilter|TutorDestination|CloneFilter|SourceCondition|EntersTappedUnless|ModifierMode|AuthorStatus|std::vector<[^>]*>) [a-z_]+' \
    | sed -E 's/^ +[A-Za-z_:<>0-9 ]+ ([a-z_]+).*/\1/' | sort -u)

# TWO EXCLUSIONS, both learned by testing this check against the bug it exists
# for (11.0's eighth rule: write the wrong implementation and check the test
# notices). With CARD_COST's only consumer removed, the first version still
# passed:
#
#   * TESTS DO NOT COUNT. tests/unit/test_convoke.cpp sets has_card_cost to build
#     a fixture, and that made the field look consumed while the simulation
#     ignored it. A test is evidence the field is exercised, not evidence the
#     MODEL acts on it - and the point of this check is the model.
#   * A WRITE IS NOT A READ. `x.field = v` fills the field; it is what the loader
#     does and it is exactly what a dead field still has. `+=`, `==` and bare
#     uses all count, so a read-modify-write is a consumer.
for field in ${fields}; do
    # `|| true` ON BOTH GREPS, AND IT IS NOT COSMETIC.
    #
    # grep exits 1 when it matches nothing. Under `set -euo pipefail` that killed
    # this script the moment a field had no readers - so the check produced NO
    # OUTPUT AT ALL in exactly the case it exists to report, and passed loudly in
    # every other. A checker that is silent on failure and cheerful on success is
    # the mechanism-quietly-not-running family, in the thing built to catch it.
    #
    # Found by testing the check against the bug it was written for, which is why
    # 11.0's eighth rule says to do that rather than to reason about it.
    readers=$(grep -rhE "[.>]${field}\b" \
        --include='*.cpp' --include='*.hpp' \
        --exclude='effects.hpp' --exclude='effects_load.cpp' \
        "${ROOT}/src" 2>/dev/null || true)
    readers=$(printf '%s' "${readers}" | grep -vE "[.>]${field} *=[^=]" || true)
    readers=$(printf '%s' "${readers}" | grep -c . || true)
    if [[ "${readers}" == "0" ]]; then
        echo "FAIL: core/effects.hpp declares '${field}', the loader fills it, and nothing reads it."
        echo "      This is the CARD_COST shape: right data, right load, green tests, no effect."
        status=1
    fi
done

if [[ ${status} -eq 0 ]]; then
    echo "every authored field has a reader."
fi
exit "${status}"
