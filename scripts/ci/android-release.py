#!/usr/bin/env python3
"""@brief 核验并交付同源 Android APK，供 CI 聚合和正式发布共同使用。

签名与清单由 Android 构建入口实际核验；这里以其 SHA256 记录锁定同一份包。
本工具不构建、不签名、不发布版本，也不读取密钥或整个构建环境。
交付范围仅含 APK、摘要和来源记录，不把相邻缓存或签名文件带入发布目录。
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
import zipfile


def verify_delivery(directory, source, version=None):
    """@brief 核验源码身份、发布变体、版本、摘要与实际 JNI 架构。

    @param directory 构建入口产生的集中交付目录，不递归查找其他旧包。
    @param source 发布者要求的完整提交 SHA，不能只相信目录或文件名。
    @param version 可选的正式版本，允许标签具有一个 v 前缀。
    @return 已核验的 APK 路径与原始来源记录。
    @note 签名验证已经发生于构建端；文件摘要确保发布端消费的就是该包。
    """
    root = Path(directory).resolve()
    if not re.fullmatch(r"[0-9a-f]{40}", source):
        raise ValueError("Android source must be a full commit SHA")
    record_path = root / "build-info.json"
    # 不接受链接到交付目录外的记录，防止路径布局掩盖真实来源。
    if record_path.is_symlink():
        raise ValueError("Android build record must not be a symbolic link")
    record = json.loads(record_path.read_text())
    # 发布目标由外层 tag/提交确定，不从 APK 自己声明的 source 反推目标。
    # Android 与桌面即使版本号相同，也可能属于不同的尚未发布提交。
    if record.get("source") != source:
        raise ValueError("Android APK source does not match release source")
    if record.get("abi") != "arm64-v8a" or record.get("variant") != "release":
        raise ValueError("Android delivery must be an ARM64 release APK")
    actual_version = record.get("versionName", "")
    # 版本同时进入文件名，拒绝路径字符、换行和非预期的 QA 后缀。
    # 精确版本仍由调用者传入，不能把另一个版本的 APK 改名后发布。
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?", actual_version):
        raise ValueError("Android delivery has an invalid version")
    if version is not None and actual_version != version.removeprefix("v"):
        raise ValueError("Android APK version does not match release version")
    apk = root / f"MusicMapMaker-Next-{actual_version}-android-arm64.apk"
    if apk.is_symlink() or not apk.is_file() or apk.stat().st_size <= 0:
        raise ValueError("Android APK is missing, empty or linked")
    if apk.stat().st_size != record.get("bytes"):
        raise ValueError("Android APK size does not match build record")
    # 长度相同不代表内容相同；按块计算摘要，避免把大型内置资源读入内存。
    # 原记录中的证书说明不参与这次计算，不能代替 APK 字节本身的校验。
    with apk.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != record.get("sha256"):
        raise ValueError("Android APK SHA256 does not match build record")
    # 摘要之外核对 ZIP 和 JNI，避免把宿主库或错误 ABI 的安装包误当手机包。
    # CRC 检查覆盖资源正文；仅读中央目录不能发现截断或损坏的资源内容。
    with zipfile.ZipFile(apk) as archive:
        libraries = [name for name in archive.namelist() if name.startswith("lib/")]
        if {name.split("/")[1] for name in libraries} != {"arm64-v8a"}:
            raise ValueError("Android APK contains an unexpected JNI ABI")
        with archive.open("lib/arm64-v8a/libmain.so") as stream:
            header = stream.read(20)
        # e_machine 位于固定 ELF 头偏移；只改 JNI 目录名不能伪装交叉编译。
        # 无需解压本机库或加载动态库，发布检查不执行来自安装包的代码。
        if header[:4] != b"\x7fELF" or int.from_bytes(header[18:20], "little") != 183:
            raise ValueError("Android main library is not AArch64 ELF")
        if archive.testzip() is not None:
            raise ValueError("Android APK ZIP CRC check failed")
    return apk, record


def stage_delivery(directory, destination, source, version=None):
    """@brief 完整复制并原子替换一个 Android 交付目录，失败时保留原候选。

    @param destination 运行专属 staging 或 release/android-arm64 目录。
    @return 已落地的 APK 路径，可直接供后续打包和上传步骤使用。
    @note 校验在修改目标前完成，旧包、残缺包和错误来源不会覆盖有效候选。
    事务目录与目标同盘，目录重命名不需要跨文件系统复制。
    """
    apk, record = verify_delivery(directory, source, version)
    target = Path(destination).absolute()
    # 校验和落地不能复用源目录，否则替换事务会反过来移走唯一的输入包。
    # 拒绝文件及链接目标，避免事务目录与用户已有路径产生不同的覆盖语义。
    if (target.is_symlink() or (target.exists() and not target.is_dir())
            or target.resolve() == Path(directory).resolve()):
        raise ValueError("Android destination must be a separate real directory")
    target.parent.mkdir(parents=True, exist_ok=True)
    previous = target.with_name(f".{target.name}.previous")
    # 不擅自删除上次中断留下的回滚目录，以免覆盖唯一有效的候选。
    if previous.exists() or previous.is_symlink():
        raise ValueError("Android previous transaction requires recovery")
    temporary = Path(tempfile.mkdtemp(prefix=f".{target.name}.", dir=target.parent))
    # 临时目录唯一且与目标同盘；只有复制完成才切换可见的候选路径。
    # previous 只保管当前被替换的那一份，不扫描相邻版本或其他平台。
    moved = False
    try:
        # 精确白名单避免上传 SDK、原生符号和相邻私钥；摘要使用实际包名。
        shutil.copy2(apk, temporary / apk.name)
        shutil.copy2(apk.parent / "build-info.json", temporary / "build-info.json")
        (temporary / f"{apk.name}.sha256").write_text(f"{record['sha256']}  {apk.name}\n")
        # 复制后重验临时交付，源文件若在复制期间变化，不得替换当前有效候选。
        # 摘要来自实际临时 APK，记录和文件必须仍然描述同一份签名安装包。
        verify_delivery(temporary, source, version)
        if target.exists():
            os.replace(target, previous)
            moved = True
        # 新候选一步出现，读取者不会看到只有记录而 APK 仍未复制的半套目录。
        os.replace(temporary, target)
    except OSError:
        # 仅恢复本事务刚改名的旧候选，不碰其他平台或历史发布目录。
        if moved and not target.exists():
            os.replace(previous, target)
        raise
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    if moved:
        # 只有新目录已成功落地后才清理本事务的旧候选，失败路径保留回滚能力。
        shutil.rmtree(previous)
    return target / apk.name


def main():
    """@brief 提供只读校验和集中 staging 两个入口，不隐式触发公开发布。

    参数由调用者明确给出；成功时只输出最终 APK 路径，便于安全的 shell 引用。
    校验失败写入标准错误并返回非零，后续上传不得沿用上次结果。
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("verify", "stage"))
    parser.add_argument("--directory", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--version")
    parser.add_argument("--destination")
    args = parser.parse_args()
    # 标准输出仅留给已通过核验的路径；错误不返回旧路径作为成功结果。
    # 不回显整个环境、记录内容或签名配置，构建日志不需要这些私有信息。
    try:
        if args.action == "stage":
            if not args.destination:
                parser.error("stage requires --destination")
            apk = stage_delivery(args.directory, args.destination, args.source, args.version)
        else:
            apk, _ = verify_delivery(args.directory, args.source, args.version)
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile) as error:
        print(f"Android release validation failed: {error}", file=sys.stderr)
        return 1
    print(apk)
    return 0


if __name__ == "__main__":
    sys.exit(main())
