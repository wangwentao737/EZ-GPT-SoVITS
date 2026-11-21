"""Exercise dataset validation, fresh training runs, and synthesis with the new weights."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import uuid


def clone(source, target):
    source, target = Path(source), Path(target)
    try:
        os.link(source, target)
    except OSError:
        shutil.copyfile(source, target)


def identifier():
    return "ez_" + uuid.uuid4().hex


def run(program, engine, data, requests, expected):
    batch = data / f"batch-{uuid.uuid4().hex}.json"
    batch.write_text(json.dumps(requests, ensure_ascii=False), encoding="utf-8")
    before = 0
    database_path = data / "library.sqlite"
    if database_path.exists():
        with sqlite3.connect(database_path) as database:
            before = database.execute("SELECT COALESCE(MAX(rowid), 0) FROM jobs").fetchone()[0]
    handle = subprocess.run(
        [str(program), "--data-root", str(data), "--probe-engine", str(engine / "go-webui.bat"),
         "--probe-queue", "--probe-job", str(batch)],
        env={
            **os.environ, "QT_QPA_PLATFORM": "windows", "GRADIO_ANALYTICS_ENABLED": "False",
            "BROWSER": r"C:\Windows\System32\cmd.exe /d /c rem %s",
        },
        capture_output=True, encoding="utf-8", errors="replace", timeout=420,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    assert handle.returncode == expected, (handle.returncode, handle.stdout, handle.stderr, data)
    with sqlite3.connect(database_path) as database:
        return list(database.execute(
            "SELECT id, state, message, result FROM jobs WHERE rowid>? ORDER BY rowid", (before,)
        ))


def clone_dataset(source, target, missing=None, duplicate=False):
    target.mkdir()
    for name in ("2-name2text.txt", "6-name2semantic.tsv", "3-bert", "4-cnhubert", "5-wav32k", "ez-dataset.json"):
        old = source / name
        new = target / name
        if name == "6-name2semantic.tsv" and duplicate:
            content = old.read_text(encoding="utf-8").splitlines()
            new.write_text("\n".join([*content, content[1]]) + "\n", encoding="utf-8")
        elif old.is_dir():
            shutil.copytree(old, new, copy_function=clone, ignore=(
                (lambda _folder, filenames: [missing] if missing in filenames else [])
                if name == "4-cnhubert" and missing is not None else None
            ))
        elif old.is_file():
            clone(old, new)


def main():
    workspace = Path(__file__).resolve().parents[1]
    scratch = workspace / ".test"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=workspace / "out" / "portable" / "EZ-GPT-SoVITS.exe")
    parser.add_argument("--engine", type=Path, default=scratch / "engine-20261006")
    parser.add_argument("--prepared", type=Path, default=scratch / "training" / "prepare.json")
    parser.add_argument("--voice", type=Path, default=scratch / "synthesis" / "fixtures" / "synthesis.json")
    args = parser.parse_args()
    engine = args.engine.resolve(strict=True)
    fixture_path = args.prepared.resolve(strict=True)
    voice_path = args.voice.resolve(strict=True)
    assert all(path.is_relative_to(scratch) for path in (engine, fixture_path, voice_path))
    fixture = json.loads(fixture_path.read_text(encoding="utf-8-sig"))
    voice = json.loads(voice_path.read_text(encoding="utf-8-sig"))

    data = scratch / f"integrity-{uuid.uuid4().hex}"
    project = data / "projects" / "训练样本 (校验)"
    clips = project / "clips"
    clips.mkdir(parents=True)
    label_path = Path(fixture["annotation"]).resolve(strict=True)
    assert label_path.is_relative_to(scratch), label_path
    lines = []
    for line in label_path.read_text(encoding="utf-8-sig").splitlines():
        source, speaker, language, text = line.split("|", 3)
        origin = Path(source).resolve(strict=True)
        assert origin.is_relative_to(scratch), origin
        clip = clips / origin.name
        clone(origin, clip)
        lines.append(f"{clip.as_posix()}|{speaker}|{language}|{text}")
    annotation = project / "annotations.list"
    annotation.write_text("\n".join(lines) + "\n", encoding="utf-8")

    prepare = {**fixture, "operation": "prepare", "experiment": identifier(), "project": str(project),
               "source": str(clips), "annotation": str(annotation), "batch": 1, "epochs": 2}
    rows = run(args.app, engine, data, [prepare], expected=0)
    assert len(rows) == 1 and rows[0][1] == "succeeded", rows
    prepared = json.loads(rows[0][3])
    dataset = Path(prepared["dataset"])
    assert prepared["sample_count"] == len(lines)
    manifest = json.loads((dataset / "ez-dataset.json").read_text(encoding="utf-8"))
    assert set(manifest["samples"]) == {clip.name for clip in clips.glob("*.wav")}
    print(f"Real preparation: {prepared['sample_count']} samples and their required artifacts matched.", flush=True)

    missing_id, duplicate_id = identifier(), identifier()
    missing = next(iter(manifest["samples"])) + ".pt"
    clone_dataset(dataset, engine / "logs" / missing_id, missing=missing)
    clone_dataset(dataset, engine / "logs" / duplicate_id, duplicate=True)
    changed = project / "changed.list"
    changed.write_text(annotation.read_text(encoding="utf-8").replace("\n", " edited\n", 1), encoding="utf-8")

    # Leave a real earlier checkpoint in the prepared directory to detect accidental reuse.
    old_folder = engine / "logs" / fixture["experiment"] / "logs_s2_v5turbo_lora_32"
    old_checkpoint = next(old_folder.glob("G_*.pth"))
    old_copy = dataset / old_folder.name / old_checkpoint.name
    old_copy.parent.mkdir()
    clone(old_checkpoint, old_copy)
    old_stat = old_copy.stat()

    text_index = dataset / "2-name2text.txt"
    text_rows = text_index.read_text(encoding="utf-8").splitlines()
    text_index.write_text("\n".join(["", text_rows[0], "", *text_rows[1:], "", ""]), encoding="utf-8")

    train = {**prepare, "operation": "train_sovits"}
    rows = run(args.app, engine, data, [
        {**train, "experiment": missing_id},
        {**train, "experiment": duplicate_id},
        {**train, "annotation": str(changed)},
        train,
        {**train, "operation": "train_gpt"},
    ], expected=3)
    assert len(rows) == 5, rows
    for (job, state, message, _), expected in zip(rows[:3], (missing, "重复样本", "标注与预处理版本不同"), strict=True):
        assert state == "failed" and expected in message, (state, message)
        assert not (data / "jobs" / job / "training-run.json").exists()
    runs = []
    for job, state, message, result in rows[3:]:
        assert state == "succeeded", (state, message)
        output = json.loads(result)
        assert Path(output["dataset"]) == dataset and output["sample_count"] == len(lines)
        assert Path(output["checkpoints"]) != dataset
        assert Path(output["weight"]).is_file() and output["weight"].startswith(str(project))
        record = json.loads((data / "jobs" / job / "training-run.json").read_text(encoding="utf-8"))
        assert record["checkpoints"] == output["checkpoints"] and record["epochs"] == 2
        runs.append(output)
    assert runs[0]["checkpoints"] != runs[1]["checkpoints"]
    assert (old_copy.stat().st_size, old_copy.stat().st_mtime_ns) == (old_stat.st_size, old_stat.st_mtime_ns)
    print("Invalid datasets rejected; both stages trained with blank text-index lines in fresh directories.", flush=True)

    source = Path(voice["reference"]).resolve(strict=True)
    assert source.is_relative_to(scratch)
    reference = project / "reference.wav"
    clone(source, reference)
    rows = run(args.app, engine, data, [{**voice, "sovits": runs[0]["weight"], "gpt": runs[1]["weight"],
                                         "reference": str(reference), "voice_id": str(uuid.uuid4())}], expected=0)
    assert len(rows) == 1 and rows[0][1] == "succeeded", rows
    audio = json.loads(rows[0][3])
    assert hashlib.sha256(Path(audio["audio"]).read_bytes()).hexdigest() == audio["sha256"]
    print("The newly trained pair synthesized and archived a WAV with a matching checksum.", flush=True)


if __name__ == "__main__":
    main()
