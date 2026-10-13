#!/usr/bin/env python3
"""绑定 DEB 的版本、源码提交和主 ELF，防止包管理器收到旧构建或错误修订。"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile


# /// @brief 只读验证已解压的数据流，任何身份不一致都以失败阻断发布。
# @param stream dpkg 数据归档流；验证不取得文件系统写权限。
# @param base 已发布正式版本；热修复不得改变其应用版本。
# @param revision 已校验的热修复修订，独立于 Debian 基础修订。
# @param commit 本次成功构建的精确源码提交。
# @param program_hash 网页候选或已公开主程序的 SHA256。
# @param deb_version dpkg 控制字段中的完整版本，不能只信任文件名。
def validate_payload(stream, base, revision, commit, program_hash, deb_version):
    """流式检查安装载荷，不展开维护脚本、不执行归档中的任何文件。"""
    info = None
    actual_hash = None
    # 空状态也参与最终检查，缺失条目不能被当作无须验证而跳过。
    with tarfile.open(fileobj=stream, mode='r|') as archive:
        for member in archive:
            # dpkg 的归档条目可能有 ./ 前缀，仅识别产品固定安装路径。
            name = member.name.removeprefix('./')
            if name not in ('opt/mmm/MusicMapMaker-Next',
                            'usr/share/doc/musicmapmaker-next/build-info.json'):
                continue
            # 链接条目会改变安装结果，不能只用链接名称满足预期路径。
            if not member.isfile():
                raise ValueError('DEB identity entries must be regular files')
            with archive.extractfile(member) as source:
                if name.endswith('/build-info.json'):
                    # 身份 JSON 不应携带任意大数据，也不能出现重复身份条目。
                    if info is not None or member.size > 65536:
                        raise ValueError('Invalid or duplicated DEB build-info')
                    info = json.load(source)
                    # JSON 的源身份稍后与控制字段一起检查，不单独放行。
                else:
                    if actual_hash is not None:
                        raise ValueError('Duplicated DEB executable')
                    # ELF 逐块校验，避免同时复制或载入大型程序归档。
                    digest = hashlib.sha256()
                    for chunk in iter(lambda: source.read(1024 * 1024), b''):
                        digest.update(chunk)
                    actual_hash = digest.hexdigest()
    if actual_hash != program_hash:
        raise ValueError('DEB executable differs from hotfix program')
    if not info or info.get('source_commit') != commit or info.get('version') != base[1:]:
        raise ValueError('DEB source identity differs from hotfix')
    # 记录、控制字段和文件名必须使用同一包修订，禁止只改包名假装升级。
    suffix = info.get('deb_revision', '')
    # Debian 基础修订可能由维护者提高，所以只约束正整数前缀和本轮 hotfix 后缀。
    if not re.fullmatch(rf'[1-9][0-9]*\+hotfix\.{revision}', suffix):
        raise ValueError('DEB has no matching hotfix revision')
    if deb_version != base[1:] + '-' + suffix or info.get('architecture') != 'amd64':
        raise ValueError('DEB version or architecture mismatch')


# /// @brief 验证实际 DEB 容器，返回可信的完整 Debian 版本。
# @warning 仅在低频发布任务使用；需要本机 dpkg-deb，禁止在应用热路径调用。
# 文件名、控制元数据、安装记录和主 ELF 四者必须保持同一身份。
def validate(deb, base, revision, commit, program_hash):
    """读取 dpkg 元数据并核对载荷；失败不得继续发布任何软件源。"""
    deb = Path(deb)
    # 人工恢复也使用同一边界限制，不能依赖此前 CI 已检查过这些参数。
    if not re.fullmatch(r'v\d+\.\d+\.\d+', base) or not 0 < revision <= 2147483647:
        raise ValueError('Invalid hotfix base or revision')
    if not re.fullmatch(r'[0-9a-f]{40}', commit) or not re.fullmatch(r'[0-9a-f]{64}', program_hash):
        raise ValueError('Invalid expected source or program hash')
    fields = {}
    # 控制字段来自 dpkg 解码，不能把 archive 文件名当作其替代来源。
    # 每个字段独立查询，避免 dpkg 多字段输出中的本地化标签进入解析。
    for field in ('Package', 'Version', 'Architecture'):
        fields[field] = subprocess.check_output(['dpkg-deb', '--field', str(deb), field], text=True).strip()
    if fields['Package'] != 'musicmapmaker-next' or fields['Architecture'] != 'amd64':
        raise ValueError('Unexpected DEB package or architecture')
    if deb.name != f"musicmapmaker-next_{fields['Version']}_amd64.deb":
        raise ValueError('DEB filename differs from control metadata')
    # 只读数据 tar；等待子进程完成才能确认压缩包并未被截断。
    with subprocess.Popen(['dpkg-deb', '--fsys-tarfile', str(deb)], stdout=subprocess.PIPE) as process:
        validate_payload(process.stdout, base, revision, commit, program_hash, fields['Version'])
        # 容器进程失败也会阻止发布，已匹配的两个条目不能掩盖归档损坏。
        while process.stdout.read(1024 * 1024):
            # tar EOF 后仍可能有块对齐填充，排空管道后再等待子进程结束。
            pass
        if process.wait() != 0:
            raise ValueError('Cannot read complete DEB payload')
    # 调用方只消费验证成功的版本，不重新从不可信包名推断身份。
    return fields['Version']


if __name__ == '__main__':
    # CLI 面向候选准备阶段，不承担远端发布或修改软件源的职责。
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--deb', required=True)
    parser.add_argument('--base', required=True)
    parser.add_argument('--revision', type=int, required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--program', required=True, type=Path)
    # 主 ELF 使用文件摘要，不依据大小、时间戳或缓存目录位置判断一致性。
    args = parser.parse_args()
    # 准备阶段对照本次构建的主程序，恢复阶段另对照已公开清单的摘要。
    with args.program.open('rb') as source:
        program_hash = hashlib.file_digest(source, 'sha256').hexdigest()
    print(validate(args.deb, args.base, args.revision, args.commit, program_hash))
