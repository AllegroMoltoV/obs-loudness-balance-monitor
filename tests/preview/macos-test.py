import array
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import time

exe = str(pathlib.Path(sys.argv[1]).resolve())
uid = subprocess.check_output([exe, "--apple-virtual-output-uid"], text=True, timeout=10).strip()
if not uid or "\n" in uid:
    raise RuntimeError("No unique explicit virtual output UID")
print(f"Apple Virtual Sound Device output UID={uid}", flush=True)
results = []
with tempfile.TemporaryDirectory() as directory:
    for rate in (44100, 48000):
        frames = rate * 5
        data = array.array("f", (float(i % 8191 + 1) / 4194304 for i in range(frames * 2)))
        pcm = pathlib.Path(directory) / "stereo.f32"
        pcm.write_bytes(data.tobytes())
        for mode in ("natural", "cancel"):
            args = [exe, "--play", str(pcm), str(rate), uid]
            if mode == "cancel":
                args.append("1000")
            start = time.monotonic()
            played = subprocess.run(args, capture_output=True, text=True, timeout=30)
            elapsed = time.monotonic() - start
            print(rate, mode, played.returncode, played.stdout, played.stderr, flush=True)
            record = dict(rate=rate, mode=mode, uid=uid, elapsed=elapsed,
                          code=played.returncode, stdout=played.stdout, stderr=played.stderr)
            results.append(record)
            pathlib.Path("macos-results.json").write_text(json.dumps(results, indent=2))
            match = re.search(r"submitted_bytes=(\d+) expected_bytes=(\d+) audioqueue_status=(-?\d+)", played.stdout)
            if not match:
                raise RuntimeError(record)
            submitted, expected, status = map(int, match.groups())
            if expected != frames * 8 or status or submitted % 8:
                raise RuntimeError(record)
            if mode == "natural":
                ok = played.returncode == 0 and submitted == expected and "result=completed" in played.stdout
            else:
                # More than the three primed buffers proves an output callback refilled at runtime.
                ok = played.returncode == 3 and 3 * 4096 * 8 < submitted < expected and "result=cancelled" in played.stdout
            if not ok:
                raise RuntimeError(record)
