"""Exercise migration with a v0.1.20 SQLite library and a copied portable data directory."""

import argparse
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile
import uuid
import wave

from workspace_paths import workspace_path


def launch(program, data, expected=0, extra=()):
    result = subprocess.run(
        [str(program), "--data-root", str(data), *(extra or ("--smoke-test",))],
        env={**os.environ, "QT_QPA_PLATFORM": "windows", "GRADIO_ANALYTICS_ENABLED": "False"},
        creationflags=subprocess.CREATE_NO_WINDOW,
        capture_output=True, encoding="utf-8", errors="replace", timeout=180,
    )
    assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
    return result


def write_wave(target):
    target.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(target), "wb") as stream:
        stream.setparams((1, 2, 16000, 0, "NONE", "not compressed"))
        stream.writeframes(b"\0\0" * 1600)
    payload = target.read_bytes()
    return {"bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}


def seed(baseline, old, external, launcher, fixture=None):
    launch(baseline, old)
    project = uuid.uuid4().hex
    voice = str(uuid.uuid4())
    job = str(uuid.uuid4())
    pending = str(uuid.uuid4())
    custom = str(uuid.uuid4())
    training = str(uuid.uuid4())
    path = old / "projects" / project
    clips = path / "clips" / "import"
    annotation = path / "labels.list"
    rows = [(clips / "clip-1.wav", "first"), (clips / "clip-2.wav", "second")]
    for clip, _ in rows:
        write_wave(clip)
    annotation.write_bytes("".join(f"{clip.as_posix()}|Speaker|en|{text}\n" for clip, text in rows).encode())
    project_record = {"id": project, "name": "搬迁验收", "directory": path.as_posix(),
        "clips": clips.as_posix(), "annotation": annotation.as_posix(), "family": "v5turbo",
        "speaker": "Speaker", "language": "en", "experiment": f"ez_{uuid.uuid4().hex}",
        "dataset": (launcher.parent / "logs" / "old-dataset").as_posix(),
        "engine": launcher.as_posix(), "pending": training}
    request = {"operation": "synthesize", "voice_id": voice, "text": "Test after relocating a voice."}
    if fixture:
        request.update(fixture)
        for key in ("gpt", "sovits", "reference"):
            source = Path(request[key])
            with source.open("rb") as handle:
                digest = hashlib.file_digest(handle, "sha256").hexdigest()
            folder = {"gpt": "models/v5turbo/GPT", "sovits": "models/v5turbo/SoVITS", "reference": "audio"}[key]
            target = old / "assets" / folder / (digest + source.suffix)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
            request[key] = target.as_posix()
    audio = old / "outputs" / voice / f"{job}.wav"
    ext_audio = external / voice / f"{custom}.wav"
    first_result = {"audio": audio.as_posix(), **write_wave(audio), "archive_version": 1}
    custom_result = {"audio": ext_audio.as_posix(), **write_wave(ext_audio)}
    pending_audio = old / "outputs" / voice / f"{pending}.wav"
    pending_result = {"audio": pending_audio.as_posix(), **write_wave(pending_audio), "archive_version": 1}
    receipt = old / "jobs" / pending / "completion.json"
    receipt.parent.mkdir(parents=True)
    receipt.write_text(json.dumps(pending_result), encoding="utf-8")
    with closing(sqlite3.connect(old / "library.sqlite")) as db, db:
        db.execute("INSERT INTO projects(id,name,state) VALUES(?,?,?)",
                   (project, project_record["name"], json.dumps(project_record).encode()))
        db.execute("INSERT INTO voices(id,name,settings) VALUES(?,?,?)",
                   (voice, "搬迁声音", json.dumps(request).encode()))
        for key, value in (("outputRoot", external.as_posix()), ("currentEngine", launcher.as_posix())):
            db.execute("INSERT INTO workspace_settings VALUES(?,?)", (key, value))
        for identifier, state, snapshot, result in (
            (job, "succeeded", {**request, "output_root": (old / "outputs").as_posix()}, first_result),
            (custom, "succeeded", {**request, "output_root": external.as_posix()}, custom_result),
            (pending, "running", {**request, "output_root": (old / "outputs").as_posix()}, None),
            (training, "queued", {**project_record, "operation": "train_sovits"}, None),
        ):
            db.execute("INSERT INTO jobs(id,created,launcher,operation,state,request,result) VALUES(?,?,?,?,?,?,?)",
                (identifier, "2026-11-23", launcher.as_posix(), snapshot["operation"], state,
                 json.dumps(snapshot).encode(), json.dumps(result).encode() if result else None))
    return project, rows, annotation, job, custom, pending, training, request


def check(program, old, new, seed_data, interrupt=False):
    project, rows, original, job, custom, pending, training, request = seed_data
    database = new / "library.sqlite"
    annotation = new / original.relative_to(old)
    expected = "".join(f"{(new / clip.relative_to(old)).as_posix()}|Speaker|en|{text}\n" for clip, text in rows).encode()
    migrated = annotation.with_name(f"relocated-{hashlib.sha256(expected).hexdigest()}.list")
    if interrupt:
        migrated.mkdir()
        launch(program, new, expected=1)
        with closing(sqlite3.connect(database)) as db:
            assert db.execute("PRAGMA user_version").fetchone() == (0,)
            record = json.loads(db.execute("SELECT state FROM projects WHERE id=?", (project,)).fetchone()[0])
            assert record["annotation"] == original.as_posix()
            assert db.execute("SELECT state FROM jobs WHERE id=?", (training,)).fetchone() == ("queued",)
        migrated.rmdir()
    launch(program, new)
    with closing(sqlite3.connect(database)) as db:
        assert db.execute("PRAGMA user_version").fetchone() == (1,)
        state = json.loads(db.execute("SELECT state FROM projects WHERE id=?", (project,)).fetchone()[0])
        assert workspace_path(new, state["annotation"]) == migrated
        assert migrated.read_bytes() == expected and annotation.read_bytes() != expected
        assert state["training_recovery"] and "dataset" not in state
        assert "dataset" in state["prior_training"]
        assert db.execute("SELECT state FROM jobs WHERE id=?", (training,)).fetchone() == ("interrupted",)
        assert db.execute("SELECT state FROM jobs WHERE id=?", (pending,)).fetchone() == ("succeeded",)
        for identifier in (job, custom, pending):
            receipt = json.loads(db.execute("SELECT result FROM jobs WHERE id=?", (identifier,)).fetchone()[0])
            path = workspace_path(new, receipt["audio"])
            if identifier == job:
                assert path.is_relative_to(old)
                path = new / path.relative_to(old)
            assert hashlib.sha256(path.read_bytes()).hexdigest() == receipt["sha256"]
            with wave.open(str(path), "rb") as audio:
                assert audio.getnframes() == 1600
        voice = json.loads(db.execute("SELECT settings FROM voices").fetchone()[0])
    assert list(new.glob("library-before-migration-*.sqlite"))
    before = set(new.glob("library-before-migration-*.sqlite"))
    launch(program, new)
    assert set(new.glob("library-before-migration-*.sqlite")) == before
    return voice


def main():
    workspace = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--portable", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, default=workspace / "out/portable-0.1.20")
    parser.add_argument("--live", action="store_true")
    parser.add_argument("--actual-rename", action="store_true")
    args = parser.parse_args()
    program = (args.portable / "EZ-GPT-SoVITS.exe").resolve(strict=True)
    baseline = (args.baseline / "EZ-GPT-SoVITS.exe").resolve(strict=True)
    scratch = workspace / ".test" / f"migration-{uuid.uuid4().hex}"
    scratch.mkdir()
    old = scratch / "original" / "data"
    launcher = workspace / ".test/engine-20261006/go-webui.bat"
    fixture = json.loads((workspace / ".test/synthesis/fixtures/synthesis.json").read_text(encoding="utf-8-sig")) if args.live else None
    seed_data = seed(baseline, old, scratch / "external", launcher, fixture)
    copied = scratch / "copied" / "data"
    shutil.copytree(old, copied)
    voice = check(program, old, copied, seed_data, interrupt=True)
    print("Same-drive copy, old copy still present, migration interruption and retry passed.", flush=True)

    with tempfile.TemporaryDirectory(prefix="ez-migration-") as system_tmp:
        cross_disk = Path(system_tmp) / "data"
        shutil.copytree(old, cross_disk)
        unavailable = old.with_name("unavailable") / "data"
        assert not unavailable.exists()
        with closing(sqlite3.connect(cross_disk / "library.sqlite")) as db, db:
            for table, column in (("projects", "state"), ("voices", "settings"), ("jobs", "request"), ("jobs", "result")):
                db.execute(f"UPDATE {table} SET {column}=CAST(REPLACE(CAST({column} AS TEXT), ?, ?) AS BLOB)",
                           (old.as_posix(), unavailable.as_posix()))
        for file in cross_disk.rglob("*"):
            if file.name in ("labels.list", "completion.json"):
                file.write_bytes(file.read_bytes().replace(old.as_posix().encode(), unavailable.as_posix().encode()))
        project, clips, annotation, *rest = seed_data
        absent_clips = [(unavailable / clip.relative_to(old), label) for clip, label in clips]
        absent_seed = (project, absent_clips, unavailable / annotation.relative_to(old), *rest)
        check(program, unavailable, cross_disk, absent_seed)
        print(f"Relocation {old.drive} -> {cross_disk.drive} with old data unavailable passed.", flush=True)
    if args.actual_rename:
        renamed = scratch / "renamed" / "data"
        renamed.parent.mkdir()
        assert old.resolve().is_relative_to(scratch.resolve()) and renamed.resolve().is_relative_to(scratch.resolve())
        old.rename(renamed)
        check(program, old, renamed, seed_data)
        print("Moving the original directory by same-drive rename passed.", flush=True)
    if fixture:
        for key in ("gpt", "sovits", "reference"):
            voice[key] = workspace_path(copied, voice[key]).as_posix()
        request = scratch / "synthesis.json"
        request.write_text(json.dumps(voice), encoding="utf-8")
        launch(program, copied, extra=("--probe-engine", launcher, "--probe-queue", "--probe-job", request))
        with closing(sqlite3.connect(copied / "library.sqlite")) as db:
            state, result = db.execute("SELECT state,result FROM jobs ORDER BY rowid DESC LIMIT 1").fetchone()
        assert state == "succeeded" and Path(json.loads(result)["audio"]).is_file()
        print("Real synthesis from relocated voice preset and external WAV destination passed.", flush=True)


if __name__ == "__main__":
    main()
