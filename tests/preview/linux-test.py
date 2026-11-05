import array
import json
import pathlib
import signal
import subprocess
import sys
import time

exe = str(pathlib.Path(sys.argv[1]).resolve())
results = []


def wait_for_capture(capture, predicate):
    deadline = time.monotonic() + 12
    while not predicate():
        if capture.poll() is not None:
            raise RuntimeError("capture exited during observation")
        if time.monotonic() >= deadline:
            raise TimeoutError("capture did not reach the observation point")
        time.sleep(.01)


def run_case(rate, sink, path, data, mode):
    out = pathlib.Path(f"{rate}-{mode}.f32")
    with out.open("wb") as output:
        capture = subprocess.Popen(
            ["parec", "--raw", "--format=float32le", f"--rate={rate}",
             "--channels=2", f"--device={sink}.monitor"], stdout=output)
        try:
            wait_for_capture(capture, lambda: out.stat().st_size >= 8192)
            args = [exe, "--play", str(path), str(rate), sink]
            if mode == "early-cancel":
                args.append("1000")
            with subprocess.Popen(args) as player:
                try:
                    if mode == "cancel":
                        wait_for_capture(capture, lambda: data[:8192] in out.read_bytes())
                        assert player.poll() is None, "playback ended before cancellation"
                        player.send_signal(signal.SIGTERM)
                    code = player.wait(timeout=25)
                finally:
                    if player.poll() is None:
                        player.kill()
            # Observe another full clip duration so queued data cannot be
            # mistaken for a successfully cancelled clip.
            target = out.stat().st_size + len(data)
            wait_for_capture(capture, lambda: out.stat().st_size >= target)
        finally:
            capture.terminate()
            capture.wait(timeout=5)

    audio = out.read_bytes()
    frames = memoryview(audio).cast("Q")  # One stereo float32 frame per word.
    first = next((i for i, frame in enumerate(frames) if frame), 0)
    end = next((i + 1 for i in range(len(frames)-1, first-1, -1) if frames[i]), first)
    played = audio[first*8:end*8]
    assert played == data[:len(played)], "observed signal differs from the original prefix"
    if mode == "natural":
        assert code == 0 and played == data, "incomplete natural playback"
    else:
        assert code == 3 and len(played) < len(data), "cancellation failed"
        if mode == "cancel":
            assert len(played) >= 8192, "no confirmed playback before cancellation"
    assert len(frames)-end >= rate//4, "no observed silent tail"
    return dict(rate=rate, mode=mode, code=code, played_frames=len(played)//8,
                trailing_silent_frames=len(frames)-end)


for rate in (44100, 48000):
    sink = f"lbm_probe_{rate}"
    module = subprocess.check_output(
        ["pactl", "load-module", "module-null-sink", f"sink_name={sink}",
         "format=float32le", f"rate={rate}", "channels=2"], text=True).strip()
    try:
        data = array.array("f", ((i % 8191 + 1)/4194304 for i in range(rate*5*2))).tobytes()
        path = pathlib.Path(f"input-{rate}.f32")
        path.write_bytes(data)
        for mode in ("natural", "early-cancel", "cancel"):
            row = run_case(rate, sink, path, data, mode)
            results.append(row)
            print(json.dumps(row), flush=True)
            pathlib.Path("linux-results.json").write_text(json.dumps(results, indent=2))
        invalid = subprocess.run([exe, "--play", str(path), str(rate), "lbm_missing_sink"], timeout=15)
        assert invalid.returncode == 2, "missing sink did not fail"
    finally:
        subprocess.run(["pactl", "unload-module", module], check=True)
