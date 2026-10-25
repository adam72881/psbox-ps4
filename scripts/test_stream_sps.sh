#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc=${CC:-clang}
"$cc" -std=c11 -O2 -Wall -Wextra -Werror -Werror=vla -Werror=alloca "$root/tests/stream_sps_test.c" "$root/src/video/stream_sps.c" -o "$out/sps"
"$out/sps"
"$cc" -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "$root/tests/stream_sps_test.c" "$root/src/video/stream_sps.c" -o "$out/sps-san"
ASAN_OPTIONS=detect_leaks=1 "$out/sps-san"
"$cc" -std=c11 -O2 -Wall -Wextra -Werror -Werror=vla -Werror=alloca "$root/tests/stream_sps_fixture_cli.c" "$root/src/video/stream_sps.c" -o "$out/sps-fixture"
python3 "$root/tests/stream_sps_real_test.py" "$out/sps-fixture"
"$cc" -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "$root/tests/stream_sps_fixture_cli.c" "$root/src/video/stream_sps.c" -o "$out/sps-fixture-san"
ASAN_OPTIONS=detect_leaks=1 python3 "$root/tests/stream_sps_real_test.py" "$out/sps-fixture-san"
