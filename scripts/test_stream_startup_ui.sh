#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
"${PYTHON:-python3}" -B tests/stream_startup_ui_test.py
