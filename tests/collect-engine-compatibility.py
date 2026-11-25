"""Collect script hashes and read-only Gradio API snapshots from a running test installation."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
from urllib.request import HTTPRedirectHandler, ProxyHandler, build_opener


SCRIPTS = ("go-webui.bat", "webui.py", "config.py", "GPT_SoVITS/inference_webui_fast.py",
           "tools/slice_audio.py", "GPT_SoVITS/prepare_datasets/1-get-text.py",
           "GPT_SoVITS/prepare_datasets/2-get-hubert-wav32k.py", "GPT_SoVITS/prepare_datasets/2-get-sv.py",
           "GPT_SoVITS/prepare_datasets/3-get-semantic.py", "GPT_SoVITS/s2_train_v3_lora.py",
           "GPT_SoVITS/s2_train.py", "GPT_SoVITS/s1_train.py")


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, *_):
        return None


def port_number(value):
    port = int(value)
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("Use a TCP port in the range 1–65535.")
    return port


def capture(port, name, folder):
    opener = build_opener(ProxyHandler({}), NoRedirect())
    versions = {}
    for route in ("config", "info"):
        url = f"http://127.0.0.1:{port}/{route}"
        with opener.open(url, timeout=5) as response:
            body = response.read(4 * 1024 * 1024 + 1)
        if len(body) > 4 * 1024 * 1024:
            raise ValueError(f"Snapshot too large: {url}")
        document = json.loads(body)
        if route == "config":
            versions["gradio"] = document["version"]
        else:
            versions["inputs"] = {method: len(signature["parameters"])
                                  for method, signature in document["named_endpoints"].items()}
        (folder / f"{name}-{route}.json").write_bytes(body)
    return {"port": port, **versions}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--main-port", type=port_number, required=True)
    parser.add_argument("--inference-port", type=port_number)
    parser.add_argument("--source", required=True, help="Package origin and archive identifier.")
    parser.add_argument("--upstream-version", help="Optional value from a credible version declaration.")
    parser.add_argument("--version-source", help="File or link containing the declaration.")
    parser.add_argument("--device", help="GPU name and other relevant hardware details.")
    args = parser.parse_args()
    engine = args.engine.resolve(strict=True)
    output = args.output.resolve()
    if output.is_relative_to(engine):
        parser.error("Write test evidence outside the engine installation.")
    if bool(args.upstream_version) != bool(args.version_source):
        parser.error("--upstream-version and --version-source must be supplied together")
    if not (engine / "go-webui.bat").is_file():
        parser.error("--engine must contain go-webui.bat")

    report = {"captured_at": datetime.now(timezone.utc).isoformat(), "engine": str(engine),
              "source": args.source, "platform": platform.platform(), "device": args.device,
              "scripts": {}, "endpoints": {}}
    if args.upstream_version:
        report["upstream_version"] = {"value": args.upstream_version, "source": args.version_source}
    for relative in SCRIPTS:
        path = engine / relative
        if path.is_file():
            with path.open("rb") as stream:
                report["scripts"][relative] = hashlib.file_digest(stream, "sha256").hexdigest()

    output.mkdir(parents=True)
    report["endpoints"]["main"] = capture(args.main_port, "main", output)
    if args.inference_port is not None:
        report["endpoints"]["inference"] = capture(args.inference_port, "inference", output)
    (output / "capture.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(output / "capture.json")


if __name__ == "__main__":
    main()
