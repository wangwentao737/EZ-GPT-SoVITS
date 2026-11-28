"""Verify owned inference reuse and compare it with the v0.1.20 portable build."""

import argparse
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import time
import uuid
import wave

from workspace_paths import workspace_path


def stop_owned_inference(engine, request):
    code = """
import pathlib, psutil, sys
engine = pathlib.Path(sys.argv[1]).resolve()
expected = engine / "runtime" / "python.exe"
parent = psutil.Process(int(sys.argv[2]))
if pathlib.Path(parent.exe()).resolve() != expected:
    sys.exit(1)
for child in parent.children(recursive=True):
    try:
        if pathlib.Path(child.exe()).resolve() == expected and any(
            pathlib.Path(arg).name == "inference_webui_fast.py" for arg in child.cmdline()
        ):
            child.kill()
            sys.exit(0)
    except psutil.NoSuchProcess:
        pass
sys.exit(1)
"""
    result = subprocess.run([str(engine / "runtime/python.exe"), "-I", "-B", "-c", code,
                             str(engine), str(request["webui_pid"])], creationflags=subprocess.CREATE_NO_WINDOW,
                            timeout=12, check=False)
    return result.returncode == 0


def run(program, engine, scratch, fixture, voices, name, terminate_row=None):
    data = scratch / name
    data.mkdir()
    inputs = {}
    for key in ("gpt", "sovits", "reference"):
        source = Path(fixture[key])
        target = data / (key + source.suffix)
        try:
            os.link(source, target)
        except OSError:
            shutil.copyfile(source, target)
        inputs[key] = target.as_posix()
    batch = str(uuid.uuid4())
    requests = [{**fixture, **inputs, "voice_id": voice, "batch_id": batch, "batch_index": index,
                 "batch_size": len(voices), "text": f"Hello, test line {index}.",
                 "line_id": str(uuid.uuid4()), "line_revision": str(uuid.uuid4()), "line_order": index}
                for index, voice in enumerate(voices, 1)]
    snapshot = data / "test.json"
    snapshot.write_text(json.dumps(requests), encoding="utf-8")
    database_path = data / "library.sqlite"
    weights = [engine / ("GPT_weights_" + fixture["family"]), engine / ("SoVITS_weights_" + fixture["family"])]
    copied = {}
    first = None
    terminated = False
    with (data / "desktop.log").open("wb") as log:
        started = time.monotonic()
        args = [program, "--data-root", data, "--probe-engine", engine / "go-webui.bat",
                "--probe-queue", "--probe-job", snapshot]
        with subprocess.Popen(list(map(str, args)), stdout=log, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW,
                env={**os.environ, "QT_QPA_PLATFORM": "windows", "GRADIO_ANALYTICS_ENABLED": "False"}) as desktop:
            try:
                while desktop.poll() is None:
                    elapsed = time.monotonic() - started
                    if elapsed > 1200:
                        raise TimeoutError(f"The synthesis batch timed out. See {data}")
                    if elapsed < 1 or not database_path.is_file():
                        time.sleep(0.15)
                        continue
                    try:
                        with closing(sqlite3.connect(database_path, timeout=0.1)) as db:
                            states = db.execute("SELECT id,state FROM jobs ORDER BY rowid").fetchall()
                    except sqlite3.OperationalError:
                        time.sleep(0.15)
                        continue
                    if states and states[0][1] == "succeeded" and first is None:
                        first = elapsed
                    if terminate_row is not None and not terminated and len(states) > terminate_row:
                        job, state = states[terminate_row]
                        path = data / "jobs" / job
                        if state == "running" and (path / "progress.json").is_file():
                            progress = json.loads((path / "progress.json").read_text(encoding="utf-8"))
                            if progress.get("phase") == "生成音频":
                                terminated = stop_owned_inference(engine,
                                    json.loads((path / "request.json").read_text(encoding="utf-8")))
                    if all(state not in ("running", "queued") for _, state in states) and states:
                        break
                    for index, suffix in enumerate((".ckpt", ".pth")):
                        for job, state in states:
                            if state != "running":
                                continue
                            target = weights[index] / ("ez_" + job.replace("-", "") + suffix)
                            try:
                                status = target.stat()
                                if status.st_nlink == 1:
                                    copied[target] = max(status.st_size, copied.get(target, 0))
                            except FileNotFoundError:
                                pass
                    time.sleep(0.15)
                assert desktop.wait(timeout=30) == (3 if terminate_row is not None else 0), data
            finally:
                if desktop.poll() is None:
                    desktop.kill()
    elapsed = time.monotonic() - started
    with closing(sqlite3.connect(database_path)) as db:
        rows = db.execute("SELECT id,state,request,result FROM jobs ORDER BY rowid").fetchall()
    assert len(rows) == len(voices) and first is not None, (data, rows)
    results = []
    for index, (job, state, request, result) in enumerate(rows):
        assert json.loads(request)["voice_id"] == voices[index]
        if index == terminate_row:
            assert terminated and state == "failed" and not (data / "jobs" / job / "completion.json").exists(), (data, job, state)
            continue
        assert state == "succeeded", (data, job, state)
        result = json.loads(result)
        audio = workspace_path(data, result["audio"])
        if name == "baseline":
            assert audio.name == job + ".wav"
        else:
            assert re.fullmatch(r"synth-[0-9]{12}(?:-[0-9]+)?\.wav", audio.name)
            assert result["archive_job"] == job
        assert audio.parent.parent.name == voices[index]
        assert hashlib.sha256(audio.read_bytes()).hexdigest() == result["sha256"]
        with wave.open(str(audio), "rb") as stream:
            assert stream.getnframes() > 0
        for index, suffix in enumerate((".ckpt", ".pth")):
            assert not (weights[index] / ("ez_" + job.replace("-", "") + suffix)).exists()
        results.append(result)
    assert len({result["audio"] for result in results}) == len(results)
    log = (data / "engine-start.log").read_text(encoding="utf-8", errors="replace")
    launches = sum("Running on local URL:" in line and line.rstrip().endswith(":9872") for line in log.splitlines())
    metrics = {"first_seconds": round(first, 2), "batch_seconds": round(elapsed, 2),
               "starts": launches, "weight_bytes_copied": sum(copied.values())}
    print(f"{name}: {json.dumps(metrics)}", flush=True)
    return results, metrics


def retry_script_line(program, engine, data, results):
    snapshots = json.loads((data / "test.json").read_bytes())
    old = snapshots[1]
    edited = {**old, "text": "The middle line was rewritten.", "line_revision": str(uuid.uuid4()),
              "batch_id": str(uuid.uuid4()), "batch_index": 1, "batch_size": 1}
    path = data / "edited-line.json"
    path.write_text(json.dumps(edited), encoding="utf-8")
    args = [program, "--data-root", data, "--probe-engine", engine / "go-webui.bat", "--probe-queue", "--probe-job", path]
    subprocess.run(list(map(str, args)), check=True, creationflags=subprocess.CREATE_NO_WINDOW, timeout=180,
                   env={**os.environ, "QT_QPA_PLATFORM": "windows", "GRADIO_ANALYTICS_ENABLED": "False"})
    with closing(sqlite3.connect(data / "library.sqlite")) as db:
        rows = db.execute("SELECT state,request,result FROM jobs ORDER BY rowid").fetchall()
    assert len(rows) == len(snapshots) + 1 and all(row[0] == "succeeded" for row in rows), rows
    newest = json.loads(rows[-1][1])
    assert newest["line_id"] == old["line_id"] and newest["line_revision"] != old["line_revision"]
    assert newest["line_order"] == 2 and newest["voice_id"] == old["voice_id"]
    newest_result = json.loads(rows[-1][2])
    audio = workspace_path(data, newest_result["audio"])
    assert audio.parent.parent.name == old["voice_id"] and hashlib.sha256(audio.read_bytes()).hexdigest() == newest_result["sha256"]
    for result in results:
        previous = workspace_path(data, result["audio"])
        assert previous != audio and hashlib.sha256(previous.read_bytes()).hexdigest() == result["sha256"]
    print("Real A/B/A synthesis and middle-line revision passed; all three original WAVs are unchanged.", flush=True)


def main():
    workspace = Path(__file__).resolve().parents[1]
    scratch_root = workspace / ".test"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--portable", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, default=workspace / "out/portable-0.1.20")
    parser.add_argument("--rows", type=int, default=20)
    parser.add_argument("--compare", action="store_true")
    parser.add_argument("--baseline-only", action="store_true")
    parser.add_argument("--faults-only", action="store_true")
    parser.add_argument("--script-only", action="store_true")
    parser.add_argument("--engine", type=Path, default=scratch_root / "engine-20261006")
    args = parser.parse_args()
    engine = args.engine.resolve(strict=True)
    assert engine.is_relative_to(scratch_root), "Use a disposable engine copy."
    assert args.rows >= 2
    fixture = json.loads((scratch_root / "synthesis/fixtures/synthesis.json").read_text(encoding="utf-8-sig"))
    scratch = scratch_root / f"inference-{uuid.uuid4().hex}"
    scratch.mkdir()
    print(f"Test data: {scratch}", flush=True)
    program = (args.portable / "EZ-GPT-SoVITS.exe").resolve(strict=True)
    a, b = (str(uuid.uuid4()) for _ in range(2))
    if args.script_only:
        results, metrics = run(program, engine, scratch, fixture, [a, b, a], "script")
        assert metrics["starts"] == 3
        retry_script_line(program, engine, scratch / "script", results)
        return
    if args.faults_only:
        results, metrics = run(program, engine, scratch, fixture, [a, a, a], "exited-inference", terminate_row=1)
        assert metrics["starts"] == 2 and not results[-1]["inference_reused"]
        return
    if not args.baseline_only:
        results, metrics = run(program, engine, scratch, fixture, [a] * args.rows, "reuse")
        assert metrics["starts"] == 1 and [row["inference_reused"] for row in results] == [False] + [True] * (args.rows - 1)
        assert metrics["weight_bytes_copied"] == sum(row["weight_bytes_copied"] for row in results)
        results, metrics = run(program, engine, scratch, fixture, [a, a, b, a], "switch")
        assert metrics["starts"] == 3 and [row["inference_reused"] for row in results] == [False, True, False, False]
    if args.compare or args.baseline_only:
        baseline = (args.baseline / "EZ-GPT-SoVITS.exe").resolve(strict=True)
        _, metrics = run(baseline, engine, scratch, fixture, [a] * args.rows, "baseline")
        assert metrics["starts"] == args.rows


if __name__ == "__main__":
    main()
