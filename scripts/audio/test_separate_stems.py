"""用同进程契约的替身验证路由与发布，避免测试依赖模型或外网。"""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import wave


# 替身按正式 MSST 参数读取输入目录。成功时输出三类不同样本，另提供错位
# 后端用于检查帧数错误时不会发布半套缓存。
BACKEND = """
import argparse
from pathlib import Path
import subprocess
import wave

parser = argparse.ArgumentParser()
parser.add_argument("--model_type")
parser.add_argument("--config_path", type=Path)
parser.add_argument("--start_check_point", type=Path)
parser.add_argument("--input_folder", type=Path)
parser.add_argument("--store_dir", type=Path)
parser.add_argument("--filename_template")
parser.add_argument("--pcm_type")
parser.add_argument("--disable_detailed_pbar", action="store_true")
parser.add_argument("--force_cpu", action="store_true")
args = parser.parse_args()
assert args.filename_template == "{instr}" and args.pcm_type == "PCM_16"
assert args.force_cpu
with wave.open(str(args.input_folder / "mixture.wav"), "rb") as source:
    frames = source.getnframes()
    assert source.readframes(frames) == bytes(frames * 4)
with Path("calls").open("a", encoding="utf-8") as journal:
    journal.write(args.model_type + "\\n")
args.store_dir.mkdir()
for target, value in (("vocals", 1), ("bass", 2), ("drums", 3)):
    with wave.open(str(args.store_dir / (target + ".wav")), "wb") as result:
        result.setparams((2, 2, 44100, 0, "NONE", "not compressed"))
        result.writeframes(bytes([value]) * 4 * (frames - (args.model_type == "wrong")))
    if args.model_type == "flac":
        wav = args.store_dir / (target + ".wav")
        subprocess.run(["ffmpeg", "-v", "error", "-i", str(wav),
                        str(wav.with_suffix(".flac"))], check=True)
        wav.unlink()
"""


class SeparationTest(unittest.TestCase):
    """从 CLI 触发真实工作区及子进程交接，断言最终产物。"""

    def setUp(self):
        """为每例创建独立原曲、权重及配置，不触碰工程资源。"""
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / "original.wav"
        self.backend = self.root / "inference.py"
        self.output = self.root / "session"
        self.plan = self.root / "plan.json"
        self.backend.write_text(BACKEND, encoding="utf-8")
        for filename in ("config.yml", "model.ckpt"):
            (self.root / filename).write_bytes(b"test")
        # 全零原曲用于识别是否把前一模型的输出误传给下一个模型。
        with wave.open(str(self.source), "wb") as audio:
            audio.setparams((2, 2, 44100, 0, "NONE", "not compressed"))
            audio.writeframes(bytes(64))

    def run_worker(self, routes, convert=False):
        """执行同编辑器手工导入时使用的 CLI，并返回执行状态。"""
        tracks = {
            name: {"model": model, "target": target, "config": "config.yml",
                   "weights": "model.ckpt"}
            for name, model, target in routes
        }
        self.plan.write_text(json.dumps({"version": 1, "tracks": tracks}),
                             encoding="utf-8")
        script = Path(__file__).with_name("separate_stems.py")
        # 编辑器把工作入口作为 -c 参数嵌入，验证不依赖 __file__ 或当前目录。
        arguments = [
            sys.executable, "-c", script.read_text(encoding="utf-8"),
            str(self.source), str(self.plan), str(self.output),
            "--backend", str(self.backend), "--cpu",
        ]
        if convert:
            arguments.append("--convert-input")
        return subprocess.run(
            arguments, capture_output=True, text=True, timeout=10, check=False,
        )

    def test_uses_each_requested_output_and_reuses_model_run(self):
        """按声部读取专用模型，两个同权重输出合并成一次推理。"""
        result = self.run_worker([
            ("voice", "mel_band_roformer", "vocals"), ("bass", "scnet", "bass"),
            ("drums", "scnet", "drums"),
        ])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            (self.root / "calls").read_text(encoding="utf-8").splitlines(),
            ["mel_band_roformer", "scnet"],
        )
        manifest = json.loads((self.output / "stems.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["frame_count"], 16)
        self.assertEqual([entry["id"] for entry in manifest["tracks"]],
                         ["voice", "bass", "drums"])
        # 文件名和波形都验证，可捕获保留错误模型通道的回归。
        for index, name in enumerate(("voice", "bass", "drums"), start=1):
            with wave.open(str(self.output / f"{name}.wav"), "rb") as audio:
                self.assertEqual(audio.readframes(16), bytes([index]) * 64)
        self.assertFalse((self.output / "vocals.wav").exists())

    @unittest.skipUnless(shutil.which("ffmpeg"), "requires FFmpeg")
    def test_flac_is_published_as_wave_with_identical_pcm(self):
        """覆盖 MSST 使用 FLAC 容器时的真实解码和帧数检查。"""
        result = self.run_worker([("bass", "flac", "bass")], convert=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        # 只比较有效采样和布局，重新封装允许文件元数据不同。
        with wave.open(str(self.output / "bass.wav"), "rb") as audio:
            self.assertEqual(audio.getnframes(), 16)
            self.assertEqual(audio.readframes(16), bytes([2]) * 64)

    def test_invalid_output_does_not_publish_any_tracks(self):
        """首个模型成功后第二条轨道错位，整组仍不对外可见。"""
        result = self.run_worker([
            ("voice", "mel_band_roformer", "vocals"), ("bass", "wrong", "bass")
        ])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Frame count does not match", result.stderr)
        self.assertFalse(self.output.exists())
        self.assertFalse(any(self.root.glob(".mmm-stems-*")))


if __name__ == "__main__":
    unittest.main()
