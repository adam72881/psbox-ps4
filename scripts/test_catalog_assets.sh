#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -DX4_ASSETS_HOST_TEST -O2 -g -Wall -Wextra -Werror \
  ${X4_TEST_CFLAGS:-} tests/catalog_assets_test.c src/ui/catalog_assets.c src/ui/lang.c src/ui/catalog_disk_cache.c src/ui/product_metadata.c \
  src/auth/json.c src/auth/public_image_policy.c -pthread -lm -o "$build_dir/catalog-assets-test"
"$build_dir/catalog-assets-test" tests/fixtures/public_product_displaycatalog.json \
  tests/fixtures/ui_synthetic_cover.jpg tests/fixtures/ui_synthetic_cover.png tests/fixtures/ui_color_layout.png
