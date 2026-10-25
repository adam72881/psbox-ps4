#!/usr/bin/env python3
"""Check actual scheduler metric recording with controlled host timestamps."""
import json
import sys
from pathlib import Path

stages = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))["stages"]
pad = stages["controller_read_interval"]
assert (pad["count"], pad["sum_us"], pad["mean_us"], pad["maximum_us"]) == (2, 4000, 2000, 3000)
flip = stages["flip_end_to_conversion_begin"]
assert (flip["count"], flip["sum_us"], flip["mean_us"], flip["maximum_us"]) == (2, 1200, 600, 700)
assert (flip["p50_upper_us"], flip["p95_upper_us"]) == (600, 800)
print("PASS actual scheduler pad mean/max and native flip-end delay p50/p95; invalid timestamps produce no sample")
