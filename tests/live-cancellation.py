"""Stop real stage subprocesses in the independent test engine."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import time
import uuid


def test_path(value, scratch):
    path = Path(value).resolve(strict=True)
    if not path.is_relative_to(scratch):
        raise ValueError(f"Test engine and inputs must be in the workspace .test directory: {path}")
    return path


def clone_file(source, destination):
    try:
        os.link(source, destination)
    except OSError:
        shutil.copyfile(source, destination)


def children(engine, script):
    environment = {
        **os.environ,
        "EZ_TEST_ENGINE_PYTHON": str(engine / "runtime" / "python.exe"),
        "EZ_TEST_PROCESS_SCRIPT": script,
    }
    command = (
        'Get-CimInstance Win32_Process -Filter "Name = \'python.exe\'" | '
        'Where-Object { $_.ExecutablePath -eq $env:EZ_TEST_ENGINE_PYTHON -and '
        '$_.CommandLine -like ("*" + $env:EZ_TEST_PROCESS_SCRIPT + "*") } | '
        'ForEach-Object { $_.ProcessId }'
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
        env=environment, capture_output=True, text=True, check=True,
        creationflags=subprocess.CREATE_NO_WINDOW, timeout=12,
    )
    return {int(line) for line in result.stdout.split()}


def slice_request(data, reference):
    project = data / "projects" / "切片 (取消)"
    clips = project / "original"
    clips.mkdir(parents=True)
    for index in range(64):
        clone_file(reference, clips / f"recording_{index:03d}.wav")
    return {
        "operation": "slice", "project": str(project), "source": str(clips),
        "destination": str(project / "slice-output"), "family": "v5turbo",
    }


def training_request(data, engine, prepared):
    experiment = "ez_" + uuid.uuid4().hex
    previous = engine / "logs" / prepared["experiment"]
    temporary = engine / "logs" / experiment
    temporary.mkdir()
    for name in ("2-name2text.txt", "6-name2semantic.tsv", "3-bert", "4-cnhubert", "5-wav32k"):
        source = previous / name
        target = temporary / name
        if source.is_dir():
            shutil.copytree(source, target, copy_function=clone_file)
        else:
            clone_file(source, target)
    project = data / "projects" / "训练 (取消)"
    project.mkdir(parents=True)
    return {
        "operation": "train_sovits", "project": str(project), "experiment": experiment,
        "family": prepared["family"], "gpu": prepared["gpu"], "batch": 1, "epochs": 20,
    }


def run_case(program, engine, data, request, voice, needle):
    marker = data / "test-cancel"
    spec = data / "test-batch.json"
    spec.write_text(json.dumps([request, voice]), encoding="utf-8")
    arguments = [
        str(program), "--data-root", str(data),
        "--probe-engine", str(engine / "go-webui.bat"), "--probe-queue",
        "--probe-job", str(spec), "--probe-cancel-file", str(marker),
    ]
    environment = {
        **os.environ, "QT_QPA_PLATFORM": "windows", "GRADIO_ANALYTICS_ENABLED": "False",
        "BROWSER": r"C:\Windows\System32\cmd.exe /d /c rem %s",
    }
    earlier = children(engine, "webui.py")
    assert not earlier, f"Expected the independent engine to be stopped: {earlier}"
    log = data / "desktop.log"
    with log.open("wb") as output:
        with subprocess.Popen(arguments, env=environment, stdout=output, stderr=subprocess.STDOUT,
                              creationflags=subprocess.CREATE_NO_WINDOW) as desktop:
            try:
                deadline = time.monotonic() + 130
                while not (active := children(engine, needle)):
                    if desktop.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(f"No live {needle} subprocess. See {log}")
                    time.sleep(0.15)
                marker.touch()
                status = desktop.wait(timeout=150)
                assert status == 0, f"Desktop exit {status}. See {log} and job logs under {data}"
            finally:
                if desktop.poll() is None:
                    desktop.kill()
    assert not active.intersection(children(engine, needle)), f"Cancelled subprocess is still active: {active}"

    with sqlite3.connect(data / "library.sqlite") as database:
        rows = database.execute("SELECT id,operation,state,message,result FROM jobs ORDER BY rowid").fetchall()
    stopped, continued = rows
    assert stopped[1:3] == (request["operation"], "cancelled") and not json.loads(stopped[4]), stopped
    assert continued[1:3] == ("synthesize", "succeeded"), continued
    result = json.loads(continued[4])
    wav = Path(result["audio"])
    assert wav.stat().st_size == result["bytes"]
    assert hashlib.sha256(wav.read_bytes()).hexdigest() == result["sha256"]
    assert sorted((data / "outputs").rglob("*.wav")) == [wav]
    assert not (data / "jobs" / stopped[0] / "completion.json").exists()
    if request["operation"] == "synthesize":
        for kind, extension in (("GPT", "ckpt"), ("SoVITS", "pth")):
            alias = "ez_" + stopped[0].replace("-", "") + "." + extension
            assert not (engine / f"{kind}_weights_{request['family']}" / alias).exists(), alias
    print(f"{request['operation']}: stopped child {sorted(active)}, {stopped[3]}, "
          f"queued synthesis archived {result['bytes']} bytes.", flush=True)


def main():
    workspace = Path(__file__).resolve().parents[1]
    scratch = workspace / ".test"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=workspace / "out" / "portable" / "EZ-GPT-SoVITS.exe")
    parser.add_argument("--engine", type=Path, default=scratch / "engine-20261006")
    parser.add_argument("--synthesis", type=Path, default=scratch / "synthesis" / "fixtures" / "synthesis.json")
    parser.add_argument("--prepared", type=Path, default=scratch / "training" / "train_sovits.json")
    parser.add_argument("--case", action="append", choices=("slice", "train_sovits", "synthesize"))
    args = parser.parse_args()
    engine = test_path(args.engine, scratch)
    voice = json.loads(test_path(args.synthesis, scratch).read_text(encoding="utf-8-sig"))
    prepared = json.loads(test_path(args.prepared, scratch).read_text(encoding="utf-8-sig"))
    needles = {"slice": "slice_audio.py", "train_sovits": "s2_train_v3_lora.py",
               "synthesize": "inference_webui_fast.py"}
    for case in args.case or needles:
        data = scratch / f"cancel-{case}-{uuid.uuid4().hex}"
        data.mkdir()
        copied_voice = {**voice, "voice_id": str(uuid.uuid4())}
        for role in ("gpt", "sovits", "reference"):
            source = test_path(voice[role], scratch)
            destination = data / (role + source.suffix)
            clone_file(source, destination)
            copied_voice[role] = str(destination)
        if case == "slice":
            request = slice_request(data, Path(copied_voice["reference"]))
        elif case == "train_sovits":
            request = training_request(data, engine, prepared)
        else:
            request = {**copied_voice, "voice_id": str(uuid.uuid4()), "text": copied_voice["text"] * 20}
        run_case(args.app.resolve(strict=True), engine, data, request, copied_voice, needles[case])


if __name__ == "__main__":
    main()
