#!/usr/bin/env python3
"""公开程序热修复成功后同步两个软件源，也可单独恢复软件源发布。"""

import argparse
import json
from pathlib import Path
import subprocess
from urllib.request import urlopen

from validate_deb import validate


# /// @brief 将已公开的程序修订镜像到包管理器入口。
# @warning 涉及网络和签名软件源写入，只能由显式发布或失败恢复调用。
# 普通 CI 构建和纯资产发布不执行此函数。
def sync(deb, base, revision, commit):
    """只消费已公开且身份相同的修复，不能借恢复入口发布未选择的程序。"""
    # 固定官方清单地址，不接受调用者指定任意网络来源。
    with urlopen('https://mmm.xiang233.top/download/check/check.json', timeout=30) as response:
        published = json.load(response)
    # 正式版本、基线、修订、源码提交缺一不可，不能只比较修订数值。
    if (published.get('version') != base or published.get('hotfix_base_tag') != base
            or published.get('hotfix_revision') != revision or published.get('hotfix_commit') != commit):
        raise ValueError('Hotfix is not the currently published program')
    # 对照公开主 ELF 摘要，恢复时不依赖可能已被下一轮构建覆盖的 release 目录。
    checksum = published['platforms']['linux']['checksum']
    # 软件源仅发布 Linux DEB；其他平台的摘要不能代替包内 ELF 身份。
    if not checksum.startswith('sha256:'):
        raise ValueError('Published Linux program has no SHA256 identity')
    validate(deb, base, revision, commit, checksum[7:])
    root = Path(__file__).resolve().parents[2]
    # 两次外部操作都使用固定脚本和 argv，路径不经过 shell 拼接。
    # APT 原入口保留签名、公钥校验和 CDN 下载验证，失败不继续写 Gentoo。
    subprocess.run(['bash', str(root / 'scripts/apt/publish_repository.sh'), str(Path(deb).resolve())], check=True)
    # Gentoo 的不可变源码 tag 和二进制 Manifest 随后绑定同一修订、提交和 DEB。
    subprocess.run(['bash', str(root / 'scripts/ci/update-gentoo-overlay.sh'),
                    base, commit, str(revision), str(Path(deb).resolve())], check=True)


if __name__ == '__main__':
    # 恢复调用保留原 DEB 和精确 SHA；禁止自动重新编译或选择目录中的最新文件。
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--deb', required=True, type=Path)
    parser.add_argument('--base', required=True)
    parser.add_argument('--revision', type=int, required=True)
    parser.add_argument('--commit', required=True)
    args = parser.parse_args()
    sync(args.deb, args.base, args.revision, args.commit)
