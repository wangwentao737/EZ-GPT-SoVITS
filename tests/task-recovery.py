"""Exercise startup recovery through the packaged desktop and SQLite store."""

import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import uuid
import wave


def start(program, data):
    subprocess.run(
        [str(program), "--data-root", str(data), "--smoke-test"],
        env={**os.environ, "QT_QPA_PLATFORM": "windows"},
        creationflags=subprocess.CREATE_NO_WINDOW,
        timeout=30,
        check=True,
    )


def interrupted_synthesis(database, data, variant):
    job_id = str(uuid.uuid4())
    output_root = data.with_name(data.name + "-external") if variant == "custom_output" else data / "outputs"
    target = output_root / f"{job_id}.wav"
    temporary = data / "jobs" / job_id
    temporary.mkdir(parents=True)
    target.parent.mkdir(exist_ok=True)
    wave_path = temporary / "verified.wav" if variant == "before_move" else target
    with wave.open(str(wave_path), "wb") as stream:
        stream.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
        stream.writeframes(b"\0\0" * 1600)
    content = wave_path.read_bytes()
    receipt = {"audio": target.as_posix(), "duration": 0.1}
    if variant != "legacy_truncated":
        receipt.update(bytes=len(content), sha256=hashlib.sha256(content).hexdigest())
    if variant == "changed":
        wave_path.write_bytes(content[:-1] + b"\xff")
    elif variant == "legacy_truncated":
        wave_path.write_bytes(content[:-50])
    (temporary / "completion.json").write_text(json.dumps(receipt), encoding="utf-8")
    request = {"text": "snapshot"}
    if variant == "custom_output":
        request["output_root"] = output_root.as_posix()
    database.execute(
        "INSERT INTO jobs(id,created,launcher,operation,state,request) VALUES(?,?,?,?,?,?)",
        (job_id, "2026-01-01", "fixture", "synthesize", "running", json.dumps(request).encode()),
    )
    return job_id


def main():
    workspace = Path(__file__).resolve().parents[1]
    default = workspace / "out" / "portable" / "EZ-GPT-SoVITS.exe"
    program = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else default
    data = workspace / ".test" / f"recovery-{uuid.uuid4().hex}"
    start(program, data)
    database = sqlite3.connect(data / "library.sqlite")
    variants = ("complete", "custom_output", "before_move", "changed", "legacy_truncated")
    jobs = {variant: interrupted_synthesis(database, data, variant) for variant in variants}

    project_id = uuid.uuid4().hex
    project_dir = data / "projects" / project_id
    project_dir.mkdir(parents=True)
    stage = str(uuid.uuid4())
    weight = project_dir / "completed.pth"
    weight.write_bytes(b"successful stage fixture")
    project = {
        "id": project_id, "name": "Recovery fixture", "directory": project_dir.as_posix(),
        "dataset": "last successful dataset", "pending": stage, "family": "v5turbo",
    }
    database.execute(
        "INSERT INTO projects(id,name,state) VALUES(?,?,?)",
        (project_id, project["name"], json.dumps(project).encode()),
    )
    database.execute(
        "INSERT INTO jobs(id,created,launcher,operation,state,request,result) VALUES(?,?,?,?,?,?,?)",
        (stage, "2026-01-01", "fixture", "train_sovits", "succeeded",
         json.dumps({"id": project_id}).encode(), json.dumps({"weight": weight.as_posix()}).encode()),
    )
    database.commit()

    start(program, data)
    for variant, job_id in jobs.items():
        state, request, result = database.execute(
            "SELECT state,request,result FROM jobs WHERE id=?", (job_id,)
        ).fetchone()
        assert json.loads(request)["text"] == "snapshot"
        if variant in ("complete", "custom_output"):
            assert state == "succeeded" and Path(json.loads(result)["audio"]).is_file(), (variant, state)
        else:
            assert state == "interrupted" and result is None, (variant, state, result)
    recovered = json.loads(database.execute("SELECT state FROM projects WHERE id=?", (project_id,)).fetchone()[0])
    assert recovered["sovits"] == weight.as_posix() and "pending" not in recovered
    assert recovered["dataset"] == project["dataset"] and "gpt" not in recovered
    assert database.execute("SELECT COUNT(*) FROM jobs WHERE operation='train_gpt'").fetchone()[0] == 0

    interrupted = str(uuid.uuid4())
    recovered["pending"] = interrupted
    database.execute("UPDATE projects SET state=? WHERE id=?", (json.dumps(recovered).encode(), project_id))
    database.execute(
        "INSERT INTO jobs(id,created,launcher,operation,state,request) VALUES(?,?,?,?,?,?)",
        (interrupted, "2026-01-01", "fixture", "train_gpt", "running", b'{"stage":"GPT"}'),
    )
    database.commit()

    start(program, data)
    state, request = database.execute("SELECT state,request FROM jobs WHERE id=?", (interrupted,)).fetchone()
    assert state == "interrupted" and json.loads(request) == {"stage": "GPT"}
    restored = json.loads(database.execute("SELECT state FROM projects WHERE id=?", (project_id,)).fetchone()[0])
    assert restored["sovits"] == recovered["sovits"] and "pending" not in restored and "gpt" not in restored
    database.close()
    print("Recovery passed: default and custom output roots, incomplete/corrupt WAV, and completed/interrupted training stages.")


if __name__ == "__main__":
    main()
