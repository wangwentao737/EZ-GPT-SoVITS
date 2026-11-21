"""Verify portable startup and persistent synthesis destinations with real audio."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import uuid


def run(program, *arguments):
    subprocess.run(
        [str(program), *map(str, arguments)],
        env={
            **os.environ, "QT_QPA_PLATFORM": "windows", "GRADIO_ANALYTICS_ENABLED": "False",
            "BROWSER": r"C:\Windows\System32\cmd.exe /d /c rem %s",
        },
        creationflags=subprocess.CREATE_NO_WINDOW,
        check=True, timeout=150,
    )


def copy_input(source, destination):
    try:
        os.link(source, destination)
    except OSError:
        shutil.copyfile(source, destination)


def last_wave(database, home):
    state, request, result = database.execute(
        "SELECT state,request,result FROM jobs ORDER BY rowid DESC LIMIT 1"
    ).fetchone()
    assert state == "succeeded", state
    assert Path(json.loads(request)["output_root"]).resolve() == home.resolve()
    result = json.loads(result)
    wave = Path(result["audio"]).resolve(strict=True)
    assert wave.is_relative_to(home.resolve()), wave
    assert hashlib.sha256(wave.read_bytes()).hexdigest() == result["sha256"]
    return wave


def main():
    workspace = Path(__file__).resolve().parents[1]
    scratch = workspace / ".test"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--portable", type=Path, default=workspace / "out" / "portable")
    parser.add_argument("--engine", type=Path, default=scratch / "engine-20261006")
    parser.add_argument("--synthesis", type=Path, default=scratch / "synthesis" / "fixtures" / "synthesis.json")
    args = parser.parse_args()
    engine = args.engine.resolve(strict=True)
    source = args.synthesis.resolve(strict=True)
    assert engine.is_relative_to(scratch) and source.is_relative_to(scratch), "Use the isolated test fixtures."
    test_root = scratch / f"paths-{uuid.uuid4().hex}"
    portable = test_root / "portable"
    shutil.copytree(args.portable, portable, ignore=shutil.ignore_patterns("data", "outputs"))
    program = portable / "EZ-GPT-SoVITS.exe"
    data = portable / "data"
    data.mkdir()
    database_path = data / "library.sqlite"
    # A blank portable database isolates the check from any legacy AppData registry entry.
    database_path.touch()
    run(program, "--smoke-test")

    fixture = json.loads(source.read_text(encoding="utf-8-sig"))
    for role in ("gpt", "sovits", "reference"):
        path = Path(fixture[role]).resolve(strict=True)
        assert path.is_relative_to(scratch), path
        local = data / (role + path.suffix)
        copy_input(path, local)
        fixture[role] = local.as_posix()
    request = data / "test-synthesis.json"
    request.write_text(json.dumps({**fixture, "voice_id": str(uuid.uuid4())}), encoding="utf-8")
    options = ("--probe-engine", engine / "go-webui.bat", "--probe-queue", "--probe-job", request)

    run(program, *options)
    with sqlite3.connect(database_path) as database:
        previous = last_wave(database, data / "outputs")
        custom = test_root / "生成 输出 (设置)"
        custom.mkdir()
        database.execute("INSERT INTO workspace_settings(key,value) VALUES(?,?)",
                         ("outputRoot", custom.as_posix()))

    run(program, *options)
    with sqlite3.connect(database_path) as database:
        current = last_wave(database, custom)
        history = json.loads(database.execute("SELECT result FROM jobs ORDER BY rowid LIMIT 1").fetchone()[0])
        assert Path(history["audio"]).resolve(strict=True) == previous
        assert current != previous
    print("Portable launch, bundled default WAV directory, custom directory, and original history passed.")


if __name__ == "__main__":
    main()
