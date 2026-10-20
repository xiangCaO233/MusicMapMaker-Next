#!/usr/bin/env python3
"""把版本控制中的应用资源打包到集中目录，不写入源码资源树。"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import zipfile


def main():
    """按 Git 文件清单生成 Android 资源 ZIP 和内容版本标记。"""
    # 参数只接受输入根和生成目录；调用者负责将生成目录放在仓库外。
    # ZIP 内容不包含开发机配置、密钥或本地下载文件。
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    # 只打包受版本控制的资源，排除本地导出文件或用户临时添加的大型媒体。
    tracked = subprocess.check_output(["git", "-C", str(args.repo), "ls-files", "-z", "assets/"])
    # Git 使用 NUL 分隔以支持空格和中文文件名，不能逐行拆分路径。
    # 固定排序使资源内容标记不会受目录枚举顺序影响。
    names = sorted(name.decode("utf-8") for name in tracked.split(b"\0") if name)
    args.output.mkdir(parents=True, exist_ok=True)
    archive = args.output / "mmm-assets.zip"
    # 固定 ZIP 时间戳和顺序，相同资源不会仅因重新构建而触发真机再次解压。
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as output:
        for name in names:
            path = args.repo / name
            # Git 清单中的资源必须已检出；缺失内容与 LFS 指针都应严格报错。
            # 目录不作为文件条目写入，解压器按普通文件父路径创建目录。
            if not path.is_file():
                raise SystemExit(f"Missing tracked asset: {name}")
            data = path.read_bytes()
            # LFS 指针不能作为字体、皮肤或音频资源打包，缺资源立即终止。
            if data.startswith(b"version https://git-lfs.github.com/spec/v1\n"):
                raise SystemExit(f"Missing LFS object: {name}")
            # ZIP 根从 assets 开始展开，避免私有资源目录出现额外 assets 层。
            # 不携带源文件 mtime 和宿主权限位，保持不同工作区产物内容稳定。
            info = zipfile.ZipInfo(str(Path(name).relative_to("assets")), date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            output.writestr(info, data)
    # 版本标记与 ZIP 同时随 APK 分发，只在原生启动低频路径读取。
    # 标记覆盖最终 ZIP 字节，原生启动只需比较短文本即可判断是否更新。
    # 此处内存读取属于构建工具低频操作，不进入应用帧循环。
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (args.output / "mmm-assets.sha256").write_text(digest, encoding="ascii")
    print(f"Bundled assets: {len(names)} files, {archive.stat().st_size} bytes, {digest}")


if __name__ == "__main__":
    main()
