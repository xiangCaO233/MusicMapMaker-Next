"""部署与运行 MMM 声部分离目录中的官方 Demucs 模型。"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request


# 旧权重含模型类，因此加载前核对发行文件的完整摘要。
MODELS = {
    "htdemucs": (
        "955717e8-8726e21a.th",
        "8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4",
        ("vocals", "drums", "bass", "other"),
    ),
    "htdemucs_6s": (
        "5c90dfd2-34c22ccb.th",
        "34c22ccb381c6f9fdbf324f04e1e2fe21aaaf293f5ded163a162697ff9a02ddd",
        ("vocals", "drums", "bass", "guitar", "piano", "other"),
    ),
}
WEIGHTS_URL = "https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/"


def interpreter(root):
    """返回指定虚拟环境的解释器，便于 Windows 与 POSIX 共用部署逻辑。"""
    # Windows 的 venv 使用 Scripts 子目录，POSIX 可通过 bin 下的入口运行。
    if os.name == "nt":
        return root / "env" / "Scripts" / "python.exe"
    return root / "env" / "bin" / "python"


def verify(path, expected):
    """对下载产物和本机权重经同一方式校验，避免依赖文件名判断。"""
    # 使用文件流，校验 80 MiB 权重时无需额外同尺寸内存。
    with path.open("rb") as stream:
        actual = hashlib.file_digest(stream, "sha256").hexdigest()
    # 文件名的短摘要不足以鉴别 PyTorch 将反序列化的整个文件。
    if actual != expected:
        raise ValueError("Official weight checksum mismatch")


def deploy(model, root, cancel_file=None):
    """在最终位置创建独立环境，所有文件就绪后发布模型清单。

    虚拟环境入口可能带绝对路径。直接在最终目录创建，目录存在时拒绝覆写。
    模型清单最后写入，绑定端可据此区分在途部署与完整部署。
    """
    # C++ 按候选解释器顺序启动。只有兼容的解释器才允许创建目标目录。
    if not (3, 11) <= sys.version_info[:2] <= (3, 12):
        return 42
    # 只有部署模式拥有共享帮助脚本。已安装的推理入口可从独立目录启动。
    from separate_stems import SeparationCancelled, ensure_not_cancelled, run_child

    ensure_not_cancelled(cancel_file)
    filename, expected, stems = MODELS[model]
    # 排他创建模型根。若另一个任务先建立了该目录，不进入清理分支。
    root.mkdir()
    try:
        # 所有 pip 命令显式使用新虚拟环境，基础 Python 只执行创建。
        uv = shutil.which("uv")
        # 部分发行版的 Python 没有 ensurepip。有 uv 时直接向刚创建的虚拟环境安装，
        # 否则由标准 venv 在这个虚拟环境内准备 pip。
        # uv 只操作这一个环境。对已有 (pip) 环境的包集合不作改动。
        if uv:
            # uv 负责定位其发行版的标准库目录，兼容符号链接启动的解释器。
            run_child([uv, "--no-config", "venv", "--python",
                       str(Path(sys.executable).resolve()), str(root / "env")], cancel_file)
        else:
            # 仅标准 venv 路径需要该模块，uv 管理的解释器可省略它。
            import venv

            venv.EnvBuilder(with_pip=True, symlinks=os.name != "nt").create(root / "env")
        # 使用绝对环境路径，避免 pip 或 uv 根据工作目录选中另一个环境。
        python = str(interpreter(root))
        pip = ([uv, "--no-config", "pip", "install", "--no-cache", "--python", python]
               if uv else [python, "-m", "pip", "--isolated", "install", "--no-cache-dir"])
        # Linux 和 Windows 选择 CPU wheel，避免默认拉取 GPU 运行库。
        torch_options = [] if sys.platform == "darwin" else [
            "--index-url", "https://download.pytorch.org/whl/cpu",
        ]
        # 独立索引仅用于获取构建选项与平台匹配的 PyTorch 发布。
        run_child(pip + torch_options + ["torch==2.6.0", "torchaudio==2.6.0"], cancel_file)
        # 第二次安装复用已满足的 torch 依赖，Demucs 从公共包索引获取。
        run_child(pip + ["demucs==4.0.1", "soundfile==0.14.0"], cancel_file)
        # 导入检查可在下载大权重前暴露架构、动态库和 Python 版本问题。
        run_child([python, "-c", "import demucs, soundfile, torch"], cancel_file)

        # 发布源固定，下载流不整块载入内存。校验通过前不会登记绑定。
        weights = root / "weights"
        # 依赖不完整时不下载权重，避免安装失败后留下不必要的大文件。
        weights.mkdir()
        # 在目标目录内写入固定官方文件名，校验失败后整组回收。
        checkpoint = weights / filename
        with urllib.request.urlopen(WEIGHTS_URL + filename, timeout=10) as remote:
            with checkpoint.open("xb") as output:
                # 分块检查取消，慢速下载不延迟到整个文件写完才响应。
                while chunk := remote.read(256 * 1024):
                    ensure_not_cancelled(cancel_file)
                    output.write(chunk)
        # 网络响应成功不代表产物有效。摘要校验先于模型清单与适配器发布。
        verify(checkpoint, expected)
        # Demucs 从本地模型仓库按签名取权重，此 YAML 将签名绑定在当前型号上。
        config = weights / f"{model}.yaml"
        config.write_text(f"models: ['{filename.split('-', 1)[0]}']\n", encoding="utf-8")
        # 从内嵌的安装入口复制，模型可独立于当前运行的 MMM 版本调用。
        shutil.copyfile(__file__, root / "inference.py")
        # 计划兼容已有分离工作进程。每条声部对应同一模型，可共用一次推理。
        plan = {"version": 1, "tracks": {
            stem: {"model": model, "target": stem,
                   "config": f"weights/{model}.yaml", "weights": f"weights/{filename}"}
            for stem in stems
        }}
        # 描述文件只有完整内容才可见。进程中断时可识别不完整目录。
        # 绑定器把最终清单看作部署已就绪的标志。
        staged = root / "model.json.pending"
        staged.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
        ensure_not_cancelled(cancel_file)
        # 同目录重命名保证绑定端只能看到完整计划。
        staged.rename(root / "model.json")
    except (ImportError, OSError, ValueError, RuntimeError, subprocess.CalledProcessError, SeparationCancelled):
        # 此目录刚由本任务排他创建，不回收用户选中的父目录或其他模型环境。
        shutil.rmtree(root, ignore_errors=True)
        raise
    return 0


def inference():
    """实现现有工作进程的推理参数契约，沿用独立的混音/取消链路。"""
    # 参数契约与手工 MSST 入口一致，应用无需为目录模型另建混音架构。
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("model_type", "config_path", "start_check_point", "input_folder",
                "store_dir", "filename_template", "pcm_type"):
        parser.add_argument("--" + key, required=True)
    parser.add_argument("--force_cpu", action="store_true")
    parser.add_argument("--disable_detailed_pbar", action="store_true")
    args = parser.parse_args()
    # 只接受应用提供的目录条目。无论模型描述如何编辑都不会执行陌生权重。
    filename, expected, stems = MODELS[args.model_type]
    weights = Path(args.start_check_point)
    if weights.name != filename:
        raise ValueError("Unexpected checkpoint name for model")
    verify(weights, expected)

    # 部署阶段不从基础解释器导入重型推理包。这里只在本模型环境加载。
    import soundfile as sf
    import torch
    from demucs.apply import apply_model
    from demucs.pretrained import get_model

    # 兼容设置限于当前子进程，并只对已验证的官方权重加载生效。
    os.environ.pop("TORCH_FORCE_WEIGHTS_ONLY_LOAD", None)
    os.environ["TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD"] = "1"
    # 与正在工作的编辑器共享同机 CPU，限制每次推理所使用的线程。
    torch.set_num_threads(min(4, os.cpu_count() or 1))
    model = get_model(args.model_type, repo=weights.parent).eval()
    # 从已加载模型校验输出角色，UI 的目录声明必须与实际结果对应。
    if set(model.sources) != set(stems):
        raise ValueError("Checkpoint source list does not match the catalog")
    # 上游工作进程已将输入归一为双声道 PCM WAV。
    audio, rate = sf.read(Path(args.input_folder) / "mixture.wav",
                          dtype="float32", always_2d=True)
    # 采样率必须匹配原曲时钟，避免 WAV 持续时间在混音台发生漂移。
    if rate != model.samplerate:
        raise ValueError("Input sample rate does not match the model")
    # 使用 Demucs 的参考均值和方差。全静音直接返回同长静音，避免除零。
    mix = torch.from_numpy(audio.T.copy())
    reference = mix.mean(0)
    # 共享归一化参数由两声道参考信号计算，输出各声部还原同一源电平。
    mean = reference.mean()
    scale = reference.std(unbiased=False)
    # 纯推理不建立梯度图，长曲按模型窗口切片以控制内存。
    with torch.inference_mode():
        if scale > 0:
            result = apply_model(model, ((mix - mean) / scale).unsqueeze(0),
                                 device="cpu", shifts=0, split=True,
                                 progress=False)[0] * scale + mean
        # 静默输出保留时间轴，不改变后续区间取样的帧坐标。
        else:
            result = mix.new_zeros((len(stems), *mix.shape))
    dest = Path(args.store_dir)
    # 分离进程为模型提供独立目标，没有其他工作会写到此处。
    # 排他创建目录，避免误把早先任务的结果当作当前输出。
    dest.mkdir()
    # 按模型声部名输出，调用方会对每个请求文件的帧数与格式再检查。
    for name, channel_data in zip(model.sources, result):
        sf.write(dest / f"{name}.wav", channel_data.T.numpy(), rate, subtype="PCM_16")


if __name__ == "__main__":
    # 部署与推理使用同一可复制的入口文件，安装后无需引用仓库或临时源码。
    # 部署入口由 MMM 启动。推理参数的解析交给现有工作进程契约。
    if len(sys.argv) in (4, 5) and sys.argv[1] == "deploy":
        marker = Path(sys.argv[4]) if len(sys.argv) == 5 else None
        sys.exit(deploy(sys.argv[2], Path(sys.argv[3]), marker))
    inference()
