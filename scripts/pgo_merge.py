#!/usr/bin/env python3
"""
pgo_merge.py — PGO profile 收集、下载、合并脚本

支持三种数据源:
  1) --input-dir <本地目录>            : 扫描 *.profraw，合并为 .profdata
  2) --source-url <URL 目录列表>        : 从 nginx autoindex 抓取 *.profraw 下载并合并
  3) --source-url <URL .profdata>       : 直接下载单个预合并文件

用法:
  # 本地目录合并
  python pgo_merge.py --input-dir /var/www/pgo/uploads \
    --output merged.profdata --min-files 5

  # 从服务器下载所有 .profraw 并合并
  python pgo_merge.py --source-url https://server.com/pgo/profiles/ \
    --output merged.profdata

  # 直接下载预合并的 .profdata
  python pgo_merge.py --source-url https://server.com/pgo/merged.profdata \
    --output merged.profdata

依赖:
  - llvm-profdata (合并模式)
"""

import argparse
import glob
import hashlib
import os
import re
import subprocess
import tempfile
import time
from datetime import datetime
from pathlib import Path
from urllib.parse import urldefrag, urljoin, urlparse
from urllib.request import urlopen, urlretrieve


# urlretrieve 每接收一个数据块调用本函数，用单行进度展示下载比例。
# total_size 不可信或未知时保持安静，避免除零和误导性百分比。
def _download_progress(block_num, block_size, total_size, label=""):
    if total_size <= 0:
        return
    done = block_num * block_size
    # 最后一个块可能超过声明长度，显示值上限固定为 100%。
    pct = min(100, int(done / total_size * 100))
    print(f"\r  [{label}] {pct}% ({done:,}/{total_size:,})", end="")
    if pct >= 100:
        # 下载结束后换行，避免后续状态文本覆盖进度行。
        print()


# =============================================================================
#  Source: local directory
# =============================================================================

def find_profiles_local(input_dir, max_age_days):
    """递归扫描本地目录，返回 .profraw 文件列表 (跳过空文件和过期文件)"""
    pattern = os.path.join(input_dir, "**", "*.profraw")
    # recursive glob 保留完整路径，后续 llvm-profdata 可直接消费。
    files = glob.glob(pattern, recursive=True)
    if not files:
        return [], 0

    # 新文件优先排列，使合并命令和日志在重复运行间更稳定。
    files.sort(key=os.path.getmtime, reverse=True)
    valid = []
    deleted = 0
    cutoff = time.time() - (max_age_days * 86400) if max_age_days > 0 else 0

    for f in files:
        # 元数据在删除判断前读取，文件并发消失会作为真实错误暴露。
        mtime = os.path.getmtime(f)
        size = os.path.getsize(f)

        if size == 0:
            # 空 profile 无法贡献覆盖率且可能使合并工具报警，直接清理。
            print(f"  [SKIP] empty: {os.path.basename(f)}")
            os.remove(f)
            deleted += 1
            continue

        if max_age_days > 0 and mtime < cutoff:
            # 过期策略只用于本地采样池，远程缓存随临时目录整体清理。
            age_days = int((time.time() - mtime) / 86400)
            print(f"  [EXPIRE] {os.path.basename(f)} (age={age_days}d)")
            os.remove(f)
            deleted += 1
            continue

        # 仅把非空且未过期文件传入合并阶段。
        valid.append(f)

    return valid, deleted


# =============================================================================
#  Source: remote URL
# =============================================================================

def _as_directory_url(url):
    """将 autoindex URL 规范化为目录形式。"""
    return url if url.endswith("/") else url + "/"


def _is_under_root_url(candidate_url, root_url):
    """判断候选 URL 是否仍位于入口目录树内。"""
    root = urlparse(_as_directory_url(root_url))
    # urlparse 分离 scheme、主机和路径，避免字符串前缀绕过域名边界。
    candidate = urlparse(candidate_url)
    if candidate.scheme != root.scheme or candidate.netloc != root.netloc:
        # autoindex 中的跨站链接绝不跟随。
        return False

    # 根路径补齐斜杠，避免 /profiles2 被误判为 /profiles 的子目录。
    root_path = root.path if root.path.endswith("/") else root.path + "/"
    return candidate.path.startswith(root_path)


# CMake 使用独立退出状态区分无可用样本与下载、读取或合并失败。
EXIT_NO_PROFILES = 3


def _cache_filename_for_profile(url):
    """以完整远端 URL 为缓存身份，路径及查询参数参与摘要。

    固定长度文件名避免扁平化后的别名、同 basename 和长路径碰撞。
    不使用下载顺序，使持续缓存不会在目录条目重排时复用错误内容。
    """
    # 源列表已经去除 URL 片段；它不属于服务器收到的请求。
    return hashlib.sha256(url.encode("utf-8")).hexdigest() + ".profraw"


def _scrape_autoindex(url, root_url=None, visited=None, seen_profiles=None):
    """从 nginx/apache autoindex 页面递归抓取所有 .profraw 链接。"""
    if root_url is None:
        # 首次调用固定安全根，递归调用不得扩大允许路径。
        root_url = _as_directory_url(url)
    if visited is None:
        # visited 阻止父目录链接或循环符号链接造成无限递归。
        visited = set()
    if seen_profiles is None:
        # 独立集合对跨目录重复链接的同一 profile 去重。
        seen_profiles = set()

    url = _as_directory_url(url)
    if url in visited:
        # 已访问目录不重复请求，也不重复返回其中的文件。
        return []
    visited.add(url)

    print(f"  Fetching directory listing: {url}")
    try:
        # 目录请求设置有限超时，配置阶段不能无限等待远程服务。
        with urlopen(url, timeout=30) as resp:
            html = resp.read().decode("utf-8", errors="replace")
    except Exception as e:
        # 入口或子目录不可读都会使远程数据集不完整，因此立即失败。
        raise RuntimeError(f"Failed to fetch directory listing: {e}") from e

    # 匹配 <a href="..."> 链接 (处理 nginx/apache autoindex 格式)
    links = re.findall(r'href="([^"]+)"', html, re.IGNORECASE)
    # 相对链接必须以当前目录为基准解析，再进行根目录边界检查。
    base_url = _as_directory_url(url)

    profraw_urls = []
    subdir_urls = []
    for link in links:
        # 页面锚点和查询排序链接不代表可下载文件。
        if not link or link.startswith(("#", "?")):
            continue
        if link.lower().startswith(("javascript:", "mailto:")):
            # 非 HTTP 导航协议不得交给 urlopen。
            continue

        full_url, _ = urldefrag(urljoin(base_url, link))
        if not _is_under_root_url(full_url, root_url):
            # 拒绝 ../、绝对跨域和同前缀旁路路径。
            continue

        parsed = urlparse(full_url)
        if parsed.path.lower().endswith(".profraw"):
            # 文件 URL 只加入一次，查询参数差异仍按完整 URL 区分。
            if full_url not in seen_profiles:
                seen_profiles.add(full_url)
                profraw_urls.append(full_url)
                print(f"    Found: {full_url}")
        elif parsed.path.endswith("/"):
            # 仅以斜杠结尾的链接视为可递归目录。
            subdir_urls.append(full_url)

    # 排序去重使递归请求顺序稳定，便于 CI 复现远程问题。
    for subdir_url in sorted(set(subdir_urls)):
        profraw_urls.extend(
            _scrape_autoindex(subdir_url, root_url, visited, seen_profiles)
        )

    return profraw_urls


def download_profiles_from_url(source_url, cache_dir):
    """下载目录样本或单个 profdata，返回已完成缓存的文件路径。

    临时文件不作为可命中缓存。失败下载或空响应不会成为下一轮的样本，
    每个完整请求最多在结果中占一个位置。
    """
    parsed = urlparse(source_url)
    # 后缀判断只查看 URL path，不受查询字符串影响。
    path = parsed.path

    # 缓存根通常是临时目录，但函数也支持调用方提供的持久目录。
    os.makedirs(cache_dir, exist_ok=True)

    # 如果 URL 指向单个 .profdata 文件 → 直接下载
    if path.endswith(".profdata"):
        # 预合并文件使用固定缓存名，返回标志阻止再次调用 merge。
        out_path = os.path.join(cache_dir, "downloaded.profdata")
        print(f"  Downloading pre-merged profile: {source_url}")
        urlretrieve(source_url, out_path, reporthook=_download_progress)
        print()
        size = os.path.getsize(out_path)
        # 大小用于诊断空响应或代理错误页面，不改变接受条件。
        print(f"  Downloaded: {out_path} ({size:,} bytes)")
        return out_path, True  # True = already merged

    # 目录模式: 递归抓取链接
    profraw_urls = _scrape_autoindex(source_url)

    if not profraw_urls:
        # 可访问的目录没有采样时与空本地输入共享跳过状态。
        return [], False

    print(f"  Downloading {len(profraw_urls)} profile(s) ...")
    downloaded = []
    for i, url in enumerate(profraw_urls):
        name = _cache_filename_for_profile(url)
        cached = Path(cache_dir) / name
        if cached.is_file() and cached.stat().st_size > 0:
            print(f"  [{i+1}/{len(profraw_urls)}] Cached: {name}")
        else:
            print(f"  [{i+1}/{len(profraw_urls)}] Downloading: {name}")
            try:
                # 同卷独占暂存使中断的下载永远不会出现在正式缓存路径。
                with tempfile.TemporaryDirectory(prefix="incoming-", dir=cache_dir) as pending:
                    partial = Path(pending) / "sample"
                    urlretrieve(url, partial, reporthook=_download_progress)
                    print()
                    if partial.stat().st_size == 0:
                        raise OSError("empty profile response")
                    partial.replace(cached)
            except OSError as error:
                # 单文件失败只跳过该项，所有传输都失败时另行报告源错误。
                print(f"    WARNING: download failed: {error}")
                continue
        downloaded.append(str(cached))

    if not downloaded:
        raise RuntimeError("No profiles downloaded")
    return downloaded, False


def run_profdata(profdata_bin, arguments):
    """运行配套 LLVM 工具，将错误统一为一次失败而非脚本退出码碰撞。"""
    # 参数列表直接传给 subprocess；十分钟上限覆盖大型采样池。
    result = subprocess.run([profdata_bin, *arguments], capture_output=True,
                            text=True, timeout=600)
    if result.returncode != 0:
        # LLVM 即使返回 3 也属于合并失败，不能误报为本脚本的样本不足。
        raise RuntimeError(f"llvm-profdata {arguments[0]} failed "
                           f"(rc={result.returncode}): {result.stderr[:500]}")


def publish_profile(args, profiles, pre_merged=False):
    """生成本轮的完整 profile；只有验证成功后才替换调用方的输出。"""
    if not pre_merged and (not profiles or len(profiles) < args.min_files):
        print(f"  SKIP: need >= {args.min_files} profiles, only {len(profiles)} available")
        return EXIT_NO_PROFILES

    output = Path(args.output).absolute()
    # LLVM 即使在失败前创建了文件，也只能污染本次暂存目录。
    # 目录放在输出所在卷，避免最终替换变成不具备原子性的跨卷复制。
    with tempfile.TemporaryDirectory(prefix=".pgo-output-", dir=output.parent) as pending:
        profile = Path(pending) / "profile.profdata"
        if pre_merged:
            # 将远端单文件也交给当前工具写出索引格式。show 可读取 raw，
            # 仅凭 show 成功不能保证 Clang 的 profile-use 能消费同一份文件。
            print("  Validating and indexing downloaded profile")
            profiles = [profiles]
        print(f"  Merging {len(profiles)} profiles")
        run_profdata(args.profdata, ["merge", "-o", str(profile), *profiles])
        size = profile.stat().st_size
        if size == 0:
            raise RuntimeError("llvm-profdata did not produce a nonempty profile")
        profile.replace(output)
    print(f"  Profile ready: {output} ({size:,} bytes)")
    return 0


def parse_args():
    # 数据源互斥组确保本地与远程模式不会同时生效。
    p = argparse.ArgumentParser(description="PGO profile merge & download tool")

    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--input-dir", help="Local directory with .profraw files")
    src.add_argument("--source-url", help="URL to download .profraw file(s) from "
                    "(autoindex directory or single .profdata)")

    p.add_argument("--output", required=True, help="Output .profdata path")
    # 工具路径可由 CMake 传入与当前 clang 配套的 llvm-profdata。
    p.add_argument("--profdata", default="llvm-profdata",
                   help="Path to llvm-profdata binary")
    p.add_argument("--max-age-days", type=int, default=90,
                   help="Expire .profraw older than N days (local mode only, 0=keep all)")
    p.add_argument("--min-files", type=int, default=1,
                   # 门槛用于避免以过少采样误导发布优化。
                   help="Minimum .profraw files required to merge")
    return p.parse_args()


def main():
    """运行指定来源，返回成功、样本不足或错误的不同状态。"""
    args = parse_args()
    print(f"=== PGO Merge [{datetime.now().isoformat()}] ===")
    try:
        if args.source_url:
            print(f"  Source: {args.source_url}")
            # 远端缓存覆盖下载、样本门槛和合并，任一退出路径都由作用域清理。
            with tempfile.TemporaryDirectory(prefix="pgo_dl_") as cache_dir:
                payload, pre_merged = download_profiles_from_url(args.source_url, cache_dir)
                return publish_profile(args, payload, pre_merged)

        # 本地输入归调用方所有，绝不把它交给临时缓存的清理流程。
        if not os.path.isdir(args.input_dir):
            raise OSError(f"input-dir not found: {args.input_dir}")
        print(f"  Input: {args.input_dir}")
        profiles, deleted = find_profiles_local(args.input_dir, args.max_age_days)
        print(f"  Profiles found: {len(profiles)} (expired/deleted: {deleted})")
        return publish_profile(args, profiles)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"  FATAL: {error}")
        return 1


if __name__ == "__main__":
    # 导入时仅暴露可测试函数，不解析命令行或访问网络。
    raise SystemExit(main())
