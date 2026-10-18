#!/usr/bin/env python3
"""Concatenate path.part00, path.part01, ... into path (for large files split for GitHub MCP)."""
import sys
from pathlib import Path

def assemble(target: str) -> None:
    t = Path(target)
    parts = sorted(t.parent.glob(t.name + ".part*"))
    if not parts:
        raise SystemExit(f"no parts for {target}")
    data = "".join(p.read_text(encoding="utf-8") for p in parts)
    t.write_text(data, encoding="utf-8")
    print(f"wrote {t} ({len(data)} bytes) from {len(parts)} parts")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(f"usage: {sys.argv[0]} <relative-path>")
    assemble(sys.argv[1])
