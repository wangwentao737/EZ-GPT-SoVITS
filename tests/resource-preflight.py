"""Check resource failures against an isolated simulator and the real test engine."""

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import uuid

import zipfile

from workspace_paths import workspace_path


def invoke(program, data, launcher, requests=None, expected=0):
    args = [str(program), "--data-root", str(data), "--probe-engine", str(launcher)]
    if requests is not None:
        batch = data / "test-requests.json"
        batch.write_text(json.dumps(requests), encoding="utf-8")
        args.extend(["--probe-queue", "--probe-job", str(batch)])
    process = subprocess.run(
        args, capture_output=True, encoding="utf-8", errors="replace", timeout=150,
        creationflags=subprocess.CREATE_NO_WINDOW,
        env={**os.environ, "QT_QPA_PLATFORM": "windows", "GRADIO_ANALYTICS_ENABLED": "False",
             "BROWSER": r"C:\Windows\System32\cmd.exe /d /c rem %s"},
    )
    assert process.returncode == expected, (process.returncode, process.stdout, process.stderr, data)
    return process


def check_startup(program, workspace):
    engine = workspace / "incomplete-engine"
    engine.mkdir()
    launcher = engine / "go-webui.bat"
    launcher.write_text("@echo unexpected>started.txt\n", encoding="ascii")
    result = invoke(program, workspace / "startup", launcher, expected=1)
    assert "runtime/python.exe" in result.stderr.replace("\\", "/"), result.stderr
    assert not (engine / "started.txt").exists()
    print("Startup: missing bundled Python identified before the launcher ran.", flush=True)


def check_batch_startup_failures(program, workspace):
    engine = workspace / "batch-startup-engine"
    engine.mkdir()
    launcher = engine / "go-webui.bat"
    launcher.write_text("@echo deliberate startup failure\n@exit /b 7\n", encoding="ascii")
    requests = [{"operation": "synthesize", "text": f"Line {index}"} for index in range(1, 4)]

    for case, match in (("missing-runtime", "runtime/python.exe"), ("early-exit", "引擎启动超时或已经退出")):
        if case == "early-exit":
            (engine / "runtime").mkdir()
            for name in ("runtime/python.exe", "webui.py", "config.py"):
                (engine / name).write_text("Startup-only test fixture.", encoding="ascii")
        data = workspace / case
        data.mkdir()
        invoke(program, data, launcher, requests, expected=3)
        with sqlite3.connect(data / "library.sqlite") as database:
            rows = database.execute("SELECT state,request,message,result FROM jobs ORDER BY rowid").fetchall()
        assert len(rows) == 3, rows
        for (state, request, message, result), original in zip(rows, requests, strict=True):
            assert state == "failed" and match in message.replace("\\", "/"), (state, message)
            assert json.loads(request)["text"] == original["text"]
            assert json.loads(result) == {}
        assert not (data / "jobs").exists()

    data = workspace / "invalid-batch"
    data.mkdir()
    invoke(program, data, launcher, [requests[0], {**requests[1], "output_root": "relative/output"}], expected=1)
    with sqlite3.connect(data / "library.sqlite") as database:
        assert database.execute("SELECT COUNT(*) FROM jobs").fetchone()[0] == 0
    assert not (data / "engine-start.log").exists()
    print("Batch: immediate and asynchronous startup failures reached all three jobs; invalid submission rolled back.",
          flush=True)


def check_interface_isolation(program, workspace):
    engine = workspace / "partial-interfaces"
    runtime = engine / "runtime"
    runtime.mkdir(parents=True)
    (runtime / "python.exe").write_bytes(b"exists for preflight only")
    (engine / "config.py").write_text("# simulator startup fixture\n", encoding="ascii")
    contract = json.loads((Path(__file__).resolve().parents[1] / "engine/20261006-contract.json").read_bytes())
    config = {"version": "4.45.0", "components": [], "dependencies": []}
    info = {"named_endpoints": {}}
    for method, spec in contract["main"].items():
        ids = []
        for name, component, schema, label in spec["inputs"]:
            ids.append(len(config["components"]) + 1)
            config["components"].append({"id": ids[-1], "type": component.lower(), "props": {"label": label}})
        config["dependencies"].append({"api_name": method, "inputs": ids})
        info["named_endpoints"]["/" + method] = {
            "parameters": [{"parameter_name": name, "component": widget, "type": {"type": schema}}
                           for name, widget, schema, _ in spec["inputs"]],
            "returns": [{"component": widget, "type": {"type": schema}} for widget, schema in spec["returns"]],
        }

    simulator = '''import subprocess
import sys
import webbrowser
import config
from pathlib import Path
from http.server import BaseHTTPRequestHandler, HTTPServer
controller = webbrowser.get()
assert controller.args == ["--engine-browser-notification", "%s"]
assert controller.open("http://localhost:19874/")
assert controller.open("http://localhost:19872/")
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path not in ("/config", "/info"):
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(Path(self.path[1:] + ".json").read_bytes())
    def do_POST(self):
        Path("unexpected-post.txt").touch()
        self.send_error(405)
HTTPServer(("127.0.0.1", config.webui_port_main), Handler).serve_forever()
'''
    (engine / "webui.py").write_text(simulator, encoding="ascii")
    launcher = engine / "go-webui.bat"
    launcher.write_text(f'@"{sys.executable}" -B -u "%~dp0webui.py"\n', encoding="utf-8")
    (engine / "config.py").write_text("webui_port_main = 19874\nwebui_port_infer_tts = 19872\n", encoding="ascii")

    for case, operation, affected, evidence in (
        ("new-version-and-port", None, None, None),
        ("missing-synthesis", "synthesize", "语音合成", "change_tts_inference_1"),
        ("reordered-training", "train_sovits", "SoVITS 训练", "open1Ba"),
        ("changed-return", "train_gpt", "GPT 训练", "open1Bb"),
    ):
        current_config = copy.deepcopy(config)
        current_info = copy.deepcopy(info)
        if case == "missing-synthesis":
            dependency = next(dep for dep in current_config["dependencies"] if dep["api_name"] == evidence)
            dependency["api_name"] += "_renamed"
        elif case == "reordered-training":
            ids = next(dep["inputs"] for dep in current_config["dependencies"] if dep["api_name"] == evidence)
            ids[1], ids[2] = ids[2], ids[1]
        elif case == "changed-return":
            current_info["named_endpoints"]["/" + evidence]["returns"][0]["type"]["type"] = "number"
        (engine / "config.json").write_text(json.dumps(current_config), encoding="utf-8")
        (engine / "info.json").write_text(json.dumps(current_info), encoding="utf-8")
        data = workspace / f"interface-{case}"
        invoke(program, data, launcher)
        report = (data / "probe-result.txt").read_text(encoding="utf-8")
        if operation is None:
            assert "引擎可以使用" in report, report
            invoke(program, data, launcher, [{"operation": "synthesize", "family": "future-family"}], expected=3)
            with sqlite3.connect(data / "library.sqlite") as database:
                state, reason = database.execute("SELECT state,message FROM jobs").fetchone()
            assert state == "failed" and "模型家族" in reason and "future-family" in reason
            assert not (engine / "unexpected-post.txt").exists()
            continue
        assert f"{affected}暂不可用" in report and "切片暂不可用" not in report, report
        invoke(program, data, launcher, [{"operation": operation, "family": "v5turbo"}], expected=3)
        with sqlite3.connect(data / "library.sqlite") as database:
            state, reason = database.execute("SELECT state,message FROM jobs").fetchone()
        assert state == "failed" and evidence in reason, (state, reason)
        if case == "changed-return":
            assert "返回结构" in reason
        assert not (data / "jobs").exists()
        assert not (engine / "unexpected-post.txt").exists()
    print("Interface: minor version and custom ports accepted with intact contracts; "
          "renamed endpoint, reordered parameters and changed return blocked only their operations.", flush=True)


def check_real_batch(program, engine, fixture, data, scratch):
    data.mkdir()
    voice = {**fixture, "voice_id": str(uuid.uuid4())}
    for field in ("gpt", "sovits", "reference"):
        source = Path(voice[field]).resolve(strict=True)
        assert source.is_relative_to(scratch), source
        target = data / (field + source.suffix)
        try:
            os.link(source, target)
        except OSError:
            shutil.copyfile(source, target)
        voice[field] = target.as_posix()
    truncated = data / "damaged second weight.pth"
    with Path(voice["sovits"]).open("rb") as source:
        truncated.write_bytes(source.read(64))
    file_output = data / "output blocked by file"
    file_output.write_text("test fixture", encoding="ascii")
    unloadable = data / "second weight with invalid contents.pth"
    with zipfile.ZipFile(unloadable, "w") as checkpoint:
        checkpoint.writestr("checkpoint/data.pkl", b"this fixture contains no serialized model")
    defaults = {"top_k": 15, "top_p": 1.0, "temperature": 1.0, "speed": 1.0,
                "seed": 42, "split": "凑四句一切"}
    custom = {"top_k": 9, "top_p": 0.85, "temperature": 0.7, "speed": 1.2,
              "seed": 42, "split": "不切"}
    legacy_voice = {key: value for key, value in voice.items() if key not in defaults}
    verification_batch = str(uuid.uuid4())
    requests = [
        {**voice, "reference": (data / "absent.wav").as_posix()},
        {**voice, "sovits": truncated.as_posix()},
        {**voice, "output_root": file_output.as_posix()},
        {**voice, "sovits": unloadable.as_posix()},
        {**legacy_voice, "top_k": 101},
        {**legacy_voice, "seed": 42, "batch_id": verification_batch, "batch_index": 1, "batch_size": 2},
        {**legacy_voice, **custom, "batch_id": verification_batch, "batch_index": 2, "batch_size": 2},
    ]
    invoke(program, data, engine / "go-webui.bat", requests, expected=3)
    with sqlite3.connect(data / "library.sqlite") as database:
        rows = database.execute("SELECT id,state,message,result FROM jobs ORDER BY rowid").fetchall()
    assert len(rows) == 7, rows
    for row, evidence in zip(rows[:3], ("参考音频", "SoVITS 权重", "生成目录不可写"), strict=True):
        assert row[1] == "failed" and evidence in row[2], row
        assert not (data / "jobs" / row[0]).exists(), row
    assert "截断" in rows[1][2], rows[1]
    assert rows[3][1] == "failed" and "推理进程在就绪前退出" in rows[3][2], rows[3]
    assert (data / "jobs" / rows[3][0] / "request.json").is_file()
    assert not (data / "jobs" / rows[3][0] / "completion.json").exists()
    assert rows[4][1] == "failed" and "Top K 超出有效范围" in rows[4][2], rows[4]
    assert not (data / "jobs" / rows[4][0] / "completion.json").exists()
    expected_inputs = {}
    for name in ("gpt", "sovits", "reference"):
        with Path(voice[name]).open("rb") as stream:
            expected_inputs[name] = hashlib.file_digest(stream, "sha256").hexdigest()
    archived = []
    for row, parameters in zip(rows[5:], (defaults, custom), strict=True):
        assert row[1] == "succeeded", row
        result = json.loads(row[3])
        assert result["parameters"] == parameters and result["seed"] == parameters["seed"], result
        verification = result["voice_verification"]
        assert verification["sha256"] == expected_inputs
        assert Path(verification["launcher"]).resolve() == (engine / "go-webui.bat").resolve()
        assert verification["parameters"] == parameters and verification["seed"] == parameters["seed"]
        assert verification["verified_at"].endswith("Z")
        wav = workspace_path(data, result["audio"])
        assert hashlib.sha256(wav.read_bytes()).hexdigest() == result["sha256"]
        archived.append(wav)
    assert set((data / "outputs").rglob("*.wav")) == set(archived)
    assert len({hashlib.sha256(wav.read_bytes()).hexdigest() for wav in archived}) == 2
    later_payload = json.loads((data / "jobs" / rows[-1][0] / "request.json").read_text(encoding="utf-8"))
    assert later_payload["verification_hashes"] == expected_inputs, "Unchanged inputs in a batch should share verified input digests."
    print("Real batch: missing reference, truncated SoVITS, blocked output, and failed second-model load "
          "were isolated; invalid sampling was rejected and both WAVs archived with model verification evidence.")


def main():
    root = Path(__file__).resolve().parents[1]
    scratch = root / ".test"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=root / "out" / "portable" / "EZ-GPT-SoVITS.exe")
    parser.add_argument("--engine", type=Path, default=scratch / "engine-20261006")
    parser.add_argument("--synthesis", type=Path, default=scratch / "synthesis" / "fixtures" / "synthesis.json")
    parser.add_argument("--simulator-only", action="store_true")
    args = parser.parse_args()
    program = args.app.resolve(strict=True)
    engine = args.engine.resolve(strict=True)
    source = args.synthesis.resolve(strict=True)
    assert engine.is_relative_to(scratch) and source.is_relative_to(scratch), "Use isolated test fixtures."
    workspace = scratch / f"resources-{uuid.uuid4().hex}"
    workspace.mkdir()
    check_startup(program, workspace)
    check_batch_startup_failures(program, workspace)
    check_interface_isolation(program, workspace)
    if args.simulator_only:
        return
    fixture = json.loads(source.read_text(encoding="utf-8-sig"))
    check_real_batch(program, engine, fixture, workspace / "real-batch", scratch)


if __name__ == "__main__":
    main()
