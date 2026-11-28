"""Run EZ jobs with the Gradio client bundled in the selected engine."""

from contextlib import contextmanager, redirect_stderr, redirect_stdout
from datetime import datetime
import hashlib
import importlib.util
import json
import os
import re
import sys
from tempfile import TemporaryDirectory
import time
import traceback
import uuid
import zipfile
from concurrent.futures import TimeoutError
from pathlib import Path

from gradio_client import Client, handle_file
import psutil
import soundfile


MODELS = "GPT_SoVITS/pretrained_models"
BERT = f"{MODELS}/chinese-roberta-wwm-ext-large"
HUBERT = f"{MODELS}/chinese-hubert-base"
ADAPTERS = {"20261006": "20261006-contract.json", "0725-cu124": "0725-cu124-contract.json"}
CONTRACT = None
SPLITS = ("不切", "凑四句一切", "凑50字一切", "按中文句号。切", "按英文句号.切", "按标点符号切")
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
RESUME_TRAINERS = {
    "train_sovits": "GPT_SoVITS/s2_train_v3_lora.py",
    "train_gpt": "GPT_SoVITS/s1_train.py",
}


def safe_path(value, root=None):
    original = Path(value)
    if not original.is_absolute():
        raise ValueError(f"需要绝对路径：{original}")
    path = original.resolve()
    if root is not None and not path.is_relative_to(root):
        raise ValueError(f"路径超出任务目录：{path}")
    return path


def shell_path(value, root):
    path = safe_path(value, root)
    if any(character in str(path) for character in ('"', "%", "!", "&", "|", "^", "<", ">", "\r", "\n")):
        raise ValueError("请使用不含命令行特殊字符的数据目录")
    return path.as_posix()


def artifact_stem(kind, folder=None):
    if folder is not None and re.fullmatch(rf"{kind}-[0-9]{{12}}(?:-[0-9]+)?", folder.name):
        return folder.name
    return f"{kind}-{datetime.now():%Y%m%d%H%M}"


def publish(**values):
    print("EZ_RESULT " + json.dumps(values, ensure_ascii=True), flush=True)


class JobProgress:
    def __init__(self, job):
        self.path = safe_path(job / "progress.json", job)
        self.temporary = safe_path(job / "progress.json.partial", job)
        self.last = None

    def __call__(self, phase, completed=0, total=0, unit="", device=""):
        update = {"phase": phase, "completed": completed, "total": total, "unit": unit, "device": device}
        if update == self.last:
            return
        content = json.dumps(update, ensure_ascii=False)
        try:
            self.temporary.write_text(content, encoding="utf-8")
            os.replace(self.temporary, self.path)
            print("EZ_PROGRESS " + content, flush=True)
        except OSError:
            # A progress update must never fail the task.
            return
        self.last = update


def connect(port, download_files=False):
    return Client(
        f"http://127.0.0.1:{port}",
        verbose=False,
        download_files=download_files,
        httpx_kwargs={"trust_env": False},
    )


class Cancelled(Exception):
    pass


class InterfaceChanged(ValueError):
    pass


def check_cancel(marker):
    if marker is not None and marker.exists():
        raise Cancelled


def verify_interface(client, endpoint, expected):
    matches = [item for item in client.config["dependencies"] if item.get("api_name") == endpoint]
    if len(matches) != 1:
        raise InterfaceChanged(f"接口已变化：{endpoint} 缺失或重名。")
    info = client.view_api(return_format="dict", print_info=False)["named_endpoints"].get(f"/{endpoint}", {})
    expected_count = len(expected["inputs"])
    component_ids = matches[0]["inputs"]
    parameters = info.get("parameters", ())
    if len(component_ids) != expected_count or len(parameters) != expected_count:
        raise InterfaceChanged(f"接口已变化：{endpoint} 有 {len(component_ids)} 项入参，适配契约需要 {expected_count} 项。")
    components = {item["id"]: item for item in client.config["components"]}
    for index, (parameter, component_id, required) in enumerate(zip(parameters, component_ids, expected["inputs"]), 1):
        component = components.get(component_id, {})
        observed = [parameter["parameter_name"], parameter["component"], parameter["type"].get("type"),
                    component.get("props", {}).get("label", "")]
        if observed != required or component.get("type", "").casefold() != parameter["component"].casefold():
            raise InterfaceChanged(f"接口已变化：{endpoint} 第 {index} 项入参的顺序、类型或控件不一致。")
    returns = [[item["component"], item["type"].get("type")] for item in info.get("returns", ())]
    if returns != expected["returns"]:
        raise InterfaceChanged(f"接口已变化：{endpoint} 的返回结构与适配契约不一致。")


def invoke(client, endpoint, *arguments, cancel=None, stop=None, on_wait=None):
    expected = CONTRACT["main"].get(endpoint) or CONTRACT["inference"][endpoint]
    verify_interface(client, endpoint, expected)
    if len(arguments) != len(expected["inputs"]):
        raise RuntimeError(f"桥接器的实参数量与适配契约不一致：{endpoint}")
    if cancel is None:
        return client.predict(*arguments, api_name=f"/{endpoint}")
    check_cancel(cancel)
    pending = client.submit(*arguments, api_name=f"/{endpoint}")
    last_update = 0
    while not pending.done():
        now = time.monotonic()
        if on_wait is not None and now - last_update >= 0.5:
            on_wait(pending)
            last_update = now
        try:
            pending.result(timeout=0.2)
        except TimeoutError:
            if not cancel.exists():
                continue
            if stop is not None:
                # Wait for the original callback as well, since a stop result alone
                # does not confirm that the original operation released the engine.
                invoke(client, stop)
                pending.result(timeout=2)
            raise Cancelled from None
    check_cancel(cancel)
    result = pending.result()
    if on_wait is not None:
        on_wait(pending)
    return result


def inspect_wav(path, reference=False, cancel=None):
    info = soundfile.info(path)
    if info.frames <= 0 or info.samplerate <= 0:
        raise ValueError(f"音频长度无效：{path}")
    # Read all frames in blocks so a truncated output fails before publication.
    frames = 0
    for block in soundfile.blocks(path, blocksize=65536):
        check_cancel(cancel)
        frames += len(block)
    if frames != info.frames:
        raise ValueError(f"音频未完整写入：{path}")
    if reference and not 3 <= info.duration <= 10:
        raise ValueError("参考音频长度应为 3 至 10 秒")
    return info


def weight_directory(kind, family):
    return f"{kind}_weights_{family}"


def stage_weight(source, root, kind, family, job, cancel):
    extension = ".ckpt" if kind == "GPT" else ".pth"
    if source.suffix.lower() != extension or source.stat().st_size <= 0:
        raise ValueError(f"{kind} 权重格式无效：{source.name}")
    directory = safe_path(root / weight_directory(kind, family), root)
    directory.mkdir(exist_ok=True)
    token = job.name.replace("-", "")
    if not re.fullmatch(r"[0-9a-f]{32}", token):
        token = uuid.uuid4().hex
    relative = f"{directory.name}/ez_{token}{extension}"
    target = safe_path(root / relative, root)
    try:
        os.link(source, target)
        return relative, 0
    except FileExistsError:
        raise
    except OSError:
        copy_with_cancel(source, target, cancel)
        return relative, target.stat().st_size


def copy_with_cancel(source, target, cancel):
    writer = target.open("xb")
    try:
        with writer, source.open("rb") as reader:
            while block := reader.read(1024 * 1024):
                check_cancel(cancel)
                writer.write(block)
    except BaseException:
        target.unlink(missing_ok=True)
        raise


def inference_children(parent):
    for child in parent.children(recursive=True):
        try:
            if child.name().lower() == "python.exe" and any(
                arg.replace("\\", "/").split("/")[-1] == "inference_webui_fast.py" for arg in child.cmdline()
            ):
                yield child
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue


def owns_listener(child, port):
    return child.is_running() and any(
        socket.status == psutil.CONN_LISTEN and socket.laddr.port == port
        for socket in child.net_connections(kind="tcp4")
    )


def main_client(parent, port):
    if not owns_listener(parent, port):
        raise RuntimeError("主服务监听进程已变化，请重启引擎")
    return connect(port)


def wait_for_inference(parent, port, job, cancel):
    started = time.monotonic()
    seen = False
    while time.monotonic() - started < 150:
        check_cancel(cancel)
        try:
            children = list(inference_children(parent))
        except psutil.NoSuchProcess as error:
            raise RuntimeError("主引擎进程已退出，请重启引擎") from error
        if not children and (seen or time.monotonic() - started >= 5):
            raise RuntimeError("快速推理进程在就绪前退出。请核对 GPT 和 SoVITS 权重，详见引擎启动日志。")
        seen |= bool(children)
        for child in children:
            try:
                if owns_listener(child, port):
                    return child, connect(port, download_files=job)
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                continue
            except Exception:
                # The listener may exist before the Gradio configuration is ready.
                continue
        time.sleep(0.2)
    raise RuntimeError("推理服务启动超时。请查看引擎启动日志。")


class SynthesisSession:
    def __init__(self, engine, request):
        self.engine = engine
        self.parent = psutil.Process(request["webui_pid"])
        self.main_port = request["main_port"]
        self.inference_port = request["inference_port"]
        self.client = main_client(self.parent, self.main_port)
        self.key = None
        self.child = None
        self.arguments = None
        self.staged = []

    def close(self):
        self.key = None
        self.child = None
        if self.arguments is not None:
            if not owns_listener(self.parent, self.main_port):
                raise RuntimeError("无法确认主服务归属，请重启引擎")
            invoke(self.client, "change_tts_inference_1", *self.arguments)
            deadline = time.monotonic() + 3
            while list(inference_children(self.parent)):
                if time.monotonic() >= deadline:
                    raise RuntimeError("无法确认快速推理服务已停止，请重新启动引擎")
                time.sleep(0.1)
            self.arguments = None
        for path in self.staged:
            safe_path(self.engine / path, self.engine).unlink(missing_ok=True)
        self.staged.clear()

    def load(self, request, data, job, cancel, progress):
        if not owns_listener(self.parent, self.main_port):
            raise RuntimeError("主服务监听进程已变化，请重启引擎")
        family = request["family"]
        gpu = request.get("gpu", "0")
        if not re.fullmatch(r"\d{1,2}", gpu):
            raise ValueError("请选择有效的显卡编号")
        weights = [safe_path(request[key], data) for key in ("gpt", "sovits")]
        versions = []
        for path in weights:
            version = path.stat()
            versions.append((path, version.st_dev, version.st_ino, version.st_size, version.st_mtime_ns))
        key = (request.get("batch_id"), request.get("voice_id"), family, gpu, versions)
        if key == self.key and self.child is not None:
            try:
                if owns_listener(self.child, self.inference_port) and self.child in inference_children(self.parent):
                    progress("复用已加载的推理服务")
                    return connect(self.inference_port, download_files=job), True, 0
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
        self.close()
        if list(inference_children(self.parent)):
            raise RuntimeError("引擎中已有快速推理进程，请停止引擎后重试")
        progress("准备两份模型权重")
        copied = 0
        for kind, source in zip(("GPT", "SoVITS"), weights, strict=True):
            check_cancel(cancel)
            staged, size = stage_weight(source, self.engine, kind, family, job, cancel)
            self.staged.append(staged)
            copied += size
        invoke(self.client, "switch_version", family)
        invoke(self.client, "change_choices")
        check_cancel(cancel)
        arguments = (BERT, HUBERT, gpu, *self.staged, True)
        progress("启动推理服务并加载模型")
        invoke(self.client, "change_tts_inference", *arguments)
        self.arguments = arguments
        self.child, tts = wait_for_inference(self.parent, self.inference_port, job, cancel)
        self.key = key
        return tts, False, copied


def synthesis_parameters(request):
    choices = (
        ("top_k", "Top K", 15, 1, 100, True),
        ("top_p", "Top P", 1.0, 0.0, 1.0, False),
        ("temperature", "温度", 1.0, 0.05, 1.0, False),
        ("speed", "语速", 1.0, 0.6, 1.65, False),
        ("seed", "种子", -1, -1, 4294967295, True),
    )
    parameters = {}
    for key, label, default, minimum, maximum, integer in choices:
        raw = request.get(key, default)
        message = f"{label} 超出有效范围（{minimum} 至 {maximum}）"
        try:
            number = float(raw)
        except (TypeError, ValueError, OverflowError):
            raise ValueError(message) from None
        if isinstance(raw, bool) or not minimum <= number <= maximum or (integer and not number.is_integer()):
            raise ValueError(message)
        parameters[key] = int(number) if integer else number
    split = request.get("split", SPLITS[1])
    if split not in SPLITS:
        raise ValueError("请选择此引擎支持的切句方式")
    parameters["split"] = split
    return parameters


def inference_arguments(request, reference, parameters):
    seed = parameters["seed"]
    common = (
        request["text"].strip(), request.get("language", "中文"), handle_file(str(reference)), None,
        request["prompt"].strip(), request.get("prompt_language", "中文"),
        parameters["top_k"], parameters["top_p"], parameters["temperature"], parameters["split"], 1,
        parameters["speed"], False, True, 0.3, seed, seed < 0,
    )
    if CONTRACT["id"] == "0725-cu124":
        # This protocol places parallel_infer immediately after keep_random.
        return (*common, True, 1.35, 32, False)
    family = request["family"]
    return (*common, False, False, 1.35, 4 if family == "v5turbo" else 32, False,
            1.30 if family == "v5dev" else 0.0)


def synthesize(session, data, job, request, cancel):
    progress = JobProgress(job)
    progress("检查声音预设和参考录音")
    family = request["family"]
    if family not in CONTRACT.get("synthesis_families", FAMILIES):
        raise ValueError("当前模型家族尚未适配")
    parameters = synthesis_parameters(request)
    reference = safe_path(request["reference"], data)
    inspect_wav(reference, reference=True, cancel=cancel)
    prompt = request["prompt"].strip()
    text = request["text"].strip()
    if not prompt or not text:
        raise ValueError("请填写参考文本和目标台词")
    tts, reused, copied = session.load(request, data, job, cancel, progress)
    try:
        verify_interface(tts, "inference", CONTRACT["inference"]["inference"])
    except InterfaceChanged as error:
        (job / "interface-error.json").write_text(json.dumps({"issue": str(error)}), encoding="utf-8")
        raise
    progress("生成音频")
    audio_path, actual_seed = invoke(tts, "inference", *inference_arguments(request, reference, parameters), cancel=cancel)
    progress("校验并保存音频")
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
                check_cancel(cancel)
                destination.write(block)
    with temporary.open("rb") as audio:
        digest = hashlib.file_digest(audio, "sha256").hexdigest()
    check_cancel(cancel)
    publish(audio=str(temporary), seed=int(actual_seed), duration=info.duration,
            bytes=temporary.stat().st_size, sha256=digest, parameters=parameters,
            inference_reused=reused, weight_bytes_copied=copied)


def require_outputs(folder, glob, stage):
    files = sorted(path for path in folder.glob(glob) if path.is_file() and path.stat().st_size > 0)
    if not files:
        raise RuntimeError(f"{stage}没有生成有效文件，请查看引擎日志")
    return files


def transcribe(engine, project, request, gpu, cancel, progress):
    progress("加载本地转写模型")
    # The embedded Python distribution omits site initialization and its .pth hooks.
    if importlib.util.find_spec("distutils") is None:
        from _distutils_hack import add_shim
        add_shim()
    from funasr import AutoModel
    import torch

    source = safe_path(request["source"], project)
    destination = safe_path(request["destination"], project)
    language = request.get("asr_language", "zh")
    models = engine / "tools/asr/models"
    if language == "zh":
        names = {
            "model": "speech_paraformer-large_asr_nat-zh-cn-16k-common-vocab8404-pytorch",
            "vad_model": "speech_fsmn_vad_zh-cn-16k-common-pytorch",
            "punc_model": "punc_ct-transformer_zh-cn-common-vocab272727-pytorch",
        }
    elif language == "yue":
        names = {
            "model": "speech_UniASR_asr_2pass-cantonese-CHS-16k-common-vocab1468-tensorflow1-online"
        }
    else:
        raise ValueError("请使用本地普通话或粤语模型，其他语种可导入标注")
    local = {role: safe_path(models / name, engine) for role, name in names.items()}
    for path in local.values():
        if not (path / "config.yaml").is_file() or not (path / "model.pt").is_file():
            raise FileNotFoundError(f"本地转写资源不完整：{path.name}。可导入已有标注或手动填写。")
    device = f"cuda:{gpu}" if torch.cuda.is_available() else "cpu"
    model = AutoModel(
        **{role: str(path) for role, path in local.items()},
        device=device,
        disable_update=True,
    )
    labels = []
    clips = require_outputs(source, "*.wav", "切片")
    progress("转写录音片段", 0, len(clips), "段", device)
    for candidate in clips:
        check_cancel(cancel)
        clip = safe_path(candidate, source)
        inspect_wav(clip)
        response = model.generate(input=str(clip))
        text = response[0].get("text", "").strip() if response else ""
        text = text.replace("|", "｜").replace("\n", " ")
        labels.append(f"{clip}|{source.name}|{language.upper()}|{text}")
        progress("转写录音片段", len(labels), len(clips), "段", device)
    check_cancel(cancel)
    progress("保存转写标注", len(labels), len(clips), "段", device)
    destination.mkdir()
    annotation = safe_path(destination / f"{artifact_stem('asr', destination)}.list", project)
    annotation.write_text("\n".join(labels) + "\n", encoding="utf-8")
    publish(annotation=str(annotation))


def annotated_samples(annotation, clips, cancel, inspect_audio):
    content = annotation.read_bytes()
    languages = {}
    names = set()
    rows = []
    for number, line in enumerate(content.decode("utf-8-sig").splitlines(), 1):
        check_cancel(cancel)
        fields = line.split("|", maxsplit=3)
        if len(fields) != 4:
            raise ValueError(f"第 {number} 行标注缺少路径、说话人、语言或文本")
        source, speaker, language, text = fields
        clip = safe_path(source, clips)
        language = language.lower()
        if not speaker.strip() or language not in {"zh", "en", "ja", "yue", "ko"} or not text.strip():
            raise ValueError(f"标注缺少说话人、文本或有效语言：{clip.name}")
        if clip.name.casefold() in names:
            raise ValueError(f"训练片段文件名重复：{clip.name}。请移除重复项或重命名片段后重新标注。")
        if inspect_audio:
            inspect_wav(clip, cancel=cancel)
        names.add(clip.name.casefold())
        languages[clip.name] = language
        rows.append((clip, speaker, language, text))
    if len(languages) < 2:
        raise ValueError("至少需要两个有效录音片段")
    return {"annotation_sha256": hashlib.sha256(content).hexdigest(), "samples": languages}, rows


def engine_audio_path_fits(path):
    # The bundled Windows FFmpeg refuses input paths at MAX_PATH.
    return len(str(path).encode("utf-16-le")) // 2 < 260


@contextmanager
def preparation_inputs(job, annotation, clips, rows, snapshot, cancel):
    if (engine_audio_path_fits(annotation)
            and all(clip.parent == clips and engine_audio_path_fits(clip) for clip, *_ in rows)):
        try:
            original_paths = shell_path(annotation, annotation.parent), shell_path(clips, clips)
        except ValueError:
            pass  # Stage shell-sensitive paths as well.
        else:
            yield original_paths
            return

    # Prefer the job's volume so the staged audio can be hard linked.
    candidate = job / "ezp-12345678" / "ez-00000001.wav"
    workdir = None
    if engine_audio_path_fits(candidate):
        try:
            shell_path(job, job)
            workdir = job
        except ValueError:
            pass
    with TemporaryDirectory(prefix="ezp-", dir=workdir) as directory:
        stage = safe_path(directory)
        staged_labels = stage / "labels.list"
        aliases = {}
        lines = []
        for number, (clip, speaker, language, text) in enumerate(rows, 1):
            check_cancel(cancel)
            alias = f"ez-{number:08d}.wav"
            target = stage / alias
            if not engine_audio_path_fits(target):
                raise ValueError("无法创建短路径预处理目录。请检查 Windows 临时文件夹的位置。")
            try:
                os.link(clip, target)
            except OSError:
                copy_with_cancel(clip, target, cancel)
            aliases[clip.name] = alias
            lines.append(f"{target.as_posix()}|{speaker}|{language}|{text}")
        staged_labels.write_text("\n".join(lines) + "\n", encoding="utf-8")
        snapshot["engine_samples"] = aliases
        yield shell_path(staged_labels, stage), shell_path(stage, stage)


def read_dataset_index(path, width, cancel, header=None):
    if not path.is_file():
        raise FileNotFoundError(f"缺少预处理索引：{path.name}。请重新准备训练数据。")
    rows = {}
    names = set()
    with path.open(encoding="utf-8-sig") as stream:
        if header is not None and stream.readline().rstrip("\r\n") != header:
            raise ValueError(f"预处理索引表头无效：{path.name}")
        for line_number, line in enumerate(stream, start=2 if header is not None else 1):
            check_cancel(cancel)
            line = line.rstrip("\r\n")
            if not line:
                continue
            fields = line.split("\t")
            name = fields[0]
            if len(fields) != width or not all(field.strip() for field in fields):
                raise ValueError(f"预处理索引记录不完整：{path.name} 第 {line_number} 行 · {name or '缺少样本名'}")
            if any(character in name for character in '/\\:|') or name.endswith((".", " ")):
                raise ValueError(f"预处理索引包含无效的样本名：{name}")
            if name.casefold() in names:
                raise ValueError(f"预处理索引包含重复样本：{path.name} · {name}")
            if header is not None and any(not token.isascii() or not token.isdecimal()
                                          for token in fields[1].split()):
                raise ValueError(f"语义索引包含无效编号：{name}")
            rows[name] = fields[1:]
            names.add(name.casefold())
    if not rows:
        raise ValueError(f"预处理索引没有有效记录：{path.name}。请查看工作台的引擎日志。")
    return rows


def verify_dataset(directory, family, snapshot, cancel):
    text_index = safe_path(directory / "2-name2text.txt", directory)
    semantic_index = safe_path(directory / "6-name2semantic.tsv", directory)
    phones = read_dataset_index(text_index, 4, cancel)
    semantics = read_dataset_index(semantic_index, 2, cancel, header="item_name\tsemantic_audio")
    languages = snapshot["samples"] if snapshot is not None else {}
    aliases = snapshot.get("engine_samples") if snapshot is not None else None
    if aliases is not None:
        if (not isinstance(aliases, dict) or aliases.keys() != languages.keys()
                or any(not isinstance(name, str) or not re.fullmatch(r"ez-\d{8}\.wav", name) for name in aliases.values())
                or len(set(aliases.values())) != len(languages)):
            raise ValueError("预处理的片段映射无效，请重新准备数据。")
        originals = {name: original for original, name in aliases.items()}
    else:
        originals = {name: name for name in (languages if snapshot is not None else phones)}
    expected = set(originals)
    for filename, found in ((text_index.name, phones), (semantic_index.name, semantics)):
        missing = sorted(expected - set(found))
        unexpected = sorted(set(found) - expected)
        if missing or unexpected:
            samples = [f"缺少 {originals[name]}" for name in missing[:3]] + [f"多出 {name}" for name in unexpected[:3]]
            raise ValueError(f"预处理样本不匹配：{filename} · {'、'.join(samples)}")
    if len(expected) < 2:
        raise ValueError("预处理数据至少需要两个有效样本")

    artifacts = [Path("2-name2text.txt"), Path("6-name2semantic.tsv")]
    for name in sorted(expected):
        check_cancel(cancel)
        original = originals[name]
        wave = safe_path(directory / "5-wav32k" / name, directory)
        if not wave.is_file():
            raise FileNotFoundError(f"缺少重采样音频：{original}")
        inspect_wav(wave, cancel=cancel)
        artifacts.append(Path("5-wav32k") / name)

        features = ["4-cnhubert"]
        if family in {"v2Pro", "v2ProPlus"}:
            features.append("7-sv_cn")
        bert = safe_path(directory / "3-bert" / f"{name}.pt", directory)
        if languages.get(original) == "zh" or bert.is_file():
            features.append("3-bert")
        for group in features:
            feature = safe_path(directory / group / f"{name}.pt", directory)
            if not feature.is_file() or not zipfile.is_zipfile(feature):
                raise ValueError(f"特征缺失或文件不完整：{group}/{original}.pt。请重新准备训练数据。")
            artifacts.append(Path(group) / f"{name}.pt")
    return artifacts, len(expected)


def new_training_run(engine, dataset, artifacts, cancel):
    run = safe_path(engine / "logs" / f"ez_{uuid.uuid4().hex}", engine)
    run.mkdir()
    for relative in artifacts:
        check_cancel(cancel)
        source = safe_path(dataset / relative, dataset)
        target = safe_path(run / relative, run)
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.suffix in {".wav", ".pt"}:
            try:
                # The pinned engine reads these features without modifying them.
                os.link(source, target)
                continue
            except OSError:
                pass
        copy_with_cancel(source, target, cancel)
    return run


def sha256_file(path, cancel):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            check_cancel(cancel)
            digest.update(block)
    return digest.hexdigest()


def resume_signature(engine, dataset, artifacts, snapshot, settings, base, cancel):
    if settings["family"] != "v5turbo" or snapshot is None:
        return None
    stage = settings["operation"]
    for path in ("webui.py", RESUME_TRAINERS[stage]):
        if sha256_file(safe_path(engine / path, engine), cancel) != CONTRACT["resume"][path]:
            return None
    config = ("s2.json" if stage == "train_sovits" else "s1longer-v2.yaml")
    inputs = hashlib.sha256()
    for path in artifacts:
        inputs.update(path.as_posix().encode("utf-8") + b"\0")
        inputs.update(bytes.fromhex(sha256_file(safe_path(dataset / path, dataset), cancel)))
    return {
        "version": 1,
        "annotation": snapshot["annotation_sha256"],
        "inputs": inputs.hexdigest(),
        "config": sha256_file(safe_path(engine / "config.py", engine), cancel),
        "train_config": sha256_file(safe_path(engine / "GPT_SoVITS/configs" / config, engine), cancel),
        "base": sha256_file(safe_path(engine / base, engine), cancel),
    }


def find_resume_checkpoint(data, job, engine, settings, signature, parent_id, cancel):
    if signature is None:
        raise ValueError("此引擎版本或数据集暂不支持从检查点继续，请重跑当前阶段。")
    if not re.fullmatch(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}", parent_id) or parent_id == job.name:
        raise ValueError("续训任务编号无效")
    previous = safe_path(data / "jobs" / parent_id / "training-run.json", data)
    receipt = json.loads(previous.read_text(encoding="utf-8"))
    if any(receipt.get(key) != value for key, value in settings.items()) or receipt.get("resume_signature") != signature:
        raise ValueError("训练设置、数据或引擎文件已变更，请重跑当前阶段。")
    logs = safe_path(engine / "logs", engine)
    original = safe_path(receipt["checkpoints"], logs)
    if original.parent != logs or not re.fullmatch(r"ez_[0-9a-f]{32}", original.name):
        raise ValueError("原任务检查点目录无效")
    is_sovits = settings["operation"] == "train_sovits"
    if is_sovits:
        checkpoint = safe_path(original / "logs_s2_v5turbo_lora_32/G_233333333333.pth", original)
    else:
        directory = safe_path(original / "logs_s1_v5turbo/ckpt", original)
        candidates = []
        for path in directory.glob("epoch=*-step=*.ckpt"):
            match = re.fullmatch(r"epoch=(\d+)-step=(\d+)\.ckpt", path.name)
            if match:
                candidates.append((int(match[1]), int(match[2]), path))
        if not candidates:
            raise ValueError("找不到可恢复的 GPT 检查点")
        candidates.sort(reverse=True)
        checkpoint = next((path for _, _, file in candidates
                           if zipfile.is_zipfile(path := safe_path(file, directory))), None)
        if checkpoint is None:
            raise ValueError("GPT 检查点尚未写完，请重跑当前阶段。")
    if not checkpoint.is_file() or not zipfile.is_zipfile(checkpoint):
        raise ValueError("检查点缺失或尚未写完，请重跑当前阶段。")
    check_cancel(cancel)
    try:
        import torch
        from pathlib import WindowsPath

        with torch.serialization.safe_globals([WindowsPath]):
            state = torch.load(checkpoint, map_location="cpu", weights_only=True, mmap=True)
        if is_sovits:
            epoch = state["iteration"]
            valid = bool(state["model"] and state["optimizer"]["state"])
        else:
            epoch = state["epoch"] + 1
            valid = bool(state["state_dict"] and state["optimizer_states"] and state["loops"])
    except Exception as error:
        raise ValueError("检查点无法读取训练状态，请重跑当前阶段。") from error
    if not valid or not 0 < epoch < settings["epochs"]:
        raise ValueError("检查点的训练轮次或优化器状态不可用于继续训练")
    check_cancel(cancel)
    return checkpoint, checkpoint.relative_to(original), epoch, None if is_sovits else state


def require_resume_trace(log, offset, stage, run, epoch):
    with log.open("rb") as stream:
        stream.seek(offset)
        for raw in stream:
            line = raw.replace(b"\0", b"").strip()
            if stage == "train_sovits" and line == f"start training from epoch {epoch + 1}".encode():
                return
            if (stage == "train_gpt" and b"Restored all states from the checkpoint at " in line
                    and run.name.encode() in line):
                return
    raise ValueError("引擎没有确认从检查点恢复，未采用本次权重。请查看启动日志或重跑该阶段。")


def preparation_progress(pending, progress, seen):
    for output in pending.outputs():
        message = output[0] if isinstance(output, (tuple, list)) and output else output
        if not isinstance(message, str):
            continue
        for marker, phase in (("1A-Doing", "处理训练文本"), ("1B-Doing", "提取声学特征"), ("1C-Doing", "提取语义特征")):
            if marker in message and marker not in seen:
                seen.add(marker)
                progress(phase)


def training_progress(weights, pattern, total, previous, progress):
    phase = "训练模型 · 已保存权重的轮次"
    saved = max(previous, progress.last["completed"]) if progress.last and progress.last["phase"] == phase else previous
    try:
        for file in weights.glob(pattern):
            match = re.search(r"[_-]e(\d+)(?=_s|\.)", file.name)
            if match and saved < int(match[1]) <= total and zipfile.is_zipfile(file):
                saved = int(match[1])
    except OSError:
        pass
    progress(phase, saved, total, "轮")


def run_training(client, engine, data, job, request, cancel):
    progress = JobProgress(job)
    stage = request["operation"]
    project = safe_path(request["project"], data)
    project.mkdir(parents=True, exist_ok=True)
    gpu = request.get("gpu", "0")
    if not re.fullmatch(r"\d{1,2}", gpu):
        raise ValueError("请选择有效的显卡编号")

    if stage == "slice":
        progress("切分原始录音")
        source = shell_path(request["source"], project)
        destination = safe_path(request["destination"], project)
        destination.mkdir()
        invoke(client, "open_slice", source, shell_path(destination, project),
               -34, 4000, 300, 10, 500, 0.9, 0.25, 1, cancel=cancel, stop="close_slice")
        clips = require_outputs(destination, "*.wav", "切片")
        progress("校验切片音频", 0, len(clips), "段")
        for index, clip in enumerate(clips, 1):
            check_cancel(cancel)
            inspect_wav(clip, cancel=cancel)
            progress("校验切片音频", index, len(clips), "段")
        added = []
        prior = request.get("seed_clips")
        previous = sorted(safe_path(prior, project).glob("*.wav")) if prior else []
        discarded = set(request.get("discarded_clips", ()))
        previous = [path for path in previous if path.name not in discarded]
        taken = {path.name.casefold() for path in [*previous, *clips]}
        stem = artifact_stem("slice", destination)
        number = 1
        for index, clip in enumerate(clips):
            check_cancel(cancel)
            match = re.fullmatch(r"(.*)_\d{10}_\d{10}\.wav", clip.name, re.IGNORECASE)
            recording = match[1] if match else ""
            name = f"{stem}.wav" if len(clips) == 1 else f"{stem}-{number:03d}.wav"
            while name.casefold() in taken:
                name = f"{stem}-{number:03d}.wav"
                number += 1
            target = safe_path(destination / name, destination)
            clip.rename(target)
            clips[index] = target
            taken.add(name.casefold())
            added.append({"name": name, "recording": recording})
        for prior_clip in previous:
            target = safe_path(destination / prior_clip.name, destination)
            check_cancel(cancel)
            try:
                os.link(safe_path(prior_clip, project), target)
            except FileExistsError:
                raise
            except OSError:
                copy_with_cancel(safe_path(prior_clip, project), target, cancel)
            clips.append(target)
        publish(clips=[str(clip) for clip in clips], added_clips=added)
        return

    if stage == "asr":
        transcribe(engine, project, request, gpu, cancel, progress)
        return

    family = request["family"]
    gpt_rel, sovits_rel = FAMILIES[family]
    gpt = f"{MODELS}/{gpt_rel}"
    sovits = f"{MODELS}/{sovits_rel}"
    experiment = request["experiment"]
    if not re.fullmatch(r"ez_[0-9a-f]{32}", experiment):
        raise ValueError("项目实验标识无效")
    experiment_dir = safe_path(engine / "logs" / experiment, engine)
    if stage == "prepare":
        progress("核对标注与录音")
        annotation = safe_path(request["annotation"], project)
        clips = safe_path(request["source"], project)
        snapshot, rows = annotated_samples(annotation, clips, cancel, inspect_audio=True)
        snapshot["family"] = family
        if experiment_dir.exists():
            raise ValueError("此预处理实验已存在，请从项目重新准备训练数据")
        with preparation_inputs(job, annotation, clips, rows, snapshot, cancel) as (labels_path, clips_path):
            invoke(client, "switch_version", family)
            progress("启动数据预处理")
            seen = set()
            invoke(client, "open1abc", family, labels_path, clips_path, experiment, gpu, gpu, gpu,
                   BERT, HUBERT, sovits, cancel=cancel, stop="close1abc",
                   on_wait=lambda pending: preparation_progress(pending, progress, seen))
        progress("校验预处理产物")
        if hashlib.sha256(annotation.read_bytes()).hexdigest() != snapshot["annotation_sha256"]:
            raise ValueError("预处理时标注发生变更，请重新准备训练数据")
        if not any((experiment_dir / "4-cnhubert").glob("*.pt")):
            raise ValueError("未生成音频特征。请在引擎日志中查看录音读取或特征提取错误。")
        _, sample_count = verify_dataset(experiment_dir, family, snapshot, cancel)
        receipt = safe_path(experiment_dir / "ez-dataset.json", experiment_dir)
        temporary = safe_path(experiment_dir / "ez-dataset.json.partial", experiment_dir)
        temporary.write_text(json.dumps(snapshot, ensure_ascii=False), encoding="utf-8")
        check_cancel(cancel)
        os.replace(temporary, receipt)
        publish(dataset=str(experiment_dir), sample_count=sample_count)
        return

    if stage not in {"train_sovits", "train_gpt"}:
        raise ValueError(f"未知训练阶段：{stage}")
    progress("校验训练样本与配置")
    manifest = safe_path(experiment_dir / "ez-dataset.json", experiment_dir)
    snapshot = json.loads(manifest.read_text(encoding="utf-8")) if manifest.is_file() else None
    if snapshot is not None and snapshot["family"] != family:
        raise ValueError("训练数据使用了其他模型家族，请重新准备训练数据")
    if request.get("annotation"):
        annotation = safe_path(request["annotation"], project)
        labels, _ = annotated_samples(annotation, safe_path(request["source"], project), cancel, inspect_audio=False)
        if snapshot is not None and labels["annotation_sha256"] != snapshot["annotation_sha256"]:
            raise ValueError("当前标注与预处理版本不同，请重新准备训练数据")
        if snapshot is None:
            snapshot = labels
    artifacts, sample_count = verify_dataset(experiment_dir, family, snapshot, cancel)
    batch = int(request.get("batch", 1))
    epochs = int(request.get("epochs", 2))
    settings = {"operation": stage, "family": family, "gpu": gpu, "batch": batch, "epochs": epochs,
                "dataset": str(experiment_dir), "project": str(project)}
    signature = resume_signature(engine, experiment_dir, artifacts, snapshot, settings,
                                 sovits if stage == "train_sovits" else gpt, cancel)
    parent = request.get("resume_job")
    if parent:
        progress("检查断点和训练配置")
    resume = find_resume_checkpoint(data, job, engine, settings, signature, parent, cancel) if parent else None
    completed_epoch = 0
    progress("准备本次训练目录")
    run = new_training_run(engine, experiment_dir, artifacts, cancel)
    if resume:
        source, relative, completed_epoch, state = resume
        del resume
        target = safe_path(run / relative, run)
        target.parent.mkdir(parents=True, exist_ok=True)
        if state is None:
            copy_with_cancel(source, target, cancel)
        else:
            import torch

            # Lightning records output_dir as a WindowsPath, which its default
            # weights-only loader rejects. Point the copied state at the new run.
            state["hyper_parameters"]["output_dir"] = str(target.parent.parent)
            with target.open("xb") as stream:
                torch.save(state, stream)
            check_cancel(cancel)
        del state

    attempt = safe_path(job / "training-run.json", job)
    partial = safe_path(job / "training-run.json.partial", job)
    partial.write_text(json.dumps({
        **settings, "checkpoints": str(run), "sample_count": sample_count, "resume_signature": signature,
        "resume_job": parent, "resume_epoch": completed_epoch,
    }, ensure_ascii=False), encoding="utf-8")
    os.replace(partial, attempt)
    invoke(client, "switch_version", family)
    kind, extension = ("SoVITS", "pth") if stage == "train_sovits" else ("GPT", "ckpt")
    weights = safe_path(engine / weight_directory(kind, family), engine)
    interval = 1 if signature is not None else epochs
    engine_log = safe_path(data / "engine-start.log", data)
    offset = engine_log.stat().st_size if completed_epoch else 0
    on_wait = None
    if signature is not None:
        epoch_pattern = f"{run.name}_e*_s*_l32.pth" if stage == "train_sovits" else f"{run.name}-e*.ckpt"
        training_progress(weights, epoch_pattern, epochs, completed_epoch, progress)
        on_wait = lambda _: training_progress(weights, epoch_pattern, epochs, completed_epoch, progress)
    else:
        progress("训练模型")
    if stage == "train_sovits":
        discriminator = "" if family.startswith("v5") else sovits.replace("/s2G", "/s2D")
        invoke(client, "open1Ba", family, batch, epochs, run.name, 0.4, True, True, interval,
               gpu, sovits, discriminator, family.startswith("v5"), 32, cancel=cancel, stop="close1Ba",
               on_wait=on_wait)
    else:
        invoke(client, "open1Bb", batch, epochs, run.name, False, True, True, interval, gpu, gpt,
               cancel=cancel, stop="close1Bb", on_wait=on_wait)
    progress("核对并保存训练权重")
    if completed_epoch:
        require_resume_trace(engine_log, offset, stage, run, completed_epoch)
    if signature is None:
        pattern = f"{run.name}*.{extension}"
    elif stage == "train_sovits":
        pattern = f"{run.name}_e{epochs}_s*_l32.pth"
    else:
        pattern = f"{run.name}-e{epochs}.ckpt"
    weight = max(require_outputs(weights, pattern, stage), key=lambda path: path.stat().st_mtime_ns)
    directory = safe_path(project / "weights" / experiment, project)
    directory.mkdir(parents=True, exist_ok=True)
    stem = artifact_stem(kind.lower())
    temporary = safe_path(job / f"weight-{uuid.uuid4().hex}.partial", job)
    try:
        copy_with_cancel(safe_path(weight, engine), temporary, cancel)
        check_cancel(cancel)
        saved = directory / f"{stem}.{extension}"
        sequence = 2
        while True:
            try:
                temporary.rename(saved)
                break
            except FileExistsError:
                saved = directory / f"{stem}-{sequence}.{extension}"
                sequence += 1
    finally:
        temporary.unlink(missing_ok=True)
    publish(weight=str(saved), checkpoints=str(run), dataset=str(experiment_dir), sample_count=sample_count,
            resume_epoch=completed_epoch)


def main(request_file):
    request = json.loads(Path(request_file).read_text(encoding="utf-8"))
    if request.get("adapter") != CONTRACT["id"]:
        raise ValueError("任务适配器与桥接契约不一致")
    engine = safe_path(request["engine"])
    data = safe_path(request["data"])
    job = safe_path(request["job"], data)
    cancel = safe_path(job / "cancel.requested", job)
    os.environ["GRADIO_ANALYTICS_ENABLED"] = "False"
    check_cancel(cancel)
    if request["operation"] == "synthesize":
        session = SynthesisSession(engine, request)
        try:
            synthesize(session, data, job, request, cancel)
        finally:
            session.close()
    else:
        if CONTRACT["id"] == "0725-cu124":
            raise ValueError("0725-cu124 适配器当前只处理语音合成")
        client = main_client(psutil.Process(request["webui_pid"]), request["main_port"])
        run_training(client, engine, data, job, request, cancel)


def synthesis_worker():
    protocol = sys.stdout
    session = None
    os.environ["GRADIO_ANALYTICS_ENABLED"] = "False"
    try:
        for line in sys.stdin:
            request = json.loads(line)
            engine = safe_path(request["engine"])
            data = safe_path(request["data"])
            job = safe_path(request["job"], data)
            cancel = safe_path(job / "cancel.requested", job)
            with (job / "result.log").open("w", encoding="utf-8") as output, \
                    (job / "error.log").open("w", encoding="utf-8") as errors, \
                    redirect_stdout(output), redirect_stderr(errors):
                exit_code = 0
                try:
                    if request.get("adapter") != CONTRACT["id"]:
                        raise ValueError("任务适配器与桥接契约不一致")
                    if session is None:
                        session = SynthesisSession(engine, request)
                    check_cancel(cancel)
                    synthesize(session, data, job, request, cancel)
                except Cancelled:
                    exit_code = 10
                    if session is not None:
                        session.close()
                except Exception:
                    exit_code = 1
                    traceback.print_exc()
                    if session is not None:
                        session.close()
            print(f"EZ_DONE {job.name} {exit_code}", file=protocol, flush=True)
    finally:
        if session is not None:
            session.close()


if __name__ == "__main__":
    try:
        adapter = sys.argv[1]
        CONTRACT = json.loads(Path(__file__).with_name(ADAPTERS[adapter]).read_text(encoding="utf-8"))
        if CONTRACT["id"] != adapter:
            raise ValueError("桥接器加载了错误的适配契约")
        if sys.argv[2] == "--synthesis-worker":
            synthesis_worker()
        else:
            main(sys.argv[2])
    except Cancelled:
        sys.exit(10)
