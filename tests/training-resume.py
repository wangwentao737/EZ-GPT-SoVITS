"""Interrupt both v5turbo trainers on a completed epoch and restore their states."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import sqlite3
import subprocess
import time
import uuid

from workspace_paths import workspace_path
import zipfile


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def interrupt(program, engine, data, request):
    marker = data / f"cancel-{request['operation']}"
    spec = data / f"interrupt-{request['operation']}.json"
    spec.write_text(json.dumps(request), encoding="utf-8")
    database = data / "library.sqlite"
    with sqlite3.connect(database) as connection:
        before = connection.execute("SELECT COALESCE(MAX(rowid), 0) FROM jobs").fetchone()[0]
    arguments = [
        str(program), "--data-root", str(data), "--probe-engine", str(engine / "go-webui.bat"),
        "--probe-queue", "--probe-job", str(spec), "--probe-cancel-file", str(marker),
    ]
    environment = {
        **os.environ, "QT_QPA_PLATFORM": "windows", "GRADIO_ANALYTICS_ENABLED": "False",
        "BROWSER": r"C:\Windows\System32\cmd.exe /d /c rem %s",
    }
    log = data / (spec.stem + ".log")
    with log.open("wb") as output, subprocess.Popen(
        arguments, env=environment, stdout=output, stderr=subprocess.STDOUT,
        creationflags=subprocess.CREATE_NO_WINDOW,
    ) as desktop:
        try:
            deadline = time.monotonic() + 330
            while desktop.poll() is None and time.monotonic() < deadline:
                with sqlite3.connect(database) as connection:
                    active = connection.execute(
                        "SELECT id FROM jobs WHERE rowid>? AND state='running'", (before,)
                    ).fetchone()
                if active:
                    job_id = active[0]
                    record = data / "jobs" / job_id / "training-run.json"
                    if record.is_file():
                        attempt = json.loads(record.read_text(encoding="utf-8"))
                        assert attempt["resume_signature"], attempt
                        run = Path(attempt["checkpoints"]).resolve()
                        assert run.is_relative_to(engine / "logs")
                        if request["operation"] == "train_sovits":
                            files = run.glob("logs_s2_v5turbo_lora_32/G_233333333333.pth")
                        else:
                            files = run.glob("logs_s1_v5turbo/ckpt/epoch=*-step=*.ckpt")
                        checkpoint = next((file for file in files if zipfile.is_zipfile(file)), None)
                        if checkpoint:
                            marker.touch()
                            assert desktop.wait(timeout=35) == 0, log
                            break
                time.sleep(0.1)
            else:
                raise AssertionError(f"No completed checkpoint during {request['operation']}. See {log} and {data}")
        finally:
            if desktop.poll() is None:
                desktop.kill()
    with sqlite3.connect(database) as connection:
        row = connection.execute("SELECT state,result FROM jobs WHERE id=?", (job_id,)).fetchone()
    assert row[0] == "cancelled" and not json.loads(row[1]), row
    assert zipfile.is_zipfile(checkpoint), checkpoint
    print(f"{request['operation']}: cancelled after a complete checkpoint ({checkpoint.stat().st_size} bytes).",
          flush=True)
    return job_id, checkpoint, run


def main():
    workspace = Path(__file__).resolve().parents[1]
    scratch = (workspace / ".test").resolve(strict=True)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=workspace / "out/portable/EZ-GPT-SoVITS.exe")
    parser.add_argument("--engine", type=Path, default=scratch / "engine-20261006")
    parser.add_argument("--prepared", type=Path, default=scratch / "training/prepare.json")
    parser.add_argument("--voice", type=Path, default=scratch / "synthesis/fixtures/synthesis.json")
    args = parser.parse_args()
    run = runpy.run_path(str(workspace / "tests/training-integrity.py"))["run"]
    shared = runpy.run_path(str(workspace / "tests/live-cancellation.py"))
    test_path, clone_file = shared["test_path"], shared["clone_file"]
    program = args.app.resolve(strict=True)
    engine = test_path(args.engine, scratch)
    fixture = json.loads(test_path(args.prepared, scratch).read_text(encoding="utf-8-sig"))
    data = scratch / f"resume-{uuid.uuid4().hex}"
    project = data / "projects" / "断点续训 (测试)"
    clips = project / "clips"
    clips.mkdir(parents=True)
    labels = []
    for line in test_path(fixture["annotation"], scratch).read_text(encoding="utf-8-sig").splitlines():
        source, speaker, language, text = line.split("|", maxsplit=3)
        source = test_path(source, scratch)
        clip = clips / source.name
        clone_file(source, clip)
        labels.append(f"{clip.as_posix()}|{speaker}|{language}|{text}")
    annotation = project / "labels.list"
    annotation.write_text("\n".join(labels) + "\n", encoding="utf-8")
    prepared = {**fixture, "operation": "prepare", "experiment": "ez_" + uuid.uuid4().hex,
                "project": str(project), "source": str(clips), "annotation": str(annotation),
                "batch": 1, "epochs": 3}
    [result] = run(program, engine, data, [prepared], expected=0)
    assert result[1] == "succeeded", result
    print(f"Prepared {len(labels)} synthetic training samples.", flush=True)

    weights = {}
    for operation in ("train_sovits", "train_gpt"):
        request = {**prepared, "operation": operation}
        job, checkpoint, original_run = interrupt(program, engine, data, request)
        original_digest = sha256(checkpoint)
        resumed = {**request, "resume_job": job}
        [invalid] = run(program, engine, data, [{**resumed, "batch": 2}], expected=3)
        assert invalid[1] == "failed" and "训练设置、数据或引擎文件已变更" in invalid[2], invalid
        assert not (data / "jobs" / invalid[0] / "training-run.json").exists()
        [restored] = run(program, engine, data, [resumed], expected=0)
        assert restored[1] == "succeeded", restored
        result = json.loads(restored[3])
        assert 0 < result["resume_epoch"] < resumed["epochs"], result
        assert Path(result["checkpoints"]) != original_run
        weight = workspace_path(data, result["weight"])
        assert weight.is_file() and weight.is_relative_to(project)
        assert sha256(checkpoint) == original_digest
        weights[operation.removeprefix("train_")] = weight.as_posix()
        receipt = json.loads((data / "jobs" / restored[0] / "training-run.json").read_text(encoding="utf-8"))
        assert receipt["resume_job"] == job and receipt["resume_epoch"] == result["resume_epoch"]
        print(f"{operation}: restored from epoch {result['resume_epoch']}; completed epoch {resumed['epochs']}. "
              "Changed parameters rejected and original checkpoint preserved.", flush=True)

    voice = json.loads(test_path(args.voice, scratch).read_text(encoding="utf-8-sig"))
    source = test_path(voice["reference"], scratch)
    reference = project / ("reference" + source.suffix)
    clone_file(source, reference)
    [audio] = run(program, engine, data, [{**voice, **weights, "reference": str(reference),
                                           "voice_id": str(uuid.uuid4())}], expected=0)
    assert audio[1] == "succeeded", audio
    archived = json.loads(audio[3])
    assert sha256(workspace_path(data, archived["audio"])) == archived["sha256"]
    print("Both resumed weights synthesized and archived a verified WAV.", flush=True)


if __name__ == "__main__":
    main()
