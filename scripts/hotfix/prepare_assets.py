#!/usr/bin/env python3
"""从已发布完整资源包建立基线，仅覆盖显式选择的 assets 路径。

输出保持现有客户端的 schema 1，包含完整清单与完整冷启动资源包。
未选择文件继续引用旧 URL；只为内容改变的文件生成不可变对象。
本工具不连接发布服务器、不读取凭据、不修改源码资源。
"""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import zipfile
from urllib.parse import quote


# /// @brief 统一计算文件摘要，避免一次性读取大型音频或资源包。
def digest(path):
    """摘要计算只返回十六进制正文，清单层再增加 sha256 前缀。

    文件对象由 with 管理，不让后续 ZIP 或网络阶段继承句柄。
    hashlib.file_digest 按流计算，内存不随资源包大小增长。
    """
    # 调用方仅传入经过根目录校验的普通文件。
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


# /// @brief 将远程清单路径限制为跨平台安全的 assets 相对路径。
def safe_path(value):
    """保持客户端、ZIP 和发布器使用相同的相对路径身份。

    不执行 normalize 后再放行，因为那会把恶意别名转换成合法路径。
    空段、点段和盘符直接拒绝，防止同一文件存在多个清单键。
    """
    # Windows 的盘符、反斜杠和路径别名在 Linux CI 上也必须拒绝。
    if not isinstance(value, str) or not value or "\\" in value or ":" in value:
        raise ValueError("unsafe asset path")
    if value.startswith("/") or any(p in ("", ".", "..") for p in value.split("/")):
        raise ValueError("unsafe asset path: " + value)
    return PurePosixPath(value)


# /// @brief 构造可审阅的发布目录，所有检查完成前不发布任何对象。
def prepare(source, baseline_path, package, selected, base_version, revision, output):
    """建立一次资源发布候选，不修改任何当前发布的内容。

    source 只提供显式选择项，其余文件由已发布完整包提供。
    baseline_path 的原始字节摘要绑定发布计划，避免用过期快照覆盖线上。
    revision 与正式程序版本独立，同一基线版本下必须严格递增。
    output 必须不存在，失败候选保留给诊断，不自动递归删除用户目录。

    首次启动必须拿到与新清单一致的完整包，不能把增量覆盖列表当成全量包。
    已安装客户端则通过旧条目保留 URL，只下载摘要发生变化的选择项。
    本期不提供删除项，避免删除用户已有资源或引入旧客户端不理解的语义。
    """
    # 正式版本和修订号构成对象路径，禁止通过参数逃逸输出目录。
    if not re.fullmatch(r"v\d+\.\d+\.\d+", base_version) or revision <= 0:
        raise ValueError("base version or revision is invalid")
    baseline = json.loads(Path(baseline_path).read_text(encoding="utf-8"))
    previous_base = baseline.get("base_version", baseline.get("version"))
    if previous_base != base_version or revision <= baseline.get("asset_revision", 0):
        raise ValueError("baseline version mismatch or non-increasing revision")
    # 完整包必须和已发布清单一致，不能把 CI 目录当作当前线上基线。
    expected = baseline["package"]["sha256"].removeprefix("sha256:")
    if digest(package) != expected:
        raise ValueError("baseline package checksum mismatch")
    # 清单条目必须唯一对应 ZIP 文件，重复身份不能靠字典覆盖掩盖。
    # 旧 URL 和来源字段整体复制，兼容 CDN 已重写的完整清单。
    records = {}
    for entry in baseline["files"]:
        name = str(safe_path(entry["path"]))
        if name in records:
            raise ValueError("duplicate baseline path")
        records[name] = dict(entry)
    if not records:
        raise ValueError("empty baseline manifest")
    # 不复用旧输出，避免失败重跑后遗留文件混入上传列表。
    output = Path(output).resolve()
    if output.exists():
        raise ValueError("output directory already exists")
    output.mkdir(parents=True)
    tree = output / "tree"
    tree.mkdir()
    # tree 仅是完整包构建源，不属于最终上传集合。
    # 失败输出保留供诊断，下一轮必须重新选择输出目录。
    # 按清单读取 ZIP，不信任归档目录项，也不执行 extractall。
    # 所有非目录项必须属于 assets 根，拒绝额外文件与符号链接。
    with zipfile.ZipFile(package) as archive:
        names = set()
        for info in archive.infolist():
            if info.is_dir():
                continue
            name = str(safe_path(info.filename))
            if not name.startswith("assets/") or (info.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError("unsafe baseline archive member")
            relative = name[len("assets/"):]
            if relative in names or relative not in records:
                raise ValueError("unexpected or duplicate archive member")
            names.add(relative)
            # 文件名已校验，创建父目录只会发生在本次 tree 下。
            target = tree / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as incoming, target.open("wb") as outgoing:
                shutil.copyfileobj(incoming, outgoing)
            if digest(target) != records[relative]["sha256"].removeprefix("sha256:"):
                raise ValueError("baseline file checksum mismatch: " + relative)
        if names != set(records):
            raise ValueError("baseline archive is incomplete")
    source = Path(source).resolve()
    chosen = set()
    # 选择项可为文件或目录；缺失项与符号链接不能静默跳过。
    for selection in selected:
        relative = str(safe_path(selection))
        candidate = source / relative
        # 包括目录本身和中间父目录，不能经内部符号链接挑选外部文件。
        if any(p.is_symlink() for p in (candidate, *candidate.parents) if p != source.parent):
            raise ValueError("selected symlink is not allowed")
        if not candidate.exists() or not candidate.resolve().is_relative_to(source):
            raise ValueError("selected asset is missing or outside source")
        paths = [candidate] if candidate.is_file() else candidate.rglob("*")
        for file in paths:
            if file.is_symlink():
                raise ValueError("selected symlink is not allowed")
            if file.is_file():
                chosen.add(file.relative_to(source).as_posix())
    if not chosen:
        raise ValueError("no assets selected")
    prefix = f"download/hotfix/assets/{base_version}/r{revision}"
    # 上传计划只包含差异资源和完整包，未选项不从分支读取。
    # 同一正式版本可以累积多次选择覆盖，其他资产仍继承线上基线。
    uploads = []
    for name in sorted(chosen):
        original = source / name
        checksum = digest(original)
        # 相同内容保留旧 URL，客户端也不会重新下载。
        if name in records and records[name]["sha256"].removeprefix("sha256:") == checksum:
            continue
        target = tree / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(original, target)
        # ZIP 和单文件都从同一个暂存副本生成，避免两次读取不同源字节。
        key = prefix + "/files/" + name
        staged = output / key
        staged.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(target, staged)
        entry = {"path": name, "url": "/" + quote(key, safe="/"),
                 "sha256": "sha256:" + checksum, "size": staged.stat().st_size}
        records[name] = entry
        uploads.append({"key": key, "sha256": checksum, "size": entry["size"]})
    if not uploads:
        # 不发布空修订，避免用户看到版本变化却没有任何可更新内容。
        raise ValueError("selected assets have no content changes")
    # 冷启动完整包由更新后的整个基线生成，不能继续引用旧包并提前写新版本标记。
    package_key = prefix + "/assets.zip"
    package_out = output / package_key
    package_out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(package_out, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        # 排序只用于低频发布产物，不参与客户端渲染路径。
        for name in sorted(records):
            archive.write(tree / name, "assets/" + name)
    # ZIP 关闭后才能校验完整中央目录和正文。
    checksum = digest(package_out)
    uploads.append({"key": package_key, "sha256": checksum, "size": package_out.stat().st_size})
    manifest = {"schema": 1, "version": f"{base_version}-assets.{revision}",
                "base_version": base_version, "asset_revision": revision,
                "package": {"url": "/" + package_key, "sha256": "sha256:" + checksum,
                            "size": package_out.stat().st_size},
                "files": [records[name] for name in sorted(records)]}
    # 发布计划记录原清单精确摘要，用于服务器并发修改检测。
    plan = {"baseline_sha256": digest(baseline_path), "uploads": uploads}
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (output / "plan.json").write_text(json.dumps(plan, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    # 清理范围只有新输出内 tree，不接触源资源或已发布包。
    shutil.rmtree(tree)
    # 原始基线摘要绑定候选，发布前的任何线上变化都会导致拒绝。
    return manifest, plan


# /// @brief CLI 只准备文件，正式发布由独立入口执行。
def main():
    """参数均是本地文件或严格版本文本，没有隐式网络动作。

    select 的各项由 argparse 保留边界，支持带空格的资源文件名。
    输出契约固定为 manifest.json、plan.json 和 download 下的制品。
    主程序使用退出码判断准备是否成功，不能用目录存在性判断。
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--package", required=True)
    parser.add_argument("--base-version", required=True)
    parser.add_argument("--revision", required=True, type=int)
    parser.add_argument("--output", required=True)
    parser.add_argument("--select", required=True, nargs="+")
    args = parser.parse_args()
    # 参数以值传递，不拼接 shell 命令；失败返回非零让 CI 停止。
    prepare(args.source, args.baseline, args.package, args.select,
            args.base_version, args.revision, args.output)


if __name__ == "__main__":
    main()
