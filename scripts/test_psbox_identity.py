#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""PSBox packaging identity gates (no console / no PKGTool)."""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from package_identity import metadata, check_flags, validate_ids  # noqa: E402

identity = metadata("psbox")
assert identity["title"] == "PSBox Cloud Gaming"
assert identity["title_id"] == "XCLD00001"
assert identity["content_id"] == "IV0000-XCLD00001_00-PSBOXCLOUD000001"
assert identity["version"] == "01.57"
assert identity["package_name"] == "PSBox-PS4-01.57.pkg"
assert identity["stream_profile"] == 1
validate_ids(identity["title_id"], identity["content_id"])
check_flags(
    identity,
    dict(
        X4_RECEIVER_NACK=1,
        X4_NACK_GRACE_MS=45,
        X4_RUMBLE=1,
        X4_INGRESS_MODE_BASELINE=0,
        X4_VIDEO_PROBE=0,
        X4_VIDEO_CAPACITY_1080=1,
        X4_STREAM_PROFILE=1,
        X4_PUBLIC=1,
    ),
)
icon = ROOT / "assets" / "icon0.png"
assert icon.is_file() and icon.stat().st_size > 100 and icon.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
exit_c = ROOT / "src" / "core" / "psbox_exit.c"
assert exit_c.is_file() and b"_exit(0)" in exit_c.read_bytes()
print(json.dumps({"result": "PASS", "package": identity["package_name"], "scope": "identity/icon/exit presence only"}))
