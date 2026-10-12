#!/usr/bin/env python3
"""生成 MusicMapMaker-Next 的签名 APT 仓库。

只接收既有签名密钥，绝不生成或导出私钥。输出是可直接作为
https://mmm.xiang233.top/apt/ 文档根目录发布的普通目录。
规范依据：https://wiki.debian.org/DebianRepository/Format
以及 Debian 的 apt-secure(8)、apt-ftparchive(1) 手册。
"""

import argparse
import gzip
import hashlib
import html
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


# /// @brief 本工具的所有权边界，不扫描并接管目录中其它文件。
MANIFEST = ".mmm-apt-manifest.json"
# /// @brief 固定导出名称，便于包安装脚本使用同一 Signed-By 地址。
KEYRING = "mmm-archive-keyring.gpg"


# /// @brief 报告契约违反并停止；调用者无需建立异常恢复分支。
def fail(message):
    # 仅诊断预期失败，不将用户传入内容交给 shell 或格式化命令。
    sys.stderr.write(f"APT repository: {message}\n")
    sys.exit(1)


# /// @brief 执行工具并检查退出码；不经过 shell，避免路径被再次解释。
# /// @param command 已拆分的参数列表，路径与参数值不做字符串拼接执行。
# /// @param cwd 扫描索引时使用的归档根；其它工具沿用项目根。
# /// @return 原始标准输出字节，工具失败则结束当前发布。
def run(command, cwd=None):
    # 捕获二进制输出：公钥导出和索引生成都不能经过文本编码转换。
    result = subprocess.run(command, cwd=cwd, capture_output=True, check=False)
    # 外部工具失败时保留其诊断；发布动作尚未开始，旧仓库仍可使用。
    if result.returncode:
        sys.stderr.write(result.stderr.decode("utf-8", errors="replace"))
        fail(f"{command[0]} exited with {result.returncode}")
    return result.stdout


# /// @brief 分块计算 SHA256，避免把大型 DEB 整体载入内存。
# /// @param path 已通过边界检查的本地普通文件。
# /// @return 小写十六进制 SHA256，符合 Debian 元数据格式。
def digest(path):
    # 已归档包及受管文件都使用同一算法，避免验证口径不一致。
    result = hashlib.sha256()
    with path.open("rb") as source:
        # 每轮内存占用固定，归档历史增长不会扩大单文件内存用量。
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


# /// @brief 验证受管理路径，拒绝逃逸、符号链接和非目录祖先。
# /// @param root 明确授权的仓库根，或者检查根路径时的文件系统根。
# /// @param relative 尚未解析链接的逻辑相对路径。
# /// @return 位于 root 内且没有符号链接祖先的绝对路径。
def safe_path(root, relative):
    # 根是绝对路径；所有后续 I/O 因而具有显式、稳定的文件位置。
    value = Path(relative)
    # 状态文件即使被手工编辑，也不能授权覆盖输出目录以外的文件。
    if value.is_absolute() or not value.parts or any(
        part in (".", "..") for part in value.parts
    ):
        fail(f"unsafe managed path: {relative}")
    path = root
    for index, part in enumerate(value.parts):
        path = path / part
        # 包括叶节点在内都不跟随链接，避免改写链接指向的用户文件。
        if path.is_symlink():
            fail(f"refusing symlink: {path}")
        if index < len(value.parts) - 1 and path.exists() and not path.is_dir():
            fail(f"parent is not a directory: {path}")
    return path


# /// @brief 读取本工具的所有权清单并验证已发布文件未被外部修改。
# /// @param args 当前 suite 与 package-name 身份，不能复用其它身份的清单。
# /// @return 可安全更新的文件映射，首次初始化返回空映射。
def read_state(root, args):
    # 清单不存在表示首次初始化，后续碰撞检查仍保护既有用户文件。
    path = safe_path(root, MANIFEST)
    if not path.exists():
        return {}
    state = json.loads(path.read_text(encoding="utf-8"))
    # 仓库标识不能静默变化，否则 APT 会要求客户端确认 Release 变化。
    expected = {"format": 1, "suite": args.suite, "package": args.package_name}
    if any(state.get(key) != value for key, value in expected.items()):
        fail("manifest belongs to another repository configuration")
    files = state.get("files")
    # 只接受路径到 SHA256 的映射，不把其它格式猜测为历史归档列表。
    if not isinstance(files, dict):
        fail("manifest has no managed file map")
    for relative, checksum in files.items():
        owned = safe_path(root, relative)
        # 文件消失或内容改变时拒绝修复性覆盖，交由维护者核实原因。
        if not owned.is_file() or digest(owned) != checksum:
            fail(f"managed file was removed or changed: {relative}")
    return files


# /// @brief 读取 DEB 的真实元数据，按包名、版本和架构确定归档身份。
# /// @param package_name 单一发布包的身份约束，拒绝误输入其它产品的 DEB。
# /// @return 规范归档相对路径及包的真实架构，不依赖原始文件名。
def metadata(path, package_name):
    # dpkg-deb 已解析归档结构，无需自行解压或执行包内维护脚本。
    control = run(["dpkg-deb", "--field", str(path)]).decode("utf-8")
    fields = dict(line.split(": ", 1) for line in control.splitlines()
                  if not line.startswith((" ", "\t")) and ": " in line)
    # 身份字段都是单行字段，Description 等续行不参与路径构造。
    package = fields.get("Package", "")
    version = fields.get("Version", "")
    architecture = fields.get("Architecture", "")
    # 保留 Debian 版本中的 epoch，但禁止任何路径或控制字符。
    if package != package_name or not re.fullmatch(r"[a-z0-9][a-z0-9+.-]+", package):
        fail(f"unexpected Package in {path}: {package}")
    if not re.fullmatch(r"[0-9][A-Za-z0-9.+:~\-]*", version):
        fail(f"unsafe Version in {path}: {version}")
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", architecture):
        fail(f"unsafe Architecture in {path}: {architecture}")
    # 采用真实架构重命名，apt-ftparchive 的 --arch 才不会错选文件。
    name = f"{package}_{version}_{architecture}.deb"
    return f"pool/main/{package[0]}/{package}/{name}", architecture


# /// @brief 生成浏览器可见的下载页，使用同一批实际归档生成下载链接。
# /// @param packages 已验证的历史与新包集合，不能链接尚未发布的文件。
# /// @param suite 当前签名索引发行名，页面入口必须与 APT 路径一致。
# /// @return UTF-8 页面字节，随归档事务统一发布。
def download_page(packages, suite, cdn_base_url):
    # 页面不使用外部脚本、字体或 CDN，离线复制文档根也能直接浏览。
    rows = []
    for relative, (source, architecture) in sorted(packages.items()):
        # 控制字段来自归档本身，不能依据调用方提供的文件名推测版本。
        version = run(["dpkg-deb", "--field", str(source), "Version"]).decode().strip()
        # 所有动态文本和链接都转义，控制字段不能插入页面标记。
        label = html.escape(f"MusicMapMaker-Next {version} · {architecture}")
        link = html.escape(f"{cdn_base_url}/{relative}" if cdn_base_url else relative, quote=True)
        # 文件名同时用于安装说明，让用户下载后能识别版本和目标架构。
        rows.append(f'<li><a class="download" href="{link}">{label} — 下载 DEB</a></li>')
    # 浏览页是说明入口；APT 仍从签名的 dists 索引读取包列表。
    release = html.escape(f"dists/{suite}/InRelease", quote=True)
    # 安装本地 DEB 会执行包内维护脚本，自动写入限定公钥的更新源。
    install = "sudo apt install ./musicmapmaker-next_版本_amd64.deb"
    # 使用相对地址兼容部署前缀，/apt 到 /apt/ 的重定向由服务配置负责。
    page = f'''<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>MusicMapMaker-Next · Linux 下载与更新源</title>
<style>body{{font:17px/1.7 system-ui,sans-serif;background:#111827;color:#e5e7eb;max-width:850px;margin:5vh auto;padding:24px}}a{{color:#7dd3fc}}h1{{line-height:1.2}}li{{margin:18px 0}}pre{{white-space:pre-wrap;background:#1f2937;padding:20px;border-radius:12px}}.download{{display:inline-block;padding:12px 18px;background:#075985;border-radius:10px;color:white;text-decoration:none}}small{{color:#9ca3af}}</style>
<h1>MusicMapMaker-Next</h1><p>Linux 下载 · APT 自动更新</p>
<p>下载 DEB 并安装，即可通过系统包管理器获取后续更新。</p>
<ul>{''.join(rows)}</ul>
<h2>安装与更新</h2><p>下载适合架构的 DEB，在下载目录执行（将文件名替换为实际下载文件）：</p>
<pre>{install}</pre><p>安装时自动添加 MMM 软件源。以后通过包管理器更新：</p>
<pre>sudo apt update
sudo apt upgrade musicmapmaker-next</pre>
<h2>Gentoo</h2><p><a href="https://github.com/xiangCaO233/mmm-gentoo-overlay">打开 Gentoo overlay 与安装说明</a>：源码版 9999 跟随 ci 分支 HEAD；二进制版从本源的 DEB 提取程序。</p>
<p><a href="{release}">签名索引</a> · <a href="mmm-archive-keyring.gpg">软件源公钥</a> · <a href="https://github.com/xiangCaO233/MusicMapMaker-Next">项目源码</a></p>
<small>软件源地址：https://mmm.xiang233.top/apt/ · stable / main</small></html>
'''
    # 与包、索引一样由发布清单接管，后续发布会刷新而不遗留过期版本页。
    return page.encode("utf-8")


# /// @brief 在隔离目录完整生成归档和签名，成功后返回待发布文件集合。
# /// @param state 验证过的旧文件映射，承担历史归档保留责任。
# /// @param stage 本次唯一临时区，签名工具的全部输出位于此处。
# /// @return 相对发布路径到暂存文件的映射，失败时不修改已发布索引。
def generate(root, args, state, stage):
    # 只发布单个包家族；安装包与仓库归档路径共享同一 package-name。
    packages = {}
    pool_prefix = f"pool/main/{args.package_name[0]}/{args.package_name}/"
    for relative in state:
        # 清单中的历史 DEB 是下一代索引的输入，不能只索引本次参数。
        if relative.startswith(pool_prefix) and relative.endswith(".deb"):
            packages[relative] = (root / relative, metadata(root / relative, args.package_name)[1])
    for input_path in args.debs:
        # 输入允许常规软链接，输出边界仍由独立的 safe_path 验证负责。
        source = Path(input_path).resolve()
        if not source.is_file():
            fail(f"DEB does not exist: {source}")
        relative, architecture = metadata(source, args.package_name)
        # 同身份不同内容会使缓存与包校验失效，必须发布新的 Debian 版本。
        if relative in packages and digest(packages[relative][0]) != digest(source):
            fail(f"same package version has different content: {relative}")
        packages[relative] = (source, architecture)
    architectures = sorted(set(args.architecture) | {
        architecture for _, architecture in packages.values() if architecture != "all"
    })
    # 架构集合只增不减：历史版本仍存在时其下载入口必须继续可用。
    # 仅有 Architecture: all 时仍提供默认机器架构入口。
    if not architectures:
        architectures = ["amd64"]
    generated = {}
    for relative, (source, _) in sorted(packages.items()):
        # 稳定排序便于审阅索引；不会依赖输入参数或文件系统枚举顺序。
        target = stage / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        # 历史包位于同一文件系统，硬链接避免每次更新重复制归档。
        # 新输入仅复制到暂存区，签名失败时不会留下新发布的 DEB。
        if relative in state:
            os.link(root / relative, target)
        else:
            shutil.copyfile(source, target)
        generated[relative] = target
    # 下载入口与包列表使用同一批输入，不要求维护者另行编辑静态网页。
    index = stage / "index.html"
    index.write_bytes(download_page(packages, args.suite, args.cdn_base_url))
    generated["index.html"] = index
    # 先上传并验证全部归档，再签名发布索引，避免 APT 索引指向不存在的 CDN 文件。
    if args.cdn_uploader:
        run([args.cdn_python, args.cdn_uploader, "--base-url", args.cdn_base_url,
             *(str(generated[relative]) for relative in sorted(packages))])
    distribution = stage / "dists" / args.suite
    distribution.mkdir(parents=True)
    for architecture in architectures:
        # packages 为每个归档输出独立条目，天然保留历史版本而不只选最新包。
        # --arch 会自然包含 all 包，因此无需创建独立 binary-all 目录。
        index_dir = distribution / "main" / f"binary-{architecture}"
        index_dir.mkdir(parents=True)
        data = run(["apt-ftparchive", "--arch", architecture, "packages", "pool"], cwd=stage)
        # 未压缩与压缩索引同时提供；gzip 时间戳固定以稳定索引哈希。
        for name, contents in (("Packages", data), ("Packages.gz", gzip.compress(data, mtime=0))):
            index = index_dir / name
            index.write_bytes(contents)
            generated[index.relative_to(stage).as_posix()] = index
            # 发布不可变的按哈希索引；历史地址由清单保留而不清理。
            # apt-ftparchive 同时提供 SHA512，APT 可以优先选择更强的 hash。
            # 必须提供两种地址，避免客户端回退普通索引而失去跨发布一致性。
            for algorithm in ("SHA256", "SHA512"):
                checksum = hashlib.new(algorithm.lower(), contents).hexdigest()
                hashed = index_dir / "by-hash" / algorithm / checksum
                hashed.parent.mkdir(parents=True, exist_ok=True)
                hashed.write_bytes(contents)
                generated[hashed.relative_to(stage).as_posix()] = hashed
    options = {"Origin": "MusicMapMaker-Next", "Label": "MusicMapMaker-Next",
               "Suite": args.suite, "Codename": args.suite, "Components": "main",
               "Architectures": " ".join(architectures), "Acquire-By-Hash": "yes",
               "Description": "MusicMapMaker-Next packages"}
    # 官方工具统一生成 Release 的尺寸、日期和各校验字段。
    # 散列子目录中的文件名不匹配索引模式，不会污染 Release 索引列表。
    command = ["apt-ftparchive"]
    for key, value in options.items():
        command += ["-o", f"APT::FTPArchive::Release::{key}={value}"]
    release = distribution / "Release"
    release.write_bytes(run(command + ["release", str(distribution)]))
    gpg = ["gpg", "--batch", "--yes"]
    if args.gnupg_home:
        # 用户指定的测试或生产身份目录始终以绝对路径交给 GnuPG。
        gpg += ["--homedir", str(Path(args.gnupg_home).resolve())]
    # 完整指纹精确选择用户指定的钥；GnuPG agent 负责既有钥的解锁。
    signing = gpg + ["--local-user", args.signing_key, "--digest-algo", "SHA256"]
    # 双签名提供现代客户端的 InRelease 与旧客户端的分离签名入口。
    run(signing + ["--output", str(distribution / "InRelease"), "--clearsign", str(release)])
    run(signing + ["--armor", "--output", str(distribution / "Release.gpg"),
                   "--detach-sign", str(release)])
    keyring = stage / KEYRING
    # export-minimal 只导出公钥包；输出二进制 .gpg 可供 Signed-By 使用。
    exported = run(gpg + ["--export-options", "export-minimal", "--export", args.signing_key])
    if not exported:
        fail("signing public key export was empty")
    keyring.write_bytes(exported)
    for name in ("Release", "Release.gpg", "InRelease"):
        generated[f"dists/{args.suite}/{name}"] = distribution / name
    generated[KEYRING] = keyring
    return generated


# /// @brief 预检所有目标后原子替换文件，最后发布客户端优先读取的 InRelease。
# /// @param generated 已完整签名的文件集合，不接受生成过程中的部分结果。
# /// @warning 原子性按文件保证；客户端依靠 by-hash 读取一致的索引集合。
# /// @warning 维护者不得与生成器同时手工编辑受管目录或清单。
def publish(root, args, state, generated):
    # 预检完整集合，避免发布一半才发现另一个目标属于操作者。
    for relative in generated:
        target = safe_path(root, relative)
        # 不接管手工放置的包、索引或公钥，即使内容恰巧相同也拒绝覆盖。
        if target.exists() and relative not in state:
            fail(f"unmanaged output file already exists: {relative}")
    checksums = dict(state)
    # 包、不可变哈希索引和公钥先就绪，再更新 Release 三件套。
    # 客户端持有旧 InRelease 时仍能从保留的 by-hash 地址读取旧索引。
    publication_order = {"Release": 1, "Release.gpg": 2, "InRelease": 3}
    order = sorted(generated, key=lambda name: (publication_order.get(Path(name).name, 0), name))
    for relative in order:
        source = generated[relative]
        checksum = digest(source)
        target = safe_path(root, relative)
        # 相同文件不替换；保留历史包的 inode 和旧哈希索引。
        if state.get(relative) != checksum:
            target.parent.mkdir(parents=True, exist_ok=True)
            source.chmod(0o644)
            os.replace(source, target)
        checksums[relative] = checksum
    state_path = next(iter(generated.values())).parents[0] / "manifest-publish.json"
    # 临时清单留在隔离区，清理阶段只移除本次创建的临时内容。
    # 清单最后提交，记录确实完成的文件集合；无主动删除操作。
    state_path.write_text(json.dumps({"format": 1, "suite": args.suite,
                                     "package": args.package_name, "files": checksums},
                                    indent=2, sort_keys=True) + "\n", encoding="utf-8")
    state_path.chmod(0o644)
    os.replace(state_path, root / MANIFEST)


# /// @brief 解析显式发布参数并串行更新一个仓库，不提供 unsigned 模式。
# /// @return 发布完成时返回零；预期契约失败通过非零退出码报告。
# /// @warning 不维护清理策略，历史包与 by-hash 地址一直保留。
# /// @warning 生产签名钥须预先配置，脚本不会创建、存储或显示私钥。
def main():
    # 发布需要输入 DEB 和签名身份；缺少任一项由 argparse 直接拒绝。
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, help="repository document root")
    parser.add_argument("--signing-key", required=True, help="full signing key fingerprint")
    parser.add_argument("--gnupg-home", help="existing isolated GnuPG home")
    parser.add_argument("--suite", default="stable")
    parser.add_argument("--package-name", default="musicmapmaker-next")
    parser.add_argument("--architecture", action="append", default=[],
                        help="additional binary index architecture; repeatable")
    # 测试与离线仓库可保留相对下载路径；正式发布显式启用既有 R2 分发。
    parser.add_argument("--cdn-base-url", default="")
    parser.add_argument("--cdn-uploader", help="server-side R2 upload helper")
    parser.add_argument("--cdn-python", default="python3")
    parser.add_argument("debs", nargs="+", help="DEB files to add; existing versions remain")
    args = parser.parse_args()
    # CDN 地址只接受 HTTPS 域名，不能混入认证材料、查询串或额外路径。
    if args.cdn_base_url and not re.fullmatch(r"https://[a-z0-9][a-z0-9.-]*", args.cdn_base_url):
        fail("invalid CDN base URL")
    if args.cdn_uploader and not args.cdn_base_url:
        fail("CDN uploader requires a CDN base URL")
    # 不允许短 key ID，避免不同钥碰撞后不明确的签名选择。
    if not re.fullmatch(r"(?:[A-Fa-f0-9]{40}|[A-Fa-f0-9]{64})", args.signing_key):
        fail("--signing-key must be a full fingerprint")
    if not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", args.suite):
        # suite 是目录组成部分，禁止分隔符和控制字符进入布局。
        fail("invalid suite")
    if not re.fullmatch(r"[a-z0-9][a-z0-9+.-]+", args.package_name):
        fail("invalid package name")
    if any(not re.fullmatch(r"[a-z0-9][a-z0-9-]*", value) or value == "all"
           for value in args.architecture):
        fail("invalid binary architecture")
    for tool in ("dpkg-deb", "apt-ftparchive", "gpg", "flock"):
        # 在创建输出前完成依赖检查，缺少工具不会产生半成品仓库。
        if not shutil.which(tool):
            fail(f"required tool is unavailable: {tool}")
    root = Path(args.output).absolute()
    # 验证绝对路径各级，防止根目录或祖先符号链接绕过输出边界。
    safe_path(Path(root.anchor), str(root.relative_to(root.anchor)))
    root.mkdir(parents=True, exist_ok=True)
    lock_path = safe_path(root, ".mmm-apt.lock")
    # 只在发布过程持锁；非阻塞探测让第二个发布者立即失败。
    with lock_path.open("a+b") as lock:
        available = subprocess.run(["flock", "--nonblock", str(lock.fileno())],
                                   pass_fds=(lock.fileno(),), check=False).returncode
        if available:
            fail("another publisher is updating this repository")
        # 子进程沿用同一打开文件描述，锁一直保持到父进程关闭 lock。
        state = read_state(root, args)
        # 临时区与输出同文件系统，os.replace 的原子性不依赖跨设备复制。
        with tempfile.TemporaryDirectory(prefix=".mmm-apt-stage-", dir=root) as directory:
            generated = generate(root, args, state, Path(directory))
            publish(root, args, state, generated)
    sys.stdout.write(f"Signed repository ready: {root}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
