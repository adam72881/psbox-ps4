#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror \
  ${X4_TEST_CFLAGS:-} tests/catalog_disk_cache_test.c src/ui/catalog_disk_cache.c \
  src/ui/product_metadata.c src/auth/public_image_policy.c src/auth/json.c -lm \
  -o "$build_dir/catalog-disk-cache-test"
"$build_dir/catalog-disk-cache-test" tests/fixtures/ui_synthetic_cover.jpg
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -DX4_DISK_CACHE_NATIVE_FS_TEST \
  -O2 -g -Wall -Wextra -Werror ${X4_TEST_CFLAGS:-} tests/catalog_disk_cache_test.c \
  src/ui/catalog_disk_cache.c src/ui/product_metadata.c src/auth/public_image_policy.c \
  src/auth/json.c -lm -o "$build_dir/catalog-disk-cache-native-fs-test"
"$build_dir/catalog-disk-cache-native-fs-test" tests/fixtures/ui_synthetic_cover.jpg
