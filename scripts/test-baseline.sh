#!/usr/bin/env bash
# Host regression checks; does not prove Windows runtime connectivity.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $# -gt 1 || ( $# -eq 1 && $1 != --slirp ) ]]; then
    echo 'Usage: bash scripts/test-baseline.sh [--slirp]' >&2
    exit 2
fi
cc=${CC:-gcc}
mkdir -p work/baseline-tests/obj
# --no-ignore: the inherited .gitignore pattern "core" also matches src/core/.
mapfile -t sources < <(rg --files --no-ignore src | sort | sed -n '/\.c$/p' | sed '\|^src/main.c$|d')
objects=()
for source in "${sources[@]}"; do
    object="work/baseline-tests/obj/${source#src/}"
    object="${object%.c}.o"
    mkdir -p "$(dirname "$object")"
    "$cc" -O2 -std=c99 -Wall -Wextra -Werror -Iinclude -Isrc/net -c "$source" -o "$object" -pthread
    objects+=("$object")
done
for suite in core at audio dsp ppp nat resample sequence; do
    "$cc" -O2 -std=c99 -Wall -Wextra -Werror -Iinclude -Isrc/net \
        "tests/test_${suite}.c" "${objects[@]}" -pthread -lm -o "work/baseline-tests/test_${suite}"
    if ! (cd work/baseline-tests && "./test_${suite}" > "test_${suite}.log" 2>&1); then
        cat "work/baseline-tests/test_${suite}.log"
        exit 1
    fi
    tail -3 "work/baseline-tests/test_${suite}.log"
done
python3 - <<'PY'
import json, re
from pathlib import Path
counts = {}
for name in ('core', 'at', 'audio', 'dsp', 'ppp', 'nat', 'resample', 'sequence'):
    log = Path('work/baseline-tests/test_' + name + '.log').read_text()
    match = re.search(r'(\d+) passed, (\d+) failed', log)
    if not match or int(match[2]):
        raise SystemExit('Missing/failed test summary: ' + name)
    counts[name] = int(match[1])
report = {'host_tests_only': True, 'suites': counts, 'passed': sum(counts.values()), 'failed': 0}
Path('work/baseline-tests/results.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
PY
if [[ ${1:-} == --slirp ]]; then
    read -r -a slirp_libs <<< "${SLIRP_LIBS:--lslirp}"
    "$cc" -O2 -std=c99 -Wall -Wextra -Werror -DVMODEM_HAVE_LIBSLIRP \
        -Iinclude -Isrc/net tests/test_slirp.c "${sources[@]}" \
        "${slirp_libs[@]}" -pthread -lm -o work/baseline-tests/test_slirp
    (cd work/baseline-tests && ./test_slirp) | tee work/baseline-tests/test_slirp.log
fi

rate_objects=()
for object in "${objects[@]}"; do
    [[ $object == */modem/vm_modem.o ]] || rate_objects+=("$object")
done
"$cc" -O2 -std=c99 -Wall -Wextra -Werror -Iinclude -Isrc/net \
    tests/test_modem_rate.c "${rate_objects[@]}" -pthread -lm \
    -o work/baseline-tests/test_modem_rate
work/baseline-tests/test_modem_rate
