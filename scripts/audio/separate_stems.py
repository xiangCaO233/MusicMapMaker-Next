#!/usr/bin/env python3
"""按声部选择 MSST 权重，生成可直接载入编辑器的对齐音轨。"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import wave


TOKEN = re.compile(r"[a-z][a-z0-9_]{0,47}\Z")
# 首版使用各候选配置通用的 44.1 kHz 立体声 WAV；避免以猜测方式补帧、
# 截断或重采样某条产物，导致独听和原曲的拍点逐渐错开。
WAV_FORMAT = (44100, 2, 2)


def token(value, field):
    """限制模型、声部和后端产物名为单一路径组件。"""
    # 这些值还用于日志与产物路径。目录分隔符或空白不会被后端解释。
    if not isinstance(value, str) or TOKEN.fullmatch(value) is None:
        raise ValueError(f"{field} must be a lowercase identifier")
    return value


def require_file(path):
    """输入文件由本机用户显式选择，解析后再交给受信任的推理进程。"""
    # 严格解析在创建工作目录之前完成，输入缺失时不会产生空结果。
    path = path.resolve(strict=True)
    if not path.is_file():
        raise ValueError(f"Not a file: {path}")
    return path


def digest(path):
    """流式计算来源与权重标识，文件大小不影响 Python 进程峰值内存。"""
    # 用同一个哈希算法处理原曲、配置和权重，供后续任务比较来源。
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def wav_frames(path):
    """返回合法 PCM WAV 的精确帧数，并检测只写入文件头的失败产物。

    帧是由全部声道组成的采样位置。长度比较不会被单个声道的样本数误导。
    """
    # wave 只接受 PCM 容器。浮点或压缩输出必须由后端显式转换。
    with wave.open(str(path), "rb") as audio:
        actual = (audio.getframerate(), audio.getnchannels(), audio.getsampwidth())
        # 即使采样率正确，也不能发布没有有效内容的声部。
        frames = audio.getnframes()
        if actual != WAV_FORMAT or frames <= 0:
            raise ValueError(f"Expected stereo PCM16 at 44100 Hz: {path}")
        # 文件头声明的帧数可能比文件实际内容长。验证末尾一帧无需通读文件。
        audio.setpos(frames - 1)
        if len(audio.readframes(1)) != WAV_FORMAT[1] * WAV_FORMAT[2]:
            raise ValueError(f"Truncated WAV: {path}")
        return frames


def read_routes(path):
    """按权重、配置和模型族合并推理，保留声明的目标声部和输出角色。

    声部名标识导出的文件，模型族决定后端加载策略，目标名用于挑出后端文件。
    这三个值相互独立，同名目标可以出现在拥有不同权重的模型输出里。
    """
    # 不加载模型也能校验完整计划。错误配置尽早失败，保留已完成产物。
    with path.open(encoding="utf-8") as file:
        plan = json.load(file)
    if not isinstance(plan, dict) or plan.get("version") != 1:
        raise ValueError("Expected separation plan version 1")
    # 至少声明一项可发布结果；未出现的声部无需触发推理。
    tracks = plan.get("tracks")
    if not isinstance(tracks, dict) or not tracks:
        raise ValueError("The plan must specify at least one track")

    # dict 保留声明顺序，模型执行次序与清单结构在相同输入下稳定。
    routes = {}
    for stem, route in tracks.items():
        token(stem, "track name")
        if not isinstance(route, dict):
            raise ValueError(f"Expected a model route for {stem}")
        # model 选择加载器，target 选择该加载器输出的真实声部名。
        model = token(route.get("model"), "model")
        target = token(route.get("target"), "target")
        # 配置与权重路径以计划为根，同一对文件共享一次推理，
        # 即使用户在目录中同时请求了这个权重擅长的两个输出。
        config = require_file(path.parent / route["config"])
        weights = require_file(path.parent / route["weights"])
        key = (model, config, weights)
        routes.setdefault(key, []).append((stem, target))
    return routes


class SeparationCancelled(Exception):
    """本机调用者已取消当前作业。"""


def ensure_not_cancelled(cancel_file):
    """每个阶段开始前检查作业私有标记，取消时保留上一套输出。"""
    if cancel_file is not None and cancel_file.exists():
        raise SeparationCancelled("Separation cancelled")


def run_child(command, cancel_file, cwd=None):
    """在后台等待转换或推理，取消时终止并回收直接子进程。

    args 由调用者构造，保持程序路径和音频路径的参数边界。cwd 仅影响后端自身
    找依赖的上下文。取消标记属于整个分离作业，也覆盖前置格式转换和后置转码。
    只在子进程退出后向调用者报告取消，避免清理时仍有进程写入暂存目录。
    """
    ensure_not_cancelled(cancel_file)
    # CLI 普通调用不需要定时检查。GUI 传入标记后才使用有界等待。
    if cancel_file is None:
        subprocess.run(command, cwd=cwd, check=True)
        return

    # 标记放在当前作业目录，子进程及其他作业无需共享控制状态。
    with subprocess.Popen(command, cwd=cwd) as child:
        while child.poll() is None:
            if cancel_file.exists():
                # 先给后端正常退出的机会。无响应时强制停止后再清理输出。
                try:
                    child.terminate()
                except ProcessLookupError:
                    pass
                try:
                    child.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    try:
                        child.kill()
                    except ProcessLookupError:
                        pass
                    child.wait()
                raise SeparationCancelled("Separation cancelled")
            try:
                child.wait(timeout=0.2)
            except subprocess.TimeoutExpired:
                # 等待发生在 Python 工作进程内。编辑器与音频回调继续运行。
                pass
        # 后端的非零退出仍走原有故障通道；取消只有在明确观察标记时发生。
        if child.returncode:
            raise subprocess.CalledProcessError(child.returncode, command)


def publish_track(run_dir, target, destination, frames, cancel_file):
    """归一 MSST 的 WAV/FLAC 容器选择并验证原音频坐标。

    后端可按不同峰值为同一组声部选择容器，因此逐轨探测。
    读入失败的后端即使返回成功，也必须提供选中声部的文件才允许发布。
    """
    wav = run_dir / f"{target}.wav"
    if wav.is_file():
        shutil.copyfile(wav, destination)
    else:
        # MSST 在支持整型编码且峰值不溢出时可能直接写 FLAC。
        # 转封装只改变容器，不要求重采样或下混。形状不一致仍按错误处理。
        flac = require_file(run_dir / f"{target}.flac")
        run_child(
            ["ffmpeg", "-nostdin", "-v", "error", "-i", str(flac),
             "-map", "0:a:0", "-c:a", "pcm_s16le", str(destination)],
            cancel_file,
        )

    # 目标仍在临时发布区内。结果损坏时上层销毁整个工作区。
    if wav_frames(destination) != frames:
        raise ValueError(f"Frame count does not match the mixture: {destination.stem}")


def separate(source, plan, backend, output, cpu, convert_input=False, cancel_file=None):
    """在临时目录运行各组模型，验证后一次性发布选中的 WAV 和清单。

    推理环境由调用者安装，配置和模型加载错误保留原有退出状态。
    临时目录包含完整输入、各模型的独立输出，以及只含选定轨道的发布目录。
    上层可只索取单一专用模型输出，也可以用同一权重准备多个互补声部。
    """
    # 后端脚本与权重由本机作者提供，运行环境可以独立于编辑器安装。
    ensure_not_cancelled(cancel_file)
    source = require_file(source)
    backend = require_file(backend)
    routes = read_routes(require_file(plan))
    # 每次分离输出一份完整独立的目录，不覆盖已有试听来源。
    output = output.resolve()
    if output.exists():
        raise FileExistsError(f"Choose a new output directory: {output}")

    # 同盘发布确保最终对齐目录不会在复制中途被编辑器读取。
    with tempfile.TemporaryDirectory(prefix=".mmm-stems-", dir=output.parent) as temp:
        # 输入目录只包含一份混音。各模型的运行目录置于输入目录之外。
        work = Path(temp)
        inputs = work / "input"
        inputs.mkdir()
        mixture = inputs / "mixture.wav"
        # 将原曲复制成固定快照。不同模型看到同一份采样数据。
        # 命令行默认保留原始 PCM 输入，编辑器可接收 MP3、FLAC 等常见来源。
        # 两条路径最终都以同一精确 WAV 帧数作为产物校验基准。
        if convert_input:
            # 编辑器接受解码器支持的普通音频，统一转换到模型输入坐标。
            run_child(
                ["ffmpeg", "-nostdin", "-v", "error", "-i", str(source),
                 "-map", "0:a:0", "-ar", "44100", "-ac", "2",
                 "-c:a", "pcm_s16le", str(mixture)], cancel_file,
            )
        else:
            shutil.copyfile(source, mixture)
        # 原曲长度是所有结果必须遵守的坐标系，拒绝后端自行裁短或补长。
        frames = wav_frames(mixture)
        # 只把已经验证且被选择的声部写进发布目录。
        published = work / "stems"
        published.mkdir()

        # 播放器可直接读取 WAV。清单补足跨调用重用时的模型来源与采样率。
        manifest = {
            "version": 1,
            "source_sha256": digest(mixture),
            "backend_entry_sha256": digest(backend),
            "sample_rate": WAV_FORMAT[0],
            "frame_count": frames,
            "tracks": [],
        }
        # 串行运行各组可以复用 GPU 显存。多个目标共享权重时只运行一次。
        for index, ((model, config, weights), targets) in enumerate(routes.items()):
            # 不同模型可能产生同名 WAV，须隔离临时目录。
            run_dir = work / f"run_{index}"
            # 显式指定模板，避免默认的输入文件名层级改变声部地址。
            command = [
                sys.executable, str(backend), "--model_type", model,
                "--config_path", str(config), "--start_check_point", str(weights),
                "--input_folder", str(inputs), "--store_dir", str(run_dir),
                "--filename_template", "{instr}", "--pcm_type", "PCM_16",
                "--disable_detailed_pbar",
            ]
            # 后端可依据本机设备选择实现，CPU 选项便于做可复现对照。
            if cpu:
                command.append("--force_cpu")
            # 参数以 argv 逐项传递。每组模型的输入都是同一份原始 PCM 快照。
            run_child(command, cancel_file, cwd=backend.parent)
            # 哈希在每次模型推理后记录一次，每个产物直接引用相同值。
            weight_hash = digest(weights)
            config_hash = digest(config)
            for stem, target in targets:
                # 某些后端将读入失败只写入日志，仍须检查实际文件及帧数。
                # 导出名单独使用声部 ID，与后端如何命名分类无关。
                name = f"{stem}.wav"
                # 后端退出与声部文件发布之间也可能收到窗口关闭事件。
                ensure_not_cancelled(cancel_file)
                publish_track(run_dir, target, published / name, frames, cancel_file)
                # 同名声部更换权重后，来源摘要可区分两个版本。
                manifest["tracks"].append({
                    "id": stem, "file": name, "model": model, "target": target,
                    "weights_sha256": weight_hash, "config_sha256": config_hash,
                })
        # 完整性由源文件摘要和统一帧数描述。各声部可用同一范围独听或取样。
        (published / "stems.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        # 推理耗时可能较长，发布前复核目标没有被此前的另一个任务创建。
        if output.exists():
            raise FileExistsError(f"Output directory already exists: {output}")
        # 最后检查用户意图，避免模型成功后发生的取消被误当成成功发布。
        ensure_not_cancelled(cancel_file)
        published.rename(output)


def main():
    """命令行入口。推理失败保留既有输入和输出，并返回非零退出码。"""
    # 入口使用已有隔离 Python 环境。编辑器本身不引入 PyTorch 依赖。
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="44.1 kHz stereo PCM16 WAV")
    parser.add_argument("plan", type=Path, help="local JSON model routing plan")
    parser.add_argument("output", type=Path, help="a new directory for selected stems")
    parser.add_argument("--backend", required=True, type=Path, help="MSST inference.py")
    # 此参数由应用的后台作业传入。普通 CLI 使用者无需创建控制文件。
    parser.add_argument("--cancel-file", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--convert-input", action="store_true")
    parser.add_argument("--cpu", action="store_true", help="disable GPU acceleration")
    args = parser.parse_args()
    # 失败仍由工作目录上下文清理中间文件，并同时保留后端原始进度日志。
    try:
        separate(args.source, args.plan, args.backend, args.output, args.cpu,
                 args.convert_input, args.cancel_file)
    except (OSError, KeyError, TypeError, ValueError, EOFError, wave.Error,
            subprocess.CalledProcessError, SeparationCancelled) as error:
        parser.exit(1, f"Stem separation failed: {error}\n")


if __name__ == "__main__":
    main()
