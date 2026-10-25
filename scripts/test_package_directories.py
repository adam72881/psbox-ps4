#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the exact packaging XML code without invoking a PS4 installer."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parent.parent
script = (repo / "scripts/empaquetar.sh").read_text(encoding="utf-8")
body = script.split("    python3 - <<'PY'\n", 1)[1].split("\nPY\n", 1)[0]
fixtures = [
    ("missing-assets", '<rootdir><dir targ_name="sce_sys" /></rootdir>', True),
    ("self-closing-assets", '<rootdir><dir targ_name="assets" /></rootdir>', True),
    ("existing-nested-fonts", '<rootdir><dir targ_name="assets"><dir targ_name="fonts" /><dir targ_name="audio" /></dir></rootdir>', True),
    ("existing-licenses", '<rootdir><dir targ_name="licenses" /><dir targ_name="assets"><dir targ_name="fonts" /></dir></rootdir>', True),
    ("duplicate-assets", '<rootdir><dir targ_name="assets" /><dir targ_name="assets" /></rootdir>', False),
    ("duplicate-fonts", '<rootdir><dir targ_name="assets"><dir targ_name="fonts" /><dir targ_name="fonts" /></dir></rootdir>', False),
    ("missing-rootdir", '', False),
    ("duplicate-rootdir", '<rootdir /><rootdir />', False),
]
if len(sys.argv) > 1:
    fixtures.append(("actual-sdk-project", Path(sys.argv[1]).read_text(), True))
checks = 0
for name, source, expected in fixtures:
    if name != "actual-sdk-project":
        source = '<psproject><files><file targ_path="assets/fonts/noto_sans.x4font" orig_path="assets/fonts/noto_sans.x4font" /></files>' + source + '</psproject>'
    with tempfile.TemporaryDirectory(prefix="x4-gp4-test-") as directory:
        path = Path(directory) / "xcloud4.gp4"
        path.write_text(source, encoding="utf-8")
        before_files = [dict(n.attrib) for n in ET.fromstring(source).iter("file")]
        result = subprocess.run([sys.executable, "-c", body], cwd=directory, capture_output=True)
        assert (result.returncode == 0) == expected, (name, result.stderr.decode())
        checks += 1
        if expected:
            first = path.read_bytes()
            tree = ET.fromstring(first)
            roots = list(tree.iter("rootdir"))
            assert len(roots) == 1
            assert len(roots[0].findall("dir[@targ_name='assets']")) == 1
            assert len(roots[0].findall("dir[@targ_name='assets']/dir[@targ_name='fonts']")) == 1
            assert len(roots[0].findall("dir[@targ_name='licenses']")) == 1
            assert [dict(n.attrib) for n in tree.iter("file")] == before_files
            checks += 5
            again = subprocess.run([sys.executable, "-c", body], cwd=directory, capture_output=True)
            assert again.returncode == 0 and path.read_bytes() == first, name
            checks += 1
print(json.dumps({"cases": len(fixtures), "checks": checks, "result": "PASS", "scope": "Exact GP4 directory transform, not console installation"}))
