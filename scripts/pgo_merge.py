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
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime
from urllib.parse import urljoin, urlparse
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


def _cache_filename_for_profile(url, index):
    """根据远程路径生成缓存文件名，避免递归下载时同名文件覆盖。"""
    path = urlparse(url).path.strip("/")
    # 将远程目录层级压平到安全文件名，同时保留足够来源信息。
    safe_name = re.sub(r"[^A-Za-z0-9_.-]+", "_", path)
    if not safe_name:
        # URL 没有可用路径时以稳定序号生成合法 profraw 名称。
        safe_name = f"profile_{index + 1}.profraw"
    if len(safe_name) > 180:
        # 限制文件名长度，长层级仅保留序号与 basename。
        safe_name = f"{index + 1:05d}_{os.path.basename(path)}"
    return safe_name


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
        print(f"  FATAL: Failed to fetch directory listing: {e}")
        sys.exit(1)

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

        full_url = urljoin(base_url, link)
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
    """从远程下载所有 .profraw 文件到缓存目录"""
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
        # 空远程集合不能生成有效 profdata，明确返回失败。
        print("  FATAL: No .profraw files found at the URL")
        sys.exit(1)

    print(f"  Downloading {len(profraw_urls)} profile(s) ...")
    downloaded = []
    for i, u in enumerate(profraw_urls):
        # 缓存名包含远程路径，避免不同子目录 basename 相互覆盖。
        fname = _cache_filename_for_profile(u, i)
        out = os.path.join(cache_dir, fname)
        if os.path.exists(out) and os.path.getsize(out) > 0:
            # 非空缓存直接复用，支持持久 cache_dir 的断点续传。
            print(f"  [{i+1}/{len(profraw_urls)}] Cached: {fname}")
        else:
            print(f"  [{i+1}/{len(profraw_urls)}] Downloading: {fname}")
            try:
                # 单文件失败只跳过该项，最终至少需要一个成功下载。
                urlretrieve(u, out, reporthook=_download_progress)
                print()
            except Exception as e:
                # 保留失败 URL 的上下文并继续收集其他 profile。
                print(f"    WARNING: download failed: {e}")
                continue
        # 只有已存在缓存或成功下载的路径进入合并列表。
        downloaded.append(out)

    if not downloaded:
        print("  FATAL: No profiles downloaded")
        sys.exit(1)

    return downloaded, False  # needs merge


# =============================================================================
#  Merge + main
# =============================================================================

def merge_profiles(profdata_bin, profiles, output):
    """调用 llvm-profdata merge"""
    # 参数列表直接传给 subprocess，路径不会经过 shell 展开。
    cmd = [profdata_bin, "merge", "-o", output] + profiles
    print(f"  Merging {len(profiles)} profiles → {output}")
    print(f"  Command: {' '.join(cmd[:3])} [...]")

    try:
        # 十分钟上限覆盖大型采样池，同时避免 CI 永久挂起。
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        if result.returncode != 0:
            # stderr 截断为前 500 字符，避免异常工具输出淹没配置日志。
            print(f"  MERGE FAILED (rc={result.returncode})")
            print(f"  stderr: {result.stderr[:500]}")
            sys.exit(result.returncode)
    except subprocess.TimeoutExpired:
        # 超时结果不可用，调用方不得继续发布旧输出。
        print("  MERGE TIMEOUT")
        sys.exit(1)
    except FileNotFoundError:
        # 明确区分工具缺失与 profile 内容合并失败。
        print(f"  FATAL: {profdata_bin} not found")
        sys.exit(1)

    # 成功返回后读取最终文件大小，确认工具确实创建输出。
    size = os.path.getsize(output)
    print(f"  Merge OK: {output} ({size:,} bytes)")


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
    # 时间戳只用于日志关联，不进入输出文件命名。
    args = parse_args()
    print(f"=== PGO Merge [{datetime.now().isoformat()}] ===")

    # 仅远程模式创建临时缓存；本地输入目录绝不作为清理目标。
    cache_dir = None  # track temp dir for cleanup

    # --- Determine source ---
    if args.source_url:
        # 远程下载模式
        print(f"  Source: {args.source_url}")
        cache_dir = tempfile.mkdtemp(prefix="pgo_dl_")
        # 返回值可能是单个 profdata 路径或 profraw 路径列表。
        profiles_or_file, pre_merged = download_profiles_from_url(
            args.source_url, cache_dir
        )

        if pre_merged:
            # 直接下载的 .profdata — 拷贝到输出
            print(f"  Using pre-merged profile directly")
            shutil.copy2(profiles_or_file, args.output)
        else:
            # 需要合并
            # 低于门槛属于暂不生成新优化数据，按约定返回成功跳过。
            if len(profiles_or_file) < args.min_files:
                print(f"  SKIP: need >= {args.min_files} profiles, "
                      f"only {len(profiles_or_file)} downloaded")
                sys.exit(0)
            merge_profiles(args.profdata, profiles_or_file, args.output)

    elif args.input_dir:
        # 本地目录模式
        print(f"  Input:  {args.input_dir}")
        print(f"  Output: {args.output}")

        if not os.path.isdir(args.input_dir):
            # 输入路径必须是目录，普通文件不会隐式解释为 profdata。
            print(f"  FATAL: input-dir not found: {args.input_dir}")
            sys.exit(1)

        profiles, deleted = find_profiles_local(
            args.input_dir, args.max_age_days
        )
        print(f"  Profiles found: {len(profiles)} (expired/deleted: {deleted})")

        if len(profiles) < args.min_files:
            # 本地门槛语义与远程模式一致，便于 CMake 统一判断返回码。
            print(f"  SKIP: need >= {args.min_files} profiles, "
                  f"only {len(profiles)} available")
            sys.exit(0)

        merge_profiles(args.profdata, profiles, args.output)

    # --- Cleanup ---
    if cache_dir and os.path.isdir(cache_dir):
        # 只删除本次 mkdtemp 返回的远程缓存，不触碰用户采样目录。
        shutil.rmtree(cache_dir, ignore_errors=True)

    print("=== Done ===")


if __name__ == "__main__":
    # 导入时仅暴露可测试函数，不解析命令行或访问网络。
    main()
