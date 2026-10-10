#!/usr/bin/env python3
"""从唯一 SVG 母版导出跨平台图标，不改动应用配置或安装目录。

需要 ImageMagick 的 magick 和 Pillow；输出目录由调用方显式指定。
PNG 供软件内部与网站使用，ICO 供 Windows 资源脚本使用，ICNS 供 macOS 应用束使用。
所有小尺寸都从同一份高分辨率透明母图降采样，避免不同平台轮廓漂移。
"""

import argparse
import io
import subprocess
from pathlib import Path

from PIL import Image


def main() -> None:
    """检查 SVG 输入并生成透明 PNG、多尺寸 ICO 与 Retina ICNS。"""
    parser = argparse.ArgumentParser(description=__doc__)
    # 路径以脚本所在仓库为基准，不依赖调用时的工作目录。
    default_source = Path(__file__).resolve().parents[1] / "Modules/Main/src/logo.svg"
    parser.add_argument("--source", type=Path, default=default_source)
    # 显式输出目录防止误覆盖系统图标或个人皮肤。
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.resolve()
    if not source.is_file():
        parser.error(f"SVG 母版不存在：{source}")

    # ImageMagick 只进行矢量光栅化；无背景填充，保留标志四周与双片之间的透明留白。
    # 固定高分辨率渲染后统一降采样，ICO 小图不能从已缩小的 PNG 再放大。
    raster = subprocess.run(
        ["magick", "-background", "none", str(source), "-resize", "1024x1024", "PNG32:-"],
        check=True,
        stdout=subprocess.PIPE,
    ).stdout
    image = Image.open(io.BytesIO(raster)).convert("RGBA")
    # 方形母版是所有平台容器的契约，异常输入不能被静默拉伸。
    if image.size != (1024, 1024):
        parser.error("SVG 必须具有方形 viewBox")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # PNG 原生消费路径固定为 512 像素，保留 alpha 而非烘焙深浅背景。
    image.resize((512, 512), Image.Resampling.LANCZOS).save(output / "logo.png")
    # 浏览器兼容图标与主屏幕图标同样从母图采样，不能从 ICO 解码后重复缩放。
    for size, name in ((32, "favicon-32.png"), (180, "apple-touch-icon.png")):
        image.resize((size, size), Image.Resampling.LANCZOS).save(output / name)
    # ICO 含任务栏、文件浏览器和高 DPI 尺寸，最大 256 是 Windows 标准尺寸。
    image.save(output / "logo.ico", sizes=[(n, n) for n in (16, 24, 32, 48, 64, 128, 256)])
    # Pillow 写入从小图到 1024 Retina 图的 ICNS 条目，不依赖 macOS iconutil。
    image.save(output / "logo.icns")


if __name__ == "__main__":
    # 仅显式执行才导出，导入模块或构建业务代码不会触发资源写入。
    main()
