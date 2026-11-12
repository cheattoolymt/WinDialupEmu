#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p work/windows-models
cc=${CC:-gcc}
"$cc" -std=c99 -Wall -Wextra -Werror -D_WIN32 -Itests/fake-route -Iinclude \
 tests/test_routeguard.c src/net/vm_routeguard.c -o work/windows-models/routeguard
work/windows-models/routeguard
"$cc" -std=c99 -Wall -Wextra -Werror -D_WIN32 -ffunction-sections \
 -Itests/fake-serial -Iinclude -Isrc/serial tests/test_serial_wait.c \
 src/serial/vm_serial_win32.c -Wl,--gc-sections -o work/windows-models/serial_wait
work/windows-models/serial_wait
"$cc" -std=c99 -Wall -Wextra -Werror -Iinclude tests/test_rate.c src/net/vm_rate.c -o work/windows-models/rate
work/windows-models/rate
"$cc" -std=c99 -Wall -Wextra -Werror -Iinclude tests/test_rate_congestion.c src/net/vm_rate.c -o work/windows-models/rate_congestion
work/windows-models/rate_congestion
