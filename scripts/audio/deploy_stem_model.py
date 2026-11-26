"""部署并运行 MMM 声部分离目录中的 Demucs、SCNet 和 RoFormer 模型。"""

import argparse
from dataclasses import dataclass
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request


@dataclass(frozen=True)
class Artifact:
    """以发布地址和内容摘要标识一个部署资源。

    同一发布路径可能被维护者更新。将地址、摘要作为一个值保存，
    让部署时校验和实际推理时校验共用同一份记录。
    """

    # 本地使用源文件名。Demucs 另外用签名前缀定位检查点。
    url: str
    sha256: str

    @property
    def filename(self):
        """返回权重库或版本发布所使用的原文件名。"""
        return self.url.rsplit("/", 1)[-1]


@dataclass(frozen=True)
class Preset:
    """由稳定目录 ID 选择的推理方案及输出契约。

    family 用于选择上游架构，目录 ID 用于绑定权重版本。不同版本
    允许复用同一架构，模型路径和结果不能仅根据 family 推断。
    """

    weights: Artifact
    stems: tuple[str, ...]
    # None 配置表示 Demucs 自附模型参数，配置文件由本地登记权重时生成。
    # MSST 需要上游提供的 YAML，family 决定构建哪个模型类。
    family: str = "demucs"
    config: Artifact | None = None
    # 单目标模型用原混音减去预测声部，补出另一轨。这个名字保留上游文件大小写。
    target: str | None = None


DEMUCS_URL = "https://dl.fbaipublicfiles.com/demucs/hybrid_transformer/"
MSST = "ZFTurbo/Music-Source-Separation-Training"
# 模型代码、配置读取和滑窗逻辑使用同一 MSST 修订。
# 若升级推理依赖，需要用相同模型和 PCM 帧契约重跑整个目录。
MSST_REVISION = "84b1eac0887756b4f1a9d7a1ff49105939749ed2"
MSST_RAW = f"https://raw.githubusercontent.com/{MSST}/{MSST_REVISION}/"
MSST_RELEASE = f"https://github.com/{MSST}/releases/download/"
MSST_RUNTIME = Artifact(
    f"https://codeload.github.com/{MSST}/tar.gz/{MSST_REVISION}",
    "726c3707e4102b313848cbcf42874e4a74ef29e35a2139ff8cc89d4aba99339c",
)
HF = "https://huggingface.co/"
# Hugging Face 使用内容固定的修订地址，LFS 摘要与部署器读到的字节独立核对。
BECRUILY_VOCALS = HF + "becruily/mel-band-roformer-vocals/resolve/cce57b846ff188a0d1c7f3af42c0585b33f02eb1/"
BECRUILY_INST = HF + "becruily/mel-band-roformer-instrumental/resolve/0666986c0e06afaacd658e98fe7f3ac8d16deaab/"
FOUR_STEMS = ("vocals", "drums", "bass", "other")
VOCALS_AND_INSTRUMENTAL = ("vocals", "instrumental")
# inagoy 的检查点使用西班牙语通道名。MMM 清单使用稳定英文 ID。
# 映射只在 DrumSep 输出边界应用，保持上游网络通道与 WAV 的对应关系。
DRUMSEP_LABELS = {"bombo": "kick", "redoblante": "snare", "platillos": "cymbals"}

# 预设固定模型和配置的真实内容。旧 PyTorch 文件含需要反序列化的对象，
# 即使上游更新同名文件也要拒绝未核对过的内容。
MODELS = {
    # 两个 Demucs 权重使用发布文件名里的签名定位，不需要下载训练配置。
    # 同一签名在各自目录注册，原有模型环境仍然可由控制器绑定。
    "htdemucs": Preset(
        Artifact(DEMUCS_URL + "955717e8-8726e21a.th",
                 "8726e21a993978c7ba086d3872e7608d7d5bfca646ca4aca459ffda844faa8b4"),
        FOUR_STEMS,
    ),
    # 六轨含实验性钢琴分支。声部顺序供 UI 和输出清单使用，
    # 推理时按模型内部的实际顺序关联通道。
    "htdemucs_6s": Preset(
        Artifact(DEMUCS_URL + "5c90dfd2-34c22ccb.th",
                 "34c22ccb381c6f9fdbf324f04e1e2fe21aaaf293f5ded163a162697ff9a02ddd"),
        ("vocals", "drums", "bass", "guitar", "piano", "other"),
    ),
    # 这是 inagoy 的 Hybrid Demucs 检查点，输入是已提取鼓组。
    # 集成主音轨时由 HTDemucs 先生成鼓组，镲片轨包含踩镲。
    "drumsep": Preset(
        Artifact(MSST_RELEASE + "v1.0.5/model_drumsep.th",
                 "aefaa8543c9b9c75e22f5f32b53ab86dfe416457849af1383ff1aef83401423f"),
        ("kick", "snare", "toms", "cymbals"), "drumsep",
    ),
    # MSST 发布页的 starrytong 标准版与配置成对使用。
    # YAML 包含模型层宽和声部顺序，不能和 large 的配置互换。
    "scnet": Preset(
        Artifact(MSST_RELEASE + "v.1.0.6/scnet_checkpoint_musdb18.ckpt",
                 "1bc0d1abb20bfdf966dcd07637bafd03e4bc13653d09ef18bc9b3e342eafe2aa"),
        FOUR_STEMS, "scnet",
        Artifact(MSST_RELEASE + "v.1.0.6/config_musdb18_scnet.yaml",
                 "19103def86d549701f824804fc5f3d244e8e8ccd4032da6ee9d5b4f2a5f2da16"),
    ),
    # 使用 MSST 发布的兼容封装，模型仍是作者 starrytong 的 large 权重。
    # 这里的 drums 为整组鼓预测，鼓件拆分由专用 DrumSep 处理。
    "scnet_large": Preset(
        Artifact(MSST_RELEASE + "v1.0.9/SCNet-large_starrytong_fixed.ckpt",
                 "65900dfa07d6b6e5d784c0f143920200a4bd281d6e78a806c549d0b912d5885e"),
        FOUR_STEMS, "scnet",
        Artifact(MSST_RELEASE + "v1.0.9/config_musdb18_scnet_large_starrytong.yaml",
                 "3efa43af7c50d4a6e1105d2599d2d6a31c8012ee99eaad9a7e80e057945d44ee"),
    ),
    # 作者发布页提供权重，MSST 提供配套模型参数。
    # 声部输出以训练目标 vocals 为主，伴奏是混音残差。
    "kimberley_vocals": Preset(
        Artifact(HF + "KimberleyJSN/melbandroformer/resolve/ac9b0614ab3cd7f77219e18ba494dfd93956c348/MelBandRoformer.ckpt",
                 "87201f4d31afb5bc79993230fc49446918425574db48c01c405e44f365c7559e"),
        VOCALS_AND_INSTRUMENTAL, "mel_band_roformer",
        Artifact(MSST_RAW + "configs/KimberleyJensen/config_vocals_mel_band_roformer_kj.yaml",
                 "f63f38eb1e6e40a7db0dade714a5ae257555dd8748f4e774eae8679275a81926"),
        "vocals",
    ),
    # Becruily 为同一架构发布了两个不同训练目标。
    # 配置和权重固定在各自仓库的修订上，确保部署过程中二者不发生版本错配。
    "becruily_vocals": Preset(
        Artifact(BECRUILY_VOCALS + "mel_band_roformer_vocals_becruily.ckpt",
                 "a05961310cc55fbb901290c2e8be02682942f73522b6ac76bf2ec11e347ed95a"),
        VOCALS_AND_INSTRUMENTAL, "mel_band_roformer",
        Artifact(BECRUILY_VOCALS + "config.yaml",
                 "186792a36db251fc5834a883c519b609ca80abbd2bfc4668eea43ee3a4223985"),
        "vocals",
    ),
    # 作者的伴奏配置用 Instrumental 作为输出键。读取时保留大小写，
    # 在发布 WAV 时按 MMM 的小写声部标识命名。
    "becruily_instrumental": Preset(
        Artifact(BECRUILY_INST + "mel_band_roformer_instrumental_becruily.ckpt",
                 "a8da6632a1c25efb1c9be783ce9ea367d226d4b918cd6c3717c8b1d7a396041d"),
        VOCALS_AND_INSTRUMENTAL, "mel_band_roformer",
        Artifact(BECRUILY_INST + "config.yaml",
                 "a222235643d329817a1123d82d8eb38392d919e90ed597e053de49c092d3b442"),
        "Instrumental",
    ),
    # 采用 MSST 目录对应的 UVR 公布的 ViperX 1297 版本。
    # 1297 与作者的其他 BS/Mel 版本使用不同检查点，不能互作镜像。
    "viperx_bs_1297": Preset(
        Artifact("https://github.com/TRvlvr/model_repo/releases/download/all_public_uvr_models/model_bs_roformer_ep_317_sdr_12.9755.ckpt",
                 "5b84f37e8d444c8cb30c79d77f613a41c05868ff9c9ac6c7049c00aefae115aa"),
        VOCALS_AND_INSTRUMENTAL, "bs_roformer",
        Artifact(MSST_RAW + "configs/viperx/model_bs_roformer_ep_317_sdr_12.9755.yaml",
                 "42e5635ceab7287b83d9591c9880966f0b61cff26a0a40a29d236bacb32e5f2c"),
        "vocals",
    ),
}


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


def download(artifact, directory, cancel_file):
    """流式下载并校验权重、配置或固定版本的推理源码。

    资源直接写入本次部署的私有目录，失败时由部署事务撤回。
    返回值指向摘要匹配的文件，配置生成器可安全引用它的原文件名。
    """
    from separate_stems import ensure_not_cancelled

    # 下载资源专属于本次部署，既有文件意味着安装状态有冲突。
    ensure_not_cancelled(cancel_file)
    path = directory / artifact.filename
    with urllib.request.urlopen(artifact.url, timeout=30) as remote:
        with path.open("xb") as output:
            # 周期检查取消并避免将大权重留在内存。
            while chunk := remote.read(256 * 1024):
                ensure_not_cancelled(cancel_file)
                output.write(chunk)
    verify(path, artifact.sha256)
    return path


def install_runtime(preset, root, pip, python, cancel_file):
    """按推理后端准备依赖，复用固定的 CPU Torch 基础环境。

    pip 参数已经绑定到本模型的解释器。不同模型使用独立包目录，
    上游后端导入失败也不会改变其它模型的安装状态。
    """
    from separate_stems import run_child

    # DrumSep 沿用 Demucs 公共推理接口，二者都不需要下载 MSST 训练源码。
    if preset.family in ("demucs", "drumsep"):
        run_child(pip + ["demucs==4.0.1", "soundfile==0.14.0"], cancel_file)
        run_child([python, "-c", "import demucs, soundfile, torch"], cancel_file)
        return

    # MSST 的完整 requirements 含训练和桌面 GUI。只安装推理入口及这些
    # 模型的依赖，避免 CUDA 工具链、训练框架和额外图形程序。
    run_child(pip + [
        "soundfile==0.14.0", "librosa==0.11.0", "omegaconf==2.3.0",
        "ml_collections==1.1.0", "matplotlib==3.10.8", "tqdm==4.67.1",
        "beartype==0.14.1", "rotary_embedding_torch==0.3.5", "einops==0.8.1",
    ], cancel_file)
    archive = download(MSST_RUNTIME, root, cancel_file)
    # 固定摘要、目标目录和 tarfile 数据过滤共同约束解包范围。
    with tarfile.open(archive, "r:gz") as package:
        package.extractall(root, filter="data")
    # 部署目录保持稳定。推理入口只引用 runtime，不在使用时重新选择版本。
    (root / f"Music-Source-Separation-Training-{MSST_REVISION}").rename(root / "runtime")
    archive.unlink()
    # 执行导入探针后才下载数百 MiB 的模型权重。
    # 从上游目录做探针，避免把安装器自身误认作同名的 inference 模块。
    run_child([python, "-c", "import inference"], cancel_file, cwd=root / "runtime")


def write_demucs_config(directory, model, checkpoint):
    """在本地 Demucs 权重库中登记检查点签名前缀。"""
    config = directory / f"{model}.yaml"
    config.write_text(f"models: ['{checkpoint.name.split('-', 1)[0]}']\n", encoding="utf-8")
    return config


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
    preset = MODELS[model]
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
        install_runtime(preset, root, pip, python, cancel_file)

        # 先完成运行时和依赖探针，再下载模型文件。
        weights = root / "weights"
        weights.mkdir()
        # 模型清单在校验配置、运行时和所有依赖权重后才会出现。
        # 多级 DrumSep 的两个权重放在同一根下，移动部署时仍能一起定位。
        checkpoint = download(preset.weights, weights, cancel_file)
        if preset.family == "drumsep":
            # inagoy 的模型只接受鼓组。先准备一份同环境内专用的 HTDemucs 四轨。
            shared = download(MODELS["htdemucs"].weights, weights, cancel_file)
            config = write_demucs_config(weights, "htdemucs", shared)
        elif preset.config is None:
            # 模型的 YAML 仅引用同目录校验过的权重。
            config = write_demucs_config(weights, model, checkpoint)
        else:
            config = download(preset.config, weights, cancel_file)
        # 从内嵌的安装入口复制，模型可独立于当前运行的 MMM 版本调用。
        shutil.copyfile(__file__, root / "inference.py")
        # 计划兼容已有分离工作进程。每条声部对应同一模型，可共用一次推理。
        plan = {"version": 1, "tracks": {
            stem: {"model": model, "target": stem,
                   "config": f"weights/{config.name}", "weights": f"weights/{checkpoint.name}"}
            for stem in preset.stems
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
    # 校验部署资源，不允许编辑清单后让预设入口加载任意检查点。
    preset = MODELS[args.model_type]
    weights = Path(args.start_check_point)
    # 既检查文件名也检查内容。Demucs 查找签名依赖文件名，
    # MSST 则直接接收校验后的路径。
    if weights.name != preset.weights.filename:
        raise ValueError("Unexpected checkpoint name for model")
    verify(weights, preset.weights.sha256)
    if preset.config is None:
        infer_demucs(args, weights, preset)
    else:
        # MSST YAML 能改变架构、输出名和推理参数，读取前核对同一发布配对。
        config = Path(args.config_path)
        verify(config, preset.config.sha256)
        infer_msst(args, preset, config, weights)


def predict_demucs(model, mix):
    """用 Demucs 参考电平推理，对全静音保持原有帧数。"""
    import torch
    from demucs.apply import apply_model

    # 使用 Demucs 的参考均值和方差。全静音直接返回同长静音，避免除零。
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
            result = mix.new_zeros((len(model.sources), *mix.shape))
    return result


def infer_demucs(args, weights, preset):
    """运行单级 Demucs 或主音轨到鼓组件的两级链路。

    两级使用同一采样率和滑窗策略。第二级只依赖第一级的鼓组波形，
    因此切换独听不需要再运行第一级，混音台读到的都是对齐后的输出。
    """
    import soundfile as sf
    import torch
    from demucs.pretrained import get_model
    from demucs.states import load_model

    # 只对本次推理子进程开启上游旧检查点加载。两级输入均先校验摘要。
    os.environ.pop("TORCH_FORCE_WEIGHTS_ONLY_LOAD", None)
    os.environ["TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD"] = "1"
    torch.set_num_threads(min(4, os.cpu_count() or 1))
    drums_only = preset.family == "drumsep"
    initial_model = "htdemucs" if drums_only else args.model_type
    if drums_only:
        reference_weights = MODELS[initial_model].weights
        verify(weights.parent / reference_weights.filename, reference_weights.sha256)
    # 旧仓库按文件名前缀索引权重，DrumSep 自身的独立包不参与命名查找。
    model = get_model(initial_model, repo=weights.parent).eval()
    audio, rate = sf.read(Path(args.input_folder) / "mixture.wav",
                          dtype="float32", always_2d=True)
    # 分离工作进程已生成 44.1 kHz 的 PCM，阶段之间不允许隐式重采样。
    if rate != model.samplerate:
        raise ValueError("Input sample rate does not match the model")
    result = predict_demucs(model, torch.from_numpy(audio.T.copy()))
    if drums_only:
        # 第一阶段只保留 drums 预测。紧接着释放四轨缓冲，再载入第二级，
        # 避免同时持有两套网络和全曲四轨输出。
        drums = result[model.sources.index("drums")].clone()
        del result, model
        # 独立检查点包含模型构造信息，不需要修改第一阶段的仓库索引。
        model = load_model(weights).eval()
        if rate != model.samplerate:
            raise ValueError("DrumSep sample rate does not match the mix")
        result = predict_demucs(model, drums)
    # 上游模型声明是输出对应关系，按名称核对而非假定检查点的声部顺序。
    names = model.sources
    if drums_only:
        names = [DRUMSEP_LABELS.get(name, name) for name in names]
    if set(names) != set(preset.stems):
        raise ValueError(f"Unexpected model sources: {model.sources}")
    # 每次外层工作进程分配一个新输出目录。拒绝覆盖可暴露路径分配错误。
    dest = Path(args.store_dir)
    dest.mkdir()
    # 检查输出张量中通道组的数量，避免悄悄丢弃检查点额外返回的声部。
    for name, channel_data in zip(names, result, strict=True):
        sf.write(dest / f"{name}.wav", channel_data.T.numpy(), rate, subtype="PCM_16")


def infer_msst(args, preset, config, weights):
    """使用固定版本的 MSST 推理入口，再按 MMM 声部契约写入 WAV。"""
    import torch

    # 在同一推理进程调用上游入口，现有取消机制可结束整个作业。
    # 上游的 utils 和 models 采用本地绝对导入，因此先配置模块搜索路径。
    sys.path.insert(0, str(Path(__file__).resolve().parent / "runtime"))
    from inference import proc_folder

    torch.set_num_threads(min(4, os.cpu_count() or 1))
    dest = Path(args.store_dir)
    # 外层清理此目录；float 子目录不参与模型结果发现或混音。
    # 上游先写 float WAV，避免分离声部在计算互补轨前被量化或限幅。
    work = dest / "float"
    # 目录沿用 CPU PyTorch 预设，在音频框架没有 GPU 后端时依然可运行。
    # 上游只接收归一化工作目录，与项目素材所在路径无关。
    proc_folder({
        "model_type": preset.family, "config_path": str(config),
        "start_check_point": str(weights), "input_folder": args.input_folder,
        "store_dir": str(work), "filename_template": "{instr}",
        "pcm_type": "FLOAT", "disable_detailed_pbar": True, "force_cpu": True,
    })
    export_msst_waves(preset, Path(args.input_folder) / "mixture.wav", work, dest)
    # PCM 写出后释放上游 float 波形。失败时由外层整个临时作业目录回收。
    shutil.rmtree(work)


def export_msst_waves(preset, source, work, dest):
    """将模型结果对齐到原曲帧坐标，按块写出 MMM 使用的 PCM。"""
    import soundfile as sf

    # 全曲时长以输入 WAV 为准。单次打开音源获取元信息；只有残差路径
    # 读取混音采样。四轨输出不再为了转码把各条全曲同时载入内存。
    with sf.SoundFile(source) as mixture:
        # 工作进程在推理前将输入转为双声道，所有可混音输出沿用这个约定。
        total = mixture.frames
        rate = mixture.samplerate
        names = (preset.target,) if preset.target else preset.stems
        for name in names:
            with ExitStack() as streams:
                estimate = streams.enter_context(sf.SoundFile(work / f"{name}.wav"))
                # 跨模型的拖动和区间取样直接使用采样帧，无需重新计算缩放。
                if (estimate.samplerate, estimate.channels, estimate.frames) != (rate, 2, total):
                    raise ValueError(f"MSST changed the sample coordinates of {name}")
                # 上游可能使用首字母大写的训练目标。仅在输出协议中转为小写。
                key = name.lower()
                target = streams.enter_context(
                    sf.SoundFile(dest / f"{key}.wav", "w", rate, 2, "PCM_16"))
                # 单目标模型写一条互补轨。使用未量化预测相减，保留源时间轴。
                if preset.target:
                    complement = "instrumental" if key == "vocals" else "vocals"
                    residual = streams.enter_context(
                        sf.SoundFile(dest / f"{complement}.wav", "w", rate, 2, "PCM_16"))
                # 使用 libsndfile 的有界块读取，以相同的 PCM 规则写每条声部。
                for block in estimate.blocks(blocksize=65536, dtype="float32", always_2d=True):
                    # 两个输入从零帧开始顺序推进，尾块按实际帧数读取。
                    target.write(block)
                    if preset.target:
                        original = mixture.read(len(block), dtype="float32", always_2d=True)
                        residual.write(original - block)


if __name__ == "__main__":
    # 部署与推理使用同一可复制的入口文件，安装后无需引用仓库或临时源码。
    # 部署入口由 MMM 启动。推理参数的解析交给现有工作进程契约。
    if len(sys.argv) in (4, 5) and sys.argv[1] == "deploy":
        marker = Path(sys.argv[4]) if len(sys.argv) == 5 else None
        sys.exit(deploy(sys.argv[2], Path(sys.argv[3]), marker))
    inference()
