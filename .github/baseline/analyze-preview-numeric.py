#!/usr/bin/env python3
"""Compare published product analysis with independently mixed OBS category PCM."""
import argparse
import csv
import io
import json
import math
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument("result_dir", type=Path)
p.add_argument("reference", type=Path)
p.add_argument("output", type=Path)
a = p.parse_args()
r = a.result_dir
ui = json.loads((r / "result.json").read_text(encoding="utf-8"))
product = json.loads((r / "numeric-product.json").read_text(encoding="utf-8"))
run = subprocess.run([str(a.reference), str(r / "numeric-voice.f32"), str(r / "numeric-bgm.f32")],
                     text=True, capture_output=True, check=True)
(r / "numeric-reference.csv").write_text(run.stdout, encoding="utf-8")
rows = list(csv.DictReader(io.StringIO(run.stdout)))
mean = rows.pop()
errors = []
maximum = 0.0
maximum_peak = 0.0

def number(actual, expected, location, is_peak=False):
    global maximum, maximum_peak
    expected = float(expected)
    if not math.isfinite(expected):
        if actual is not None:
            errors.append(location + ": expected unavailable")
        return
    if actual is None or not math.isfinite(actual):
        errors.append(location + ": expected finite")
        return
    difference = abs(actual - expected)
    if is_peak:
        maximum_peak = max(maximum_peak, difference)
    else:
        maximum = max(maximum, difference)
    # Same libebur128 primitive, independent input path and aggregation.
    if difference > 1e-6:
        errors.append(location + ": numerical mismatch")

points = product["points"]
if len(points) != len(rows):
    errors.append("point count mismatch")
for i, (actual, expected) in enumerate(zip(points, rows)):
    for key in ("frame", "status", "mix_status", "clip_status"):
        if actual[key] != int(expected[key]):
            errors.append(f"point {i} {key} mismatch")
    if actual["active"] != bool(int(expected["active"])):
        errors.append(f"point {i} VAD mismatch")
    for key in ("voice", "bgm", "mix", "voice_peak", "bgm_peak", "mix_peak"):
        number(actual[key], expected[key], f"point {i} {key}", key.endswith("_peak"))
for key in ("voice", "bgm", "mix", "voice_peak", "bgm_peak", "mix_peak"):
    number(product[key], mean[key], "mean " + key, key.endswith("_peak"))
for key in ("valid_points", "mix_valid_points", "status", "mix_status", "clip_status"):
    if product[key] != int(mean[key]):
        errors.append("mean " + key + " mismatch")
if not any(not x["active"] and x["frame"] > 144000 for x in points):
    errors.append("speech pause was not exercised")
if not any(x["active"] and x["frame"] > 48000 * 8 for x in points):
    errors.append("speech resumption was not exercised")
if not ui["passed"]:
    errors.append("product UI scenario failed")
result = {"passed": not errors, "points": len(points), "valid_points": product["valid_points"],
          "mix_valid_points": product["mix_valid_points"],
          "maximum_lufs_difference": maximum, "maximum_peak_db_difference": maximum_peak, "errors": errors,
          "reference": "OBS raw category tracks + libebur128 primitives; independent VAD, float mix, averaging, interval/whole-recording peaks and rating"}
with a.output.open("x", encoding="utf-8") as output:
    json.dump(result, output, indent=2)
print(json.dumps(result))
raise SystemExit(0 if result["passed"] else 2)
