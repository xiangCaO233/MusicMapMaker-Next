#!/usr/bin/env python3
"""检查 hotfix 第一父链和已发布 tag 的固定身份，普通合并不改变基线。

Git 不记录分支创建事件，因此基线文件和第一父链共同定义可验证契约。
仅有 ancestor 关系不足以放行从任意开发提交重新建立的 hotfix 分支。
"""

import argparse
import json
from pathlib import Path
import re
import subprocess
from urllib.request import Request, urlopen
import os


# /// @brief 运行只读 Git 查询，失败直接阻断 CI。
def git(root, *args):
    """参数直接传入 argv，不将 tag 或 ref 拼成 shell 文本。

    root 是调用方选定的仓库，输出统一解码为 UTF-8 文本。
    check=True 保留 Git 失败信息，不能用空字符串掩盖缺失引用。
    """
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()


# /// @brief 验证固定 tag、第一父链和源版本，并拒绝混入其它正式版本。
def validate(root, ref='HEAD'):
    """检查的对象是本次构建提交，不能在校验后再改为远端分支最新提交。

    第一父链体现维护主线，第二父链的普通功能合并不要求线性历史。
    所有比基线更新的 release tag 均不得成为构建提交的祖先。
    升级基线必须从新的已发布 tag 重新建立分支并更新固定提交 ID。
    """
    root = Path(root)
    base = json.loads((root / 'hotfix-base.json').read_text())
    # 基线文件和 Git tag 必须具有同一对象身份，不能只看版本文本。
    tag, commit = base['tag'], base['commit']
    if not re.fullmatch(r'v[0-9]+\.[0-9]+\.[0-9]+', tag) or not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('invalid pinned release identity')
    if git(root, 'rev-parse', tag + '^{commit}') != commit:
        raise ValueError('release tag does not match pinned commit')
    # shallow checkout 会把第一父链截断；CI 必须先 fetch 完整历史。
    chain = git(root, 'rev-list', '--first-parent', ref).splitlines()
    if commit not in chain:
        raise ValueError('hotfix first-parent history is not based on release tag')
    # 从 tag 开始的第一个维护提交必须声明基线；从 develop 分叉后再补文件不放行。
    # 正常合并出现在后续维护提交中，不改变这个首提交的父提交身份。
    position = chain.index(commit)
    if position > 0:
        first = chain[position - 1]
        changed = git(root, 'diff', '--name-only', commit, first).splitlines()
        if 'hotfix-base.json' not in changed:
            raise ValueError('first hotfix commit did not declare release base')
        initial = json.loads(git(root, 'show', first + ':hotfix-base.json'))
        if initial != base:
            raise ValueError('hotfix base changed after branch initialization')
    # 源版本从提交对象读取，不能用工作树内未提交版本伪造。
    source = git(root, 'show', ref + ':CMakeLists.txt')
    version = re.search(r'set\(APPVER\s+"([0-9.]+)"\)', source)
    if version is None or 'v' + version[1] != tag:
        raise ValueError('hotfix source version differs from release tag')
    base_numbers = tuple(map(int, tag[1:].split('.')))
    for other in git(root, 'tag', '--list', 'v*').splitlines():
        if not re.fullmatch(r'v[0-9]+\.[0-9]+\.[0-9]+', other):
            continue
        if tuple(map(int, other[1:].split('.'))) <= base_numbers:
            continue
        # 新正式版本只要在任一祖先链中出现，就必须显式迁移基线。
        result = subprocess.run(['git', '-C', str(root), 'merge-base', '--is-ancestor', other, ref])
        if result.returncode == 0:
            raise ValueError('newer release tag merged into hotfix: ' + other)
        if result.returncode != 1:
            raise ValueError('cannot determine release ancestry')
    return base


# /// @brief 人工发布必须额外确认 tag 在 GitHub 上存在已发布 Release。
def main():
    """离线模式只验证 Git 契约，release 模式使用短期只读 GitHub token。

    API 查询只允许当前仓库的 tag，不接受任意上传或服务器写入。
    草稿、预发布和没有 Release 的 tag 都不能作为线上热修复基线。
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default='.')
    parser.add_argument('--release', action='store_true')
    args = parser.parse_args()
    base = validate(args.root)
    if args.release:
        # 网络验证只用于人工发布，分支 push 不依赖线上 GitHub 可用性。
        repository = os.environ['GITHUB_REPOSITORY']
        if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
            raise ValueError('invalid repository identity')
        request = Request(f'https://api.github.com/repos/{repository}/releases/tags/{base["tag"]}',
                          headers={'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
                                   'User-Agent': 'MusicMapMaker-Hotfix/1.0'})
        # API 失败直接阻断，不能把不存在的 Release 当作合法基线。
        with urlopen(request, timeout=30) as response:
            release = json.load(response)
        if release.get('draft') or release.get('prerelease') or not release.get('published_at'):
            raise ValueError('base tag is not a published stable Release')


if __name__ == '__main__':
    main()
