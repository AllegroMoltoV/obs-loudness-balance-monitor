"""Submitted/loopback PCM audit with the user-approved leading 10ms attenuation limitation.
All later PCM and interval coverage remain exact; no latency guarantee.

WASAPI flags: Windows SDK 10.0.26100.0 um/audioclient.h:199-201.
Device positions, not concatenated packet bytes, establish capture continuity.
"""
import argparse
import contextlib
import csv
import json
import math
import mmap
from pathlib import Path
import struct
import sys

FRAME = 8
CHUNK = 1024 * 1024
FLAGS = {1: "DATA_DISCONTINUITY", 2: "SILENT", 4: "TIMESTAMP_ERROR"}


def kv(path):
    return dict(line.split("=", 1) for line in path.read_text(encoding="utf-8-sig").splitlines()
                if "=" in line)


def rows(path):
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    return ([{k: int(v) for k, v in row.items()} for row in csv.DictReader(
        line for line in lines if not line.startswith("#"))], lines)


def unique_hit(data, needle):
    pos, hits = 0, []
    while len(hits) < 2:
        found = data.find(needle, pos)
        if found < 0:
            break
        pos = found + 1
        if found % FRAME == 0:
            hits.append(found // FRAME)
    return hits


def content_stats(data):
    first, last, nonfinite = None, None, 0
    for start in range(0, len(data), CHUNK):
        for index, (left, right) in enumerate(struct.iter_unpack("<ff", data[start:start + CHUNK])):
            frame = start // FRAME + index
            if not math.isfinite(left) or not math.isfinite(right):
                nonfinite += 1
            if left != 0 or right != 0:
                if first is None:
                    first = frame
                last = frame
    total = len(data) // FRAME
    return {"frames": total, "first_nonzero_frame": first, "last_nonzero_frame": last,
            "leading_zero_frames": first if first is not None else total,
            "trailing_zero_frames": total - last - 1 if last is not None else total,
            "nonfinite_frames": nonfinite}


def mismatch_diagnostic(submitted, captured, source_frame, capture_frame):
    # Bounded exact resynchronization is diagnostic only, never repairs evidence.
    window, anchor = 4096, 32
    source_end = min(len(submitted) // FRAME, source_frame + window)
    capture_end = min(len(captured) // FRAME, capture_frame + window)
    if source_frame + anchor <= len(submitted) // FRAME:
        needle = submitted[source_frame * FRAME:(source_frame + anchor) * FRAME]
        hit = captured.find(needle, (capture_frame + 1) * FRAME, capture_end * FRAME)
        if hit >= 0 and hit % FRAME == 0:
            return {"kind": "inserted_capture_frames_candidate",
                    "frames": hit // FRAME - capture_frame,
                    "interpretation": "extra PCM, repeated samples, or inserted silence; no repair applied"}
    if capture_frame + anchor <= len(captured) // FRAME:
        needle = captured[capture_frame * FRAME:(capture_frame + anchor) * FRAME]
        hit = submitted.find(needle, (source_frame + 1) * FRAME, source_end * FRAME)
        if hit >= 0 and hit % FRAME == 0:
            return {"kind": "missing_capture_frames_candidate",
                    "frames": hit // FRAME - source_frame, "repair_applied": False}
    return {"kind": "value_change_or_unresolved_edit", "resync_search_frames": window}


def compare_pcm(submitted, captured, startup_frames=0):
    a, b = content_stats(submitted), content_stats(captured)
    result = {"submitted": a, "loopback": b, "status": "unverified",
              "alignment": None, "all_submitted_pcm_equal": False}
    if a["nonfinite_frames"] or b["nonfinite_frames"]:
        result["reason"] = "nonfinite PCM"
        return result
    first = a["first_nonzero_frame"]
    if first is None or a["frames"] - first < 64:
        result["reason"] = "no nonzero 64-frame alignment anchor"
        return result
    first = max(first, startup_frames)
    needle = submitted[first * FRAME:(first + 64) * FRAME]
    hits = unique_hit(captured, needle)
    if len(hits) != 1 or unique_hit(submitted, needle) != [first]:
        result["reason"] = "exact stereo alignment absent or ambiguous; gain differences are not tolerated"
        swapped = b"".join(needle[i + 4:i + 8] + needle[i:i + 4]
                           for i in range(0, len(needle), FRAME))
        result["swapped_channel_anchor_hits"] = unique_hit(captured, swapped)
        return result
    offset = hits[0] - first
    result["alignment"] = {"capture_frame_for_submitted_zero": offset,
                           "anchor_submitted_frame": first, "anchor_capture_frame": hits[0],
                           "anchor_frames": 64}
    start, end = max(0, -offset), min(a["frames"], b["frames"] - offset)
    if end < start:
        result["reason"] = "aligned interval outside capture"
        return result
    unequal, first_bad = 0, None
    outside_startup = 0
    startup_attenuation_only = True
    for pos in range(start, end, CHUNK // FRAME):
        stop = min(end, pos + CHUNK // FRAME)
        left = submitted[pos * FRAME:stop * FRAME]
        right = captured[(pos + offset) * FRAME:(stop + offset) * FRAME]
        if left == right:
            continue
        for local in range(0, len(left), FRAME):
            if left[local:local + FRAME] != right[local:local + FRAME]:
                unequal += 1
                frame = pos + local // FRAME
                if frame >= startup_frames:
                    outside_startup += 1
                else:
                    reference = struct.unpack("<ff", left[local:local + FRAME])
                    actual = struct.unpack("<ff", right[local:local + FRAME])
                    startup_attenuation_only &= all(
                        x * y >= 0 and abs(y) <= abs(x) for x, y in zip(reference, actual))
                if first_bad is None:
                    first_bad = pos + local // FRAME
    missing_head, missing_tail = start, a["frames"] - end
    result.update({"compared_frames": end - start, "unequal_frames": unequal,
                   "unobserved_submitted_head_frames": missing_head,
                   "unobserved_submitted_tail_frames": missing_tail})
    if first_bad is not None:
        result["first_unequal_submitted_frame"] = first_bad
        result["diagnostic"] = mismatch_diagnostic(submitted, captured, first_bad, first_bad + offset)
    # Capture outside the submitted interval must be silent, but is never deleted.
    head_end = max(0, offset)
    tail_start = min(b["frames"], max(0, offset + a["frames"]))
    head = content_stats(captured[:head_end * FRAME])
    tail = content_stats(captured[tail_start * FRAME:])
    result["capture_outside_submitted"] = {"head": head, "tail": tail}
    exact = not unequal and not missing_head and not missing_tail
    result["all_submitted_pcm_equal"] = exact
    extras_silent = head["first_nonzero_frame"] is None and tail["first_nonzero_frame"] is None
    if exact and extras_silent:
        result["status"] = "exact_pcm"
    elif startup_frames and unequal and not outside_startup and startup_attenuation_only and not missing_head and not missing_tail and extras_silent:
        result["status"] = "accepted_startup_attenuation"
        result["known_limitation"] = {"approved_window_frames": startup_frames,
                                     "different_frames": unequal,
                                     "remainder_exact": True,
                                     "reason": "User accepted leading 10ms amplitude ramp for balance listening"}
    else:
        result["reason"] = "PCM mismatch, incomplete coverage, or nonzero audio outside submitted interval"
    return result


def audit_packets(events, captured, signal_first):
    expected, previous_end = 0, None
    issues, position_changes, flags_report = [], [], []
    for index, event in enumerate(events):
        first, frames = event["first_float"] // 2, event["frames"]
        flags, position = event["flags"], event["device_position_frames"]
        if event["first_float"] % 2 or first != expected or frames <= 0:
            issues.append({"packet": index, "reason": "invalid PCM offset or frame count"})
        if position < 0 or event["qpc_position_100ns"] < 0:
            issues.append({"packet": index, "reason": "negative capture position"})
        if previous_end is not None and position != previous_end:
            position_changes.append({"packet": index, "expected_device_position": previous_end,
                                     "actual_device_position": position, "delta_frames": position - previous_end,
                                     "capture_file_frame": first})
            issues.append({"packet": index, "reason": "device position gap/overlap"})
        if flags:
            startup = bool(flags & 1) and index == 0 and signal_first is not None and first + frames <= signal_first
            record = {"packet": index, "flags": flags,
                      "names": [name for bit, name in FLAGS.items() if flags & bit],
                      "unknown_bits": flags & ~7, "capture_file_frame": first,
                      "device_position_frames": position, "frames": frames,
                      "classification": "startup_before_signal" if startup else "captured_interval"}
            flags_report.append(record)
            if flags & ~7 or flags & 4 or (flags & 1 and not startup):
                issues.append({"packet": index, "reason": "unresolved discontinuity/timestamp/unknown flag"})
            if flags & 2:
                if content_stats(captured[first * FRAME:(first + frames) * FRAME])["first_nonzero_frame"] is not None:
                    issues.append({"packet": index, "reason": "SILENT flag with nonzero PCM"})
        if event["release_hresult"] & 0x80000000:
            issues.append({"packet": index, "reason": "failed capture ReleaseBuffer"})
        expected = first + frames
        previous_end = position + frames
    if expected * FRAME != len(captured):
        issues.append({"reason": "capture CSV/PCM size mismatch"})
    return {"issues": issues, "device_position_changes": position_changes, "all_nonzero_flags": flags_report,
            "packets": len(events), "csv_frames": expected,
            "startup_policy": "only first packet DATA_DISCONTINUITY wholly before nonzero signal is classified startup"}


def selftest():
    # In-memory fixtures only: no output files, audio, build, or hardware access.
    signal = b"".join(struct.pack("<ff", (i + 1) / 4096, -(i + 3) / 8192) for i in range(512))
    silence = bytes(80 * FRAME)
    submitted = signal + silence
    capture = silence + submitted + silence
    base = compare_pcm(submitted, capture)
    assert base["status"] == "exact_pcm"
    assert base["alignment"]["capture_frame_for_submitted_zero"] == 80
    inserted = capture[:280 * FRAME] + signal[199 * FRAME:200 * FRAME] + capture[280 * FRAME:]
    deletion = capture[:280 * FRAME] + capture[281 * FRAME:]
    for value, kind in ((inserted, "inserted_capture_frames_candidate"),
                        (deletion, "missing_capture_frames_candidate")):
        test = compare_pcm(submitted, value)
        assert test["status"] == "unverified" and test["unequal_frames"] > 0
        assert test["diagnostic"]["kind"] == kind
    stretched = b"".join(signal[i:i + FRAME] * 2 for i in range(0, len(signal), FRAME))
    assert compare_pcm(submitted, silence + stretched + silence)["status"] == "unverified"
    swap = b"".join(capture[i + 4:i + 8] + capture[i:i + 4] for i in range(0, len(capture), FRAME))
    assert compare_pcm(submitted, swap)["swapped_channel_anchor_hits"]
    ramp = b"".join(struct.pack("<ff", left * i / 32, right * i / 32)
                    for i, (left, right) in enumerate(struct.iter_unpack("<ff", signal[:32 * FRAME])))
    attenuated = silence + ramp + submitted[32 * FRAME:] + silence
    assert compare_pcm(submitted, attenuated, 32)["status"] == "accepted_startup_attenuation"
    later_damage = bytearray(attenuated)
    later_damage[(80 + 100) * FRAME:(80 + 101) * FRAME] = bytes(FRAME)
    assert compare_pcm(submitted, later_damage, 32)["status"] == "unverified"
    amplified = silence + struct.pack("<ff", 1.0, -1.0) + submitted[FRAME:] + silence
    assert compare_pcm(submitted, amplified, 32)["status"] == "unverified"
    assert compare_pcm(submitted, attenuated[:500 * FRAME], 32)["status"] == "unverified"
    event = lambda first, count, pos, flag=0: dict(first_float=first * 2, frames=count,
        device_position_frames=pos, flags=flag, release_hresult=0, qpc_position_100ns=pos * 200)
    good = [event(0, 80, 100, 3), event(80, len(capture) // FRAME - 80, 180)]
    assert not audit_packets(good, capture, 80)["issues"]
    bad = [dict(e) for e in good]
    bad[1]["device_position_frames"] += 1
    assert audit_packets(bad, capture, 80)["device_position_changes"][0]["delta_frames"] == 1
    bad = [dict(e) for e in good]
    bad[1]["flags"] = 1
    assert audit_packets(bad, capture, 80)["issues"]
    assert compare_pcm(submitted, capture[:500 * FRAME])["unobserved_submitted_tail_frames"] > 0
    print(json.dumps({"selftest": "passed", "cases": [
        "leading/trailing silence", "sample insertion/duplication", "sample deletion",
        "stretch", "channel swap", "CSV device-position gap", "startup flag",
        "interior discontinuity", "capture tail truncation"]}))



def audit_directory(directory):
    root = Path(directory)
    ui = json.loads((root / "result.json").read_text(encoding="utf-8-sig"))
    result = {"ui_passed": ui.get("passed") is True, "passed": False, "captures": {},
              "limitations": ["WASAPI loopback measures the OS endpoint, not physical or acoustic output",
                             "Reference rate conversion uses the same libobs resampler as playback"]}
    for prefix in sorted(root.glob("*-loopback-meta.txt")):
        name = prefix.name.removesuffix("-loopback-meta.txt")
        meta = kv(prefix)
        pcm = (root / (name + "-loopback.f32")).read_bytes()
        expected = (root / (name + "-expected.f32")).read_bytes() if (root / (name + "-expected.f32")).exists() else b""
        events, _ = rows(root / (name + "-loopback.csv"))
        issues = []
        for field in ("dropped_frames", "dropped_packets", "failures", "timestamp_error_packets"):
            if int(meta[field]):
                issues.append(field + " nonzero")
        if meta.get("failure") or int(meta["last_hresult"]) & 0x80000000:
            issues.append("capture failure")
        if len(pcm) % FRAME or int(meta["stored_frames"]) * FRAME != len(pcm):
            issues.append("capture size mismatch")
        if int(meta["stored_packets"]) != len(events):
            issues.append("capture packet count mismatch")
        stats = content_stats(pcm)
        signal_first = stats["first_nonzero_frame"]
        if signal_first is None:
            signal_first = len(pcm) // FRAME
        packets = audit_packets(events, pcm, signal_first)
        if packets["issues"] or packets["device_position_changes"]:
            issues.append("capture packet continuity failed")
        item = {"metadata": meta, "packets": packets, "issues": issues, "passed": False}
        if name in ("waiting-stopped", "output-failure"):
            item["silence"] = stats
            item["passed"] = not issues and stats["first_nonzero_frame"] is None
        elif name == "playback-stopped":
            item["interrupted_observation_only"] = True
            item["silence"] = stats
        elif name.startswith("playback-"):
            item["comparison"] = compare_pcm(expected, pcm, int(meta["rate"]) // 100)
            item["passed"] = not issues and item["comparison"]["status"] in ("exact_pcm", "accepted_startup_attenuation")
        elif name == "recording-interrupted":
            # A cancelled playback intentionally lacks the tail. Keep full expected PCM
            # and report observed coverage without accepting it as a full playback.
            item["comparison"] = compare_pcm(expected, pcm)
            item["interrupted_observation_only"] = True
        else:
            item["issues"].append("unexpected/incomplete capture")
        result["captures"][name] = item
    captures = result["captures"]
    required = ["playback-1"]
    observations = ui.get("observations", {})
    if observations.get("preview_waiting_stopped"):
        required += ["waiting-stopped", "playback-2"]
        if not observations.get("preview_rerecord_different_audio"):
            result["reason"] = "different-audio rerecord verification missing"
            return result
        required += ["playback-3"]
    if observations.get("preview_case") == "output-failure":
        if not (observations.get("preview_output_failure_restored") and
                observations.get("preview_output_failure_retry_started") and
                observations.get("preview_output_failure_message")):
            result["reason"] = "required output failure and retry observations missing"
            return result
        required += ["output-failure"]
    result["required_captures"] = required
    result["passed"] = result["ui_passed"] and all(name in captures and captures[name]["passed"] for name in required)
    if observations.get("preview_recording_start_interrupt"):
        result["passed"] = False
        result["reason"] = "Interruption evidence is observational; this audit does not certify partial output"
    return result


def main():
    parser = argparse.ArgumentParser(description="Audit product preview PCM against independently captured final mix.")
    parser.add_argument("result_dir", nargs="?")
    parser.add_argument("output_json", nargs="?")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        selftest()
        return 0
    if not args.result_dir or not args.output_json:
        parser.error("result_dir and output_json are required")
    try:
        result = audit_directory(args.result_dir)
    except (OSError, ValueError, KeyError, OverflowError, struct.error, csv.Error) as error:
        result = {"passed": False, "reason": type(error).__name__ + ": " + str(error)}
    with Path(args.output_json).open("x", encoding="utf-8") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"passed": result["passed"], "output": args.output_json}))
    return 0 if result["passed"] else 2


if __name__ == "__main__":
    sys.exit(main())
