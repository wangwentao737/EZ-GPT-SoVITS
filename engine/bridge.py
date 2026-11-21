"""Run one EZ job with the Gradio client bundled in the selected engine."""

import json
import os
import re
import shutil
import sys
import time
import uuid
from pathlib import Path

from gradio_client import Client, handle_file
import soundfile


PROTECTED = Path(r"E:\GPT-SoVITS-v5-20261005\GPT-SoVITS-v5-20261006")
MODELS = "GPT_SoVITS/pretrained_models"
BERT = f"{MODELS}/chinese-roberta-wwm-ext-large"
HUBERT = f"{MODELS}/chinese-hubert-base"
PORT_WEBUI = 9874
PORT_TTS = 9872
FAMILIES = {
    "v2": (
        "gsv-v2final-pretrained/s1bert25hz-5kh-longer-epoch=12-step=369668.ckpt",
        "gsv-v2final-pretrained/s2G2333k.pth",
    ),
    "v2Pro": ("s1v3.ckpt", "v2Pro/s2Gv2Pro.pth"),
    "v2ProPlus": ("s1v3.ckpt", "v2Pro/s2Gv2ProPlus.pth"),
    "v5dev": ("s1v3.ckpt", "gsv-v5-pretrained/s2Gv5dev.pth"),
    "v5turbo": ("s1v3.ckpt", "gsv-v5-pretrained/s2Gv5turbo.pth"),
}


def safe_path(value, root=None):
    original = Path(value)
    if not original.is_absolute():
        raise ValueError(f"需要绝对路径：{original}")
    if original == PROTECTED or PROTECTED in original.parents:
        raise ValueError("该路径位于受保护的原始安装目录")
    path = original.resolve()
    if path == PROTECTED or PROTECTED in path.parents:
        raise ValueError("该路径位于受保护的原始安装目录")
    if root is not None and not path.is_relative_to(root):
        raise ValueError(f"路径超出任务目录：{path}")
    return path


def shell_path(value, root):
    path = safe_path(value, root)
    if any(character in str(path) for character in ('"', "%", "!", "&", "|", "^", "<", ">", "\r", "\n")):
        raise ValueError("请使用不含命令行特殊字符的数据目录")
    return path.as_posix()


def publish(**values):
    print("EZ_RESULT " + json.dumps(values, ensure_ascii=True), flush=True)


def connect(port, download_files=False):
    return Client(
        f"http://127.0.0.1:{port}",
        verbose=False,
        download_files=download_files,
        httpx_kwargs={"trust_env": False},
    )


def invoke(client, endpoint, *arguments):
    matches = [item for item in client.config["dependencies"] if item["api_name"] == endpoint]
    if len(matches) != 1 or len(matches[0]["inputs"]) != len(arguments):
        raise ValueError(f"目标引擎接口与适配契约不一致：{endpoint}")
    return client.predict(*arguments, api_name=f"/{endpoint}")


def inspect_wav(path, reference=False):
    info = soundfile.info(path)
    if info.frames <= 0 or info.samplerate <= 0:
        raise ValueError(f"音频长度无效：{path}")
    # Read all frames in blocks so a truncated output fails before publication.
    frames = sum(len(block) for block in soundfile.blocks(path, blocksize=65536))
    if frames != info.frames:
        raise ValueError(f"音频未完整写入：{path}")
    if reference and not 3 <= info.duration <= 10:
        raise ValueError("参考音频长度应为 3 至 10 秒")
    return info


def weight_directory(kind, family):
    return f"{kind}_weights_{family}"


def stage_weight(source, root, kind, family):
    extension = ".ckpt" if kind == "GPT" else ".pth"
    if source.suffix.lower() != extension or source.stat().st_size <= 0:
        raise ValueError(f"{kind} 权重格式无效：{source.name}")
    directory = safe_path(root / weight_directory(kind, family), root)
    directory.mkdir(exist_ok=True)
    relative = f"{directory.name}/ez_{uuid.uuid4().hex}{extension}"
    target = safe_path(root / relative, root)
    try:
        os.link(source, target)
    except OSError:
        shutil.copyfile(source, target)
    return relative


def synthesize(client, engine, data, job, request):
    family = request["family"]
    if family not in FAMILIES:
        raise ValueError("当前模型家族尚未适配")
    reference = safe_path(request["reference"], data)
    inspect_wav(reference, reference=True)
    prompt = request["prompt"].strip()
    text = request["text"].strip()
    if not prompt or not text:
        raise ValueError("请填写参考文本和目标台词")
    staged = []
    start_requested = False
    gpu = request.get("gpu", "0")
    if not re.fullmatch(r"\d{1,2}", gpu):
        raise ValueError("请选择有效的显卡编号")
    try:
        for kind, key in (("GPT", "gpt"), ("SoVITS", "sovits")):
            staged.append(stage_weight(safe_path(request[key], data), engine, kind, family))
        invoke(client, "switch_version", family)
        invoke(client, "change_choices")
        start_requested = True
        args = (BERT, HUBERT, gpu, *staged, True)
        invoke(client, "change_tts_inference", *args)

        deadline = time.monotonic() + 150
        while True:
            try:
                tts = connect(PORT_TTS, download_files=job)
                break
            except Exception:
                if time.monotonic() >= deadline:
                    raise RuntimeError("推理服务启动超时。请查看引擎启动日志") from None
                time.sleep(1)

        seed = int(request.get("seed", -1))
        result = invoke(
            tts, "inference", text, request.get("language", "中文"),
            handle_file(str(reference)), None, prompt, request.get("prompt_language", "中文"),
            15, 1.0, 1.0, request.get("split", "凑四句一切"), 1,
            float(request.get("speed", 1)), False, True, 0.3, seed, seed < 0,
            False, False, 1.35, 4 if family == "v5turbo" else 32, False,
            1.30 if family == "v5dev" else 0.0,
        )
        audio_path, actual_seed = result
        path = safe_path(audio_path, job)
        info = inspect_wav(path)
        temporary = safe_path(job / "verified.wav", job)
        if info.format == "WAV":
            os.replace(path, temporary)
        else:
            with soundfile.SoundFile(path) as source, soundfile.SoundFile(
                temporary, mode="w", samplerate=source.samplerate, channels=source.channels
            ) as destination:
                for block in source.blocks(blocksize=65536):
                    destination.write(block)
        publish(audio=str(temporary), seed=int(actual_seed), duration=info.duration)
    finally:
        try:
            if start_requested:
                invoke(client, "change_tts_inference_1", *args)
        finally:
            for relative in staged:
                safe_path(engine / relative, engine).unlink(missing_ok=True)


def require_outputs(folder, glob, stage):
    files = sorted(path for path in folder.glob(glob) if path.is_file() and path.stat().st_size > 0)
    if not files:
        raise RuntimeError(f"{stage}没有生成有效文件，请查看引擎日志")
    return files


def run_training(client, engine, data, request):
    stage = request["operation"]
    project = safe_path(request["project"], data)
    project.mkdir(parents=True, exist_ok=True)
    gpu = request.get("gpu", "0")
    if not re.fullmatch(r"\d{1,2}", gpu):
        raise ValueError("请选择有效的显卡编号")

    if stage == "slice":
        source = shell_path(request["source"], project)
        destination = safe_path(request["destination"], project)
        destination.mkdir()
        invoke(client, "open_slice", source, shell_path(destination, project),
               -34, 4000, 300, 10, 500, 0.9, 0.25, 1)
        clips = require_outputs(destination, "*.wav", "切片")
        for clip in clips:
            inspect_wav(clip)
        publish(clips=[str(clip) for clip in clips])
        return

    if stage == "asr":
        source = safe_path(request["source"], project)
        destination = safe_path(request["destination"], project)
        destination.mkdir()
        invoke(client, "open_asr", shell_path(source, project), shell_path(destination, project),
               "达摩 ASR (中文)", "large", request.get("asr_language", "zh"), "float32")
        annotation = safe_path(destination / f"{source.name}.list", project)
        if not annotation.is_file() or annotation.stat().st_size == 0:
            raise RuntimeError("未生成标注清单，请查看引擎日志")
        publish(annotation=str(annotation))
        return

    family = request["family"]
    gpt_rel, sovits_rel = FAMILIES[family]
    gpt = f"{MODELS}/{gpt_rel}"
    sovits = f"{MODELS}/{sovits_rel}"
    experiment = request["experiment"]
    if not re.fullmatch(r"ez_[0-9a-f]{32}", experiment):
        raise ValueError("项目实验标识无效")
    invoke(client, "switch_version", family)
    experiment_dir = safe_path(engine / "logs" / experiment, engine)
    if stage == "prepare":
        annotation = safe_path(request["annotation"], project)
        clips = safe_path(request["source"], project)
        seen = set()
        for line in annotation.read_text(encoding="utf-8-sig").splitlines():
            source, speaker, language, text = line.split("|", maxsplit=3)
            clip = safe_path(source, clips)
            if not speaker or not language or not text.strip() or clip in seen:
                raise ValueError(f"标注缺少内容或重复引用片段：{clip.name}")
            inspect_wav(clip)
            seen.add(clip)
        if len(seen) < 2:
            raise ValueError("至少需要两个有效录音片段")
        invoke(client, "open1abc", family, shell_path(annotation, project), shell_path(clips, project),
               experiment, gpu, gpu, gpu,
               BERT, HUBERT, sovits)
        names = experiment_dir / "2-name2text.txt"
        semantic = experiment_dir / "6-name2semantic.tsv"
        for file, header in ((names, 0), (semantic, 1)):
            if not file.is_file() or len(file.read_text(encoding="utf-8").strip().splitlines()) <= header:
                raise RuntimeError(f"预处理产物不完整：{file.name}")
        publish(dataset=str(experiment_dir))
        return

    batch = int(request.get("batch", 1))
    epochs = int(request.get("epochs", 2))
    if stage == "train_sovits":
        discriminator = "" if family.startswith("v5") else sovits.replace("/s2G", "/s2D")
        invoke(client, "open1Ba", family, batch, epochs, experiment, 0.4, True, True, epochs,
               gpu, sovits, discriminator, family.startswith("v5"), 32)
        kind, extension = "SoVITS", "pth"
    elif stage == "train_gpt":
        invoke(client, "open1Bb", batch, epochs, experiment, False, True, True, epochs, gpu, gpt)
        kind, extension = "GPT", "ckpt"
    else:
        raise ValueError(f"未知训练阶段：{stage}")
    weight = max(
        require_outputs(engine / weight_directory(kind, family), f"{experiment}*.{extension}", stage),
        key=lambda path: path.stat().st_mtime_ns,
    )
    saved = safe_path(project / "weights" / experiment / weight.name, project)
    saved.parent.mkdir(parents=True, exist_ok=True)
    temporary = saved.with_name(saved.name + ".partial")
    try:
        shutil.copyfile(safe_path(weight, engine), temporary)
        os.replace(temporary, saved)
    finally:
        temporary.unlink(missing_ok=True)
    publish(weight=str(saved), checkpoints=str(experiment_dir))


def main(request_file):
    request = json.loads(Path(request_file).read_text(encoding="utf-8"))
    engine = safe_path(request["engine"])
    data = safe_path(request["data"])
    job = safe_path(request["job"], data)
    os.environ["GRADIO_ANALYTICS_ENABLED"] = "False"
    client = connect(PORT_WEBUI)
    if request["operation"] == "synthesize":
        synthesize(client, engine, data, job, request)
    else:
        run_training(client, engine, data, request)


if __name__ == "__main__":
    main(sys.argv[1])
