#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf -- "$out"' EXIT
for mode in plain sanitized; do
 flags=(-std=gnu2x -O1 -g -Wall -Wextra -Werror -pthread -DX4_STREAM_PROFILE=1)
 if [[ $mode == sanitized ]]; then flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer -no-pie); fi
 "${CC:-gcc}" "${flags[@]}" tests/stream_request_profile_test.c src/streaming/stream_profile.c src/auth/json.c -o "$out/$mode"
 ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$out/$mode" | tee "$out/$mode-public-contracts.log"
 "${PYTHON:-python3}" -B tests/stream_request_document_test.py --constructor-log "$out/$mode-public-contracts.log"
done
