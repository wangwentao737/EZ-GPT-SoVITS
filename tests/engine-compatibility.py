"""Check the captured legacy contract and optionally run the registered synthesis families."""

import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import runpy
import sys
import tempfile
from types import ModuleType
from unittest.mock import patch
import uuid
import wave

from workspace_paths import workspace_path


ROOT = Path(__file__).resolve().parents[1]
SYNTHESIS_FAMILIES = {
    "20261006": {"v5turbo", "v2", "v2Pro", "v2ProPlus"},
    "0725-cu124": {"v2", "v2Pro", "v2ProPlus"},
}


class InterfaceSample:
    def __init__(self, rows, returns):
        self.config = {
            "components": [{"id": i, "type": widget_type, "props": {"label": label}}
                           for i, (_, _, _, label, widget_type) in enumerate(rows)],
            "dependencies": [{"api_name": "inference", "inputs": list(range(len(rows)))}],
        }
        self.info = {"named_endpoints": {"/inference": {
            "parameters": [{"parameter_name": name, "component": widget, "type": {"type": kind}}
                           for name, widget, kind, _, _ in rows],
            "returns": [{"component": widget, "type": {"type": kind}} for widget, kind in returns],
        }}}
        self.calls = 0

    def view_api(self, **_options):
        return self.info

    def predict(self, *args, api_name):
        self.calls += 1
        return len(args), api_name


def check_captured_interface():
    spec = importlib.util.spec_from_file_location("ez_test_bridge", ROOT / "engine/bridge.py")
    bridge = importlib.util.module_from_spec(spec)
    gradio = ModuleType("gradio_client")
    gradio.Client = object
    gradio.handle_file = str
    with patch.dict(sys.modules, {"gradio_client": gradio, "psutil": ModuleType("psutil"),
                                  "soundfile": ModuleType("soundfile")}):
        spec.loader.exec_module(bridge)
    bridge.CONTRACT = json.loads((ROOT / "engine/20261006-contract.json").read_bytes())
    expected = bridge.CONTRACT["inference"]["inference"]
    recorded = json.loads((ROOT / "tests/fixtures/0725-cu124-inference.json").read_bytes())
    assert recorded["gradio"] == bridge.CONTRACT["gradio"]
    args = [None] * len(expected["inputs"])
    matching = InterfaceSample([row + [row[1].lower()] for row in expected["inputs"]], expected["returns"])
    assert bridge.invoke(matching, "inference", *args) == (23, "/inference") and matching.calls == 1
    legacy = InterfaceSample(recorded["inputs"], recorded["returns"])
    reordered_rows = copy.deepcopy(expected["inputs"])
    reordered_rows[6], reordered_rows[7] = reordered_rows[7], reordered_rows[6]
    reordered = InterfaceSample([row + [row[1].lower()] for row in reordered_rows], expected["returns"])
    for sample, required in ((legacy, "21 项入参"), (reordered, "第 7 项")):
        try:
            bridge.invoke(sample, "inference", *args)
        except bridge.InterfaceChanged as error:
            assert required in str(error), error
        else:
            raise AssertionError("A changed inference interface accepted a call.")
        assert sample.calls == 0

    request = {"family": "v2", "language": "英文", "prompt_language": "英文", "text": "Test.",
               "prompt": "Reference.", "seed": 42, "split": "不切", "speed": 1.2, "top_p": 0.8}
    parameters = bridge.synthesis_parameters(request)
    modern = bridge.inference_arguments(request, "sample.wav", parameters)
    assert len(modern) == 23 and modern[17:] == (False, False, 1.35, 32, False, 0.0)
    bridge.CONTRACT = json.loads((ROOT / "engine/0725-cu124-contract.json").read_bytes())
    assert set(bridge.CONTRACT["synthesis_families"]) == SYNTHESIS_FAMILIES["0725-cu124"]
    mapped = bridge.inference_arguments(request, "sample.wav", parameters)
    assert mapped[:17] == modern[:17] and mapped[17:] == (True, 1.35, 32, False)
    assert mapped[15:17] == (42, False)
    assert bridge.inference_arguments(request, "sample.wav", {**parameters, "seed": -1})[15:17] == (-1, True)
    assert bridge.invoke(legacy, "inference", *mapped) == (21, "/inference") and legacy.calls == 1
    legacy_reordered = copy.deepcopy(recorded["inputs"])
    legacy_reordered[7], legacy_reordered[8] = legacy_reordered[8], legacy_reordered[7]
    for sample in (matching, InterfaceSample(legacy_reordered, recorded["returns"])):
        before = sample.calls
        try:
            bridge.invoke(sample, "inference", *mapped)
        except bridge.InterfaceChanged:
            pass
        else:
            raise AssertionError("The legacy adapter accepted a different inference signature.")
        assert sample.calls == before
    print("Both inference contracts route their own arguments and reject mismatched or reordered inputs.", flush=True)


def real_synthesis(program, engine, adapter, matrix, recordings):
    scratch = (ROOT / ".test").resolve(strict=True)
    assert engine.is_relative_to(scratch) and matrix.is_relative_to(scratch)
    run = runpy.run_path(str(ROOT / "tests/training-integrity.py"))["run"]
    clone = runpy.run_path(str(ROOT / "tests/live-cancellation.py"))["clone_file"]
    data = Path(tempfile.mkdtemp(prefix="compat-families-", dir=scratch))
    families = SYNTHESIS_FAMILIES[adapter].copy()
    requests = []
    for original in json.loads(matrix.read_text(encoding="utf-8-sig")):
        family = original["family"]
        if family not in families:
            continue
        families.remove(family)
        voice = {**original, "voice_id": str(uuid.uuid4())}
        for field in ("gpt", "sovits", "reference"):
            source = Path(original[field]).resolve(strict=True)
            assert source.is_relative_to(scratch)
            target = data / (f"{family}-{field}" + source.suffix)
            clone(source, target)
            voice[field] = str(target)
        requests.append(voice)
    assert not families, f"Missing model pairs: {families}"
    if adapter == "0725-cu124":
        voice = requests[-1]
        voice.update(batch_id=str(uuid.uuid4()), batch_index=1, batch_size=2)
        requests.append({**voice, "batch_index": 2, "seed": -1, "split": "不切",
                         "speed": 1.2, "text": "The second line uses a random seed and reuses the same weights."})
    audio_requests = []
    if recordings:
        project = data / "projects/中文录音"
        raw = project / "raw"
        raw.mkdir(parents=True)
        assert recordings.is_file() and recordings.is_relative_to(scratch)
        clone(recordings, raw / recordings.name)
        clips = project / "clips"
        audio_requests = [
            {"operation": "slice", "source": str(raw), "destination": str(clips), "project": str(project)},
            {"operation": "asr", "source": str(clips), "destination": str(project / "asr"),
             "project": str(project), "asr_language": "zh"},
        ]
    outcomes = run(program, engine, data, requests + audio_requests, expected=0)
    evidence = []
    for voice, (job, status, _, payload) in zip(requests, outcomes[:len(requests)], strict=True):
        assert status == "succeeded"
        result = json.loads(payload)
        submitted = json.loads((data / "jobs" / job / "request.json").read_bytes())
        assert submitted["adapter"] == adapter
        if voice.get("seed", -1) >= 0:
            assert result["seed"] == voice["seed"]
        else:
            assert 0 <= result["seed"] <= 2**32 - 1
        assert result["inference_reused"] == (voice.get("batch_index") == 2)
        audio = workspace_path(data, result["audio"])
        with wave.open(str(audio), "rb") as stream:
            assert stream.getnframes() > 0 and len(set(stream.readframes(stream.getnframes()))) > 1
        assert hashlib.sha256(audio.read_bytes()).hexdigest() == result["sha256"]
        print(voice["family"], job, f'{result["duration"]:.2f} s', result["sha256"], flush=True)
        evidence.append({"family": voice["family"], "job": job, "sha256": result["sha256"],
                         "duration": result["duration"], "engine_fingerprint": result["engine_fingerprint"]})
    if audio_requests:
        (_, slice_state, _, slice_payload), (_, asr_state, _, asr_payload) = outcomes[len(requests):]
        assert slice_state == asr_state == "succeeded"
        clips = [workspace_path(data, path) for path in json.loads(slice_payload)["clips"]]
        assert all(path.is_file() for path in clips)
        labels = workspace_path(data, json.loads(asr_payload)["annotation"])
        rows = labels.read_text(encoding="utf-8").splitlines()
        assert clips and len(clips) == len(rows) and any(row.rsplit("|", 1)[-1].strip() for row in rows)
        print(f"Slice and Mandarin ASR passed for {len(clips)} clips.", flush=True)
    (data / "acceptance.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print(f"Archived and decoded {len(requests)} non-silent WAV files with adapter {adapter} under {data}.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path)
    parser.add_argument("--engine", type=Path, default=ROOT / ".test/engine-20261006")
    parser.add_argument("--adapter", default="20261006", choices=SYNTHESIS_FAMILIES)
    parser.add_argument("--matrix", type=Path, help="An isolated JSON request array with the tested model pairs.")
    parser.add_argument("--recordings", type=Path, help="An isolated WAV for the slice and Mandarin ASR checks.")
    args = parser.parse_args()
    if bool(args.app) != bool(args.matrix):
        parser.error("--app and --matrix must be supplied together")
    if args.recordings and args.adapter != "20261006":
        parser.error("--recordings requires the 20261006 adapter")
    check_captured_interface()
    if args.app:
        real_synthesis(args.app.resolve(strict=True), args.engine.resolve(strict=True), args.adapter, args.matrix.resolve(strict=True),
                       args.recordings.resolve(strict=True) if args.recordings else None)


if __name__ == "__main__":
    main()
