#!/usr/bin/env python3
"""在隔离的小型 tar 载荷中验证热修复包身份，不访问生产源或运行维护脚本。"""

import hashlib
import io
import json
import tarfile
import unittest
from unittest.mock import patch

from validate_deb import validate_payload
from sync_packages import sync


# /// @brief 合成包保留实际安装路径，覆盖源码记录与主 ELF 的一致性检查。
class PackageIdentityTest(unittest.TestCase):
    def payload(self, source='a' * 40, revision='2+hotfix.1', program=b'program'):
        # /// @brief 只构造验证器读取的两个普通文件，不包含可执行的维护脚本。
        stream = io.BytesIO()
        # 保留原控制字段语义，修订同时出现在包内记录和测试期望中。
        info = {'version': '0.6.3', 'deb_revision': revision,
                'source_commit': source, 'architecture': 'amd64'}
        with tarfile.open(fileobj=stream, mode='w') as archive:
            # 使用真实产品安装路径，不能以测试专用路径绕过验证器筛选。
            for name, data in [('opt/mmm/MusicMapMaker-Next', program),
                               ('usr/share/doc/musicmapmaker-next/build-info.json', json.dumps(info).encode())]:
                member = tarfile.TarInfo('./' + name)
                # 普通文件类型是默认值，大小由实际字节生成，避免无效 tar 夹具。
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))
        stream.seek(0)
        # 验证器使用流式模式，从归档起点读取，而不依赖随机定位。
        return stream

    def check(self, stream, version='0.6.3-2+hotfix.1'):
        # /// @brief 期望身份固定，错误载荷不能靠同时改变测试期望绕过校验。
        validate_payload(stream, 'v0.6.3', 1, 'a' * 40,
                         hashlib.sha256(b'program').hexdigest(), version)

    def test_matching_package(self):
        # /// @brief 同源码、同 ELF、同递增包版本允许进入发布阶段。
        # 正向用例不传绕过参数，必须经过全部身份字段检查。
        self.check(self.payload())

    def test_wrong_commit(self):
        # /// @brief 错误缓存的程序即使版本相同，也必须拒绝。
        with self.assertRaises(ValueError):
            # 只更换源码字段，其余身份保持有效，失败必须来自提交不一致。
            self.check(self.payload(source='b' * 40))

    def test_wrong_program(self):
        # /// @brief 包内 ELF 必须与网页公开候选逐字节一致。
        with self.assertRaises(ValueError):
            # 相同版本记录不能补偿 ELF 字节变化。
            self.check(self.payload(program=b'old-program'))

    def test_revision_zero_or_mismatch(self):
        # /// @brief 原正式包与其他热修复修订都不能重命名后混入本次源。
        for revision in ('2', '2+hotfix.2'):
            # 分别覆盖未递增正式包和不属于本次发布的其他热修复包。
            with self.subTest(revision=revision), self.assertRaises(ValueError):
                self.check(self.payload(revision=revision))

    def test_control_version_mismatch(self):
        # /// @brief 控制字段不能与包内记录矛盾，否则 dpkg 的升级比较会失真。
        with self.assertRaises(ValueError):
            # JSON 仍是 hotfix.1，单独控制字段回到正式版时不能放行。
            self.check(self.payload(), version='0.6.3-2')

    def test_missing_payload(self):
        # /// @brief 缺失程序或身份记录不是有效的更新包。
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode='w'):
            pass
        stream.seek(0)
        # 完整但空的 tar 与损坏归档区分，测试缺失身份而非 tar 解析器。
        with self.assertRaises(ValueError):
            self.check(stream)


# /// @brief 恢复只镜像线上身份，APT 失败不能继续推送 Gentoo。
class PackageSyncTest(unittest.TestCase):
    def published(self, revision=1):
        # /// @brief 网络层替换成静态字节，不需要任何生产 token 或部署密钥。
        # 返回 context manager，保持 urlopen 的使用方式而不打开真实连接。
        return io.BytesIO(json.dumps({'version': 'v0.6.3', 'hotfix_base_tag': 'v0.6.3',
            'hotfix_revision': revision, 'hotfix_commit': 'a' * 40,
            'platforms': {'linux': {'checksum': 'sha256:' + 'b' * 64}}}).encode())

    def test_sync_order(self):
        # /// @brief 先验证包，再更新签名 APT，最后更新 Gentoo；不重新部署网页。
        with patch('sync_packages.urlopen', return_value=self.published()), \
             patch('sync_packages.validate') as validate, patch('sync_packages.subprocess.run') as run:
            sync('package.deb', 'v0.6.3', 1, 'a' * 40)
            # 载荷必须在任何外部写入之前验证一次。
            validate.assert_called_once()
            self.assertEqual(run.call_count, 2)
            # 第一条命令必须保留 APT 签名发布 helper，不以 git push 代替。
            self.assertTrue(run.call_args_list[0].args[0][1].endswith('publish_repository.sh'))
            # 第二条命令才修改 overlay，确保其 URI 指向已验证的公开包。
            self.assertTrue(run.call_args_list[1].args[0][1].endswith('update-gentoo-overlay.sh'))

    def test_unpublished_revision_is_rejected(self):
        # /// @brief 尚未公开的新修订不能借恢复入口提前进入软件源。
        with patch('sync_packages.urlopen', return_value=self.published(2)), \
             patch('sync_packages.subprocess.run') as run, self.assertRaises(ValueError):
            sync('package.deb', 'v0.6.3', 1, 'a' * 40)
        # 旧候选不能更新当前已经推进到修订二的软件源。
        run.assert_not_called()

    def test_apt_failure_stops_gentoo(self):
        # /// @brief 跨服务器流程不能原子回滚，传播失败并保留修复入口。
        with patch('sync_packages.urlopen', return_value=self.published()), \
             patch('sync_packages.validate'), patch('sync_packages.subprocess.run', side_effect=RuntimeError) as run, \
             self.assertRaises(RuntimeError):
            sync('package.deb', 'v0.6.3', 1, 'a' * 40)
        # 模拟发布失败后只有 APT 的调用记录，不能继续传播不完整状态。
        self.assertEqual(run.call_count, 1)


if __name__ == '__main__':
    # 同一组检查也由 hotfix preflight 的 unittest discover 自动执行。
    unittest.main()
