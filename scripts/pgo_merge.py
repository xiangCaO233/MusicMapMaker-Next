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


# 进度回调
def _download_progress(block_num, block_size, total_size, label=""):
    if total_size <= 0:
        return
    done = block_num * block_size
    pct = min(100, int(done / total_size * 100))
    print(f"\r  [{label}] {pct}% ({done:,}/{total_size:,})", end="")
    if pct >= 100:
        print()


# =============================================================================
#  Source: local directory
# =============================================================================

def find_profiles_local(input_dir, max_age_days):
    """扫描本地目录，返回 .profraw 文件列表 (跳过空文件和过期文件)"""
    pattern = os.path.join(input_dir, "*.profraw")
    files = glob.glob(pattern)
    if not files:
        return [], 0

    files.sort(key=os.path.getmtime, reverse=True)
    valid = []
    deleted = 0
    cutoff = time.time() - (max_age_days * 86400) if max_age_days > 0 else 0

    for f in files:
        mtime = os.path.getmtime(f)
        size = os.path.getsize(f)

        if size == 0:
            print(f"  [SKIP] empty: {os.path.basename(f)}")
            os.remove(f)
            deleted += 1
            continue

        if max_age_days > 0 and mtime < cutoff:
            age_days = int((time.time() - mtime) / 86400)
            print(f"  [EXPIRE] {os.path.basename(f)} (age={age_days}d)")
            os.remove(f)
            deleted += 1
            continue

        valid.append(f)

    return valid, deleted


# =============================================================================
#  Source: remote URL
# =============================================================================

def _scrape_autoindex(url):
    """从 nginx/apache autoindex 页面抓取所有 .profraw 链接"""
    print(f"  Fetching directory listing: {url}")
    try:
        with urlopen(url, timeout=30) as resp:
            html = resp.read().decode("utf-8", errors="replace")
    except Exception as e:
        print(f"  FATAL: Failed to fetch directory listing: {e}")
        sys.exit(1)

    # 匹配 <a href="..."> 链接 (处理 nginx/apache autoindex 格式)
    links = re.findall(r'href="([^"]+)"', html, re.IGNORECASE)
    base_url = url if url.endswith("/") else url + "/"

    profraw_urls = []
    for link in links:
        if link.endswith(".profraw"):
            full_url = urljoin(base_url, link)
            profraw_urls.append(full_url)
            print(f"    Found: {link}")

    return profraw_urls


def download_profiles_from_url(source_url, cache_dir):
    """从远程下载所有 .profraw 文件到缓存目录"""
    parsed = urlparse(source_url)
    path = parsed.path

    os.makedirs(cache_dir, exist_ok=True)

    # 如果 URL 指向单个 .profdata 文件 → 直接下载
    if path.endswith(".profdata"):
        out_path = os.path.join(cache_dir, "downloaded.profdata")
        print(f"  Downloading pre-merged profile: {source_url}")
        urlretrieve(source_url, out_path, reporthook=_download_progress)
        print()
        size = os.path.getsize(out_path)
        print(f"  Downloaded: {out_path} ({size:,} bytes)")
        return out_path, True  # True = already merged

    # 目录模式: 抓取链接
    profraw_urls = _scrape_autoindex(source_url)

    if not profraw_urls:
        print("  FATAL: No .profraw files found at the URL")
        sys.exit(1)

    print(f"  Downloading {len(profraw_urls)} profile(s) ...")
    downloaded = []
    for i, u in enumerate(profraw_urls):
        fname = os.path.basename(urlparse(u).path)
        out = os.path.join(cache_dir, fname)
        if os.path.exists(out) and os.path.getsize(out) > 0:
            print(f"  [{i+1}/{len(profraw_urls)}] Cached: {fname}")
        else:
            print(f"  [{i+1}/{len(profraw_urls)}] Downloading: {fname}")
            try:
                urlretrieve(u, out, reporthook=_download_progress)
                print()
            except Exception as e:
                print(f"    WARNING: download failed: {e}")
                continue
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
    cmd = [profdata_bin, "merge", "-o", output] + profiles
    print(f"  Merging {len(profiles)} profiles → {output}")
    print(f"  Command: {' '.join(cmd[:3])} [...]")

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        if result.returncode != 0:
            print(f"  MERGE FAILED (rc={result.returncode})")
            print(f"  stderr: {result.stderr[:500]}")
            sys.exit(result.returncode)
    except subprocess.TimeoutExpired:
        print("  MERGE TIMEOUT")
        sys.exit(1)
    except FileNotFoundError:
        print(f"  FATAL: {profdata_bin} not found")
        sys.exit(1)

    size = os.path.getsize(output)
    print(f"  Merge OK: {output} ({size:,} bytes)")


def parse_args():
    p = argparse.ArgumentParser(description="PGO profile merge & download tool")

    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--input-dir", help="Local directory with .profraw files")
    src.add_argument("--source-url", help="URL to download .profraw file(s) from "
                    "(autoindex directory or single .profdata)")

    p.add_argument("--output", required=True, help="Output .profdata path")
    p.add_argument("--profdata", default="llvm-profdata",
                   help="Path to llvm-profdata binary")
    p.add_argument("--max-age-days", type=int, default=90,
                   help="Expire .profraw older than N days (local mode only, 0=keep all)")
    p.add_argument("--min-files", type=int, default=1,
                   help="Minimum .profraw files required to merge")
    return p.parse_args()


def main():
    args = parse_args()
    print(f"=== PGO Merge [{datetime.now().isoformat()}] ===")

    cache_dir = None  # track temp dir for cleanup

    # --- Determine source ---
    if args.source_url:
        # 远程下载模式
        print(f"  Source: {args.source_url}")
        cache_dir = tempfile.mkdtemp(prefix="pgo_dl_")
        profiles_or_file, pre_merged = download_profiles_from_url(
            args.source_url, cache_dir
        )

        if pre_merged:
            # 直接下载的 .profdata — 拷贝到输出
            print(f"  Using pre-merged profile directly")
            shutil.copy2(profiles_or_file, args.output)
        else:
            # 需要合并
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
            print(f"  FATAL: input-dir not found: {args.input_dir}")
            sys.exit(1)

        profiles, deleted = find_profiles_local(
            args.input_dir, args.max_age_days
        )
        print(f"  Profiles found: {len(profiles)} (expired/deleted: {deleted})")

        if len(profiles) < args.min_files:
            print(f"  SKIP: need >= {args.min_files} profiles, "
                  f"only {len(profiles)} available")
            sys.exit(0)

        merge_profiles(args.profdata, profiles, args.output)

    # --- Cleanup ---
    if cache_dir and os.path.isdir(cache_dir):
        shutil.rmtree(cache_dir, ignore_errors=True)

    print("=== Done ===")


if __name__ == "__main__":
    main()
