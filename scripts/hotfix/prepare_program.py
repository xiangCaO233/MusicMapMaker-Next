#!/usr/bin/env python3
"""从本次 hotfix 构建聚合目录准备程序更新清单，不执行公开上传。

程序保持正式版本号，独立修订号从 0.6.3 开始由客户端参与比较。
每次发布同时绑定构建提交和修订号，拒绝消费上次 CI 的聚合产物。
"""

import json
from pathlib import Path
import re
import shutil
from urllib.parse import quote

from prepare_assets import digest


# /// @brief 将三平台固定产物映射为不可变下载对象。
def prepare(release, baseline_path, base, revision, commit, output):
    """本期复用现有单文件和 macOS 整包更新协议。

    Windows 与 Linux 的主程序、macOS App ZIP 分别保留原协议字段。
    更新器单独上传并校验，不能用主程序摘要代替更新器摘要。
    旧平台节点的资源与其他元数据保持原值。
    revision 必须与本次聚合记录一致，不能只改服务器清单。
    """
    release, output = Path(release), Path(output)
    numbers = tuple(map(int, base[1:].split('.')))
    if numbers < (0, 6, 3) or revision <= 0:
        raise ValueError('program hotfix requires release v0.6.3 or newer')
    # 构建记录由完整聚合步骤生成，必须绑定本次精确提交。
    provenance = json.loads((release / 'hotfix-build.json').read_text())
    if provenance != {'commit': commit, 'hotfix_revision': revision}:
        raise ValueError('build provenance differs from requested release')
    # 普通升级走原发布入口，同版本热修复只能提升修订号。
    baseline = json.loads(Path(baseline_path).read_text(encoding='utf-8'))
    active_revision = baseline.get('hotfix_revision', 0) if baseline.get('hotfix_base_tag') == base else 0
    # 同一正式版本的重复或降级发布必须失败，不能依赖客户端自己拒绝。
    # 不同基线的扩展字段视为历史元数据，第一轮热修复仍从修订一开始。
    # CMake 和发布候选共享当前修订，普通构建生成头默认归零。
    # 只有同基线的修订参与判断，正式发布遗留的旧扩展字段不限制新版本。
    if baseline['version'] != base or revision <= active_revision:
        raise ValueError('published program version mismatch or non-increasing revision')
    # 显式使用与普通网站发布相同的公开工具链产物。
    mapping = {
        'windows': ('windows-msvc-clang', 'MusicMapMaker-Next.exe', 'MusicMapMaker-Updater.exe'),
        'linux': ('linux-gcc14', 'MusicMapMaker-Next', 'MusicMapMaker-Updater'),
        'macos': ('macos-arm64', 'MusicMapMaker-Next.app.zip', 'MusicMapMaker-Updater'),
    }
    # 平台目录独立，Linux 与 macOS 同名更新器不能互相覆盖。
    uploads = []
    prefix = f'download/hotfix/program/{base}/r{revision}'
    for platform, (directory, binary, updater) in mapping.items():
        # 保留平台资源和其他下载字段，只替换本次主程序与更新器。
        node = baseline['platforms'][platform]
        for filename, target in ((binary, node), (updater, node.setdefault('updater', {}))):
            source = release / directory / filename
            if not source.is_file() or source.is_symlink():
                raise ValueError('missing release binary: ' + str(source))
            key = prefix + '/' + platform + '/' + filename
            staged = output / key
            staged.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, staged)
            # 摘要和大小都来自暂存副本，避免描述另一轮构建的源文件。
            checksum, size = digest(staged), staged.stat().st_size
            target.update(url='/' + quote(key, safe='/'), checksum='sha256:' + checksum, size=size)
            # 新条目不能继承旧 origin_url，否则 CDN 帮助器可能读到旧程序。
            target.pop('origin_url', None)
            uploads.append({'key': key, 'sha256': checksum, 'size': size})
    baseline.update(hotfix_revision=revision, hotfix_base_tag=base, hotfix_commit=commit)
    # 首次普通版本清单没有修订字段，发布器按零修订建立第一轮热修复。
    # 新字段独立存储，既有 version 文本保持正式发布 tag 的版本身份。
    # 不改 changelog 的格式，添加明确的热修复身份方便现有更新弹窗展示。
    baseline['changelog'] = f'Hotfix {revision}\n\n' + baseline.get('changelog', '')
    # 程序和资产计划分开命名，组合发布绑定两份原始清单。
    plan = {'baseline_sha256': digest(baseline_path), 'uploads': uploads}
    output.mkdir(parents=True, exist_ok=True)
    (output / 'program.json').write_text(json.dumps(baseline, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    (output / 'program-plan.json').write_text(json.dumps(plan, indent=2) + '\n', encoding='utf-8')
    return baseline, plan


# /// @brief 发布阶段重新校验构建身份、当前版本和三平台制品。
def validate(stage, root):
    """服务器不相信人工上传的 program.json，必须重新检查全部引用。

    baseline_sha256 保证检查与发布之间没有另一次程序更新。
    URL 限定在本次版本目录，不能由发布计划任意覆盖线上路径。
    plan 必须精确包含六个制品，任何平台缺失都会阻止切换程序清单。
    """
    stage, root = Path(stage), Path(root)
    plan = json.loads((stage / 'program-plan.json').read_text())
    document = json.loads((stage / 'program.json').read_text())
    current = root / 'download/check/check.json'
    if digest(current) != plan['baseline_sha256']:
        raise ValueError('program baseline changed')
    # 发布前重新读取固定入口，不依赖准备阶段的内存快照。
    old = json.loads(current.read_text())
    version, revision = document['version'], document['hotfix_revision']
    if not re.fullmatch(r'v[0-9]+\.[0-9]+\.[0-9]+', version) or not isinstance(revision, int) or not 0 < revision <= 2147483647:
        raise ValueError('invalid program hotfix identity')
    active_revision = old.get('hotfix_revision', 0) if old.get('hotfix_base_tag') == version else 0
    # 发布阶段再次检查作用域，准备与上传之间线上版本可能已经改变。
    if version != old['version'] or revision <= active_revision:
        raise ValueError('program release is not newer')
    if document.get('hotfix_base_tag') != version or not re.fullmatch(r'[0-9a-f]{40}', document.get('hotfix_commit', '')):
        # 候选必须保留精确构建身份，不能通过删除字段伪装成普通发布。
        raise ValueError('program release has no valid build identity')
    prefix = f'download/hotfix/program/{version}/r{revision}/'
    # 每个平台主程序和更新器各一份，重复键不可静默去重。
    records = {r['key']: r for r in plan['uploads']}
    if len(records) != 6 or len(plan['uploads']) != 6:
        raise ValueError('program release must contain six binaries')
    for platform in ('windows', 'linux', 'macos'):
        # 下载地址由本次计划导出，不能引用未验证的执行文件。
        node = document['platforms'][platform]
        for item in (node, node['updater']):
            key = next((k for k in records if item['url'] == '/' + quote(k, safe='/')), None)
            if key is None or not key.startswith(prefix + platform + '/'):
                raise ValueError('invalid program download identity')
            # 路径解析后也要处于暂存根，防止符号链接逃逸。
            source, record = stage / key, records[key]
            if not source.resolve().is_relative_to(stage.resolve()) or source.is_symlink():
                raise ValueError('program source escapes stage')
            if digest(source) != record['sha256'] or source.stat().st_size != record['size']:
                raise ValueError('staged program checksum mismatch')
            if item['checksum'] != 'sha256:' + record['sha256'] or item['size'] != record['size']:
                raise ValueError('program manifest checksum mismatch')
    # 准备结果通过服务器校验后才成为可发布候选。
    return document, plan
