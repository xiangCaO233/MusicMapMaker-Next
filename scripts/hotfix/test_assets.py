"""验证资源选择边界、完整包一致性和并发发布检测，不连接服务器。"""

import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from prepare_assets import digest, prepare, safe_path
from publish_assets import validate


# /// @brief 使用临时目录创建两文件基线，所有输出位于源码资源目录外。
class AssetHotfixTest(unittest.TestCase):
    """测试仅覆盖发布契约，不模拟已经发生的线上安装。

    基线包、源目录和线上清单互相独立，能够检测错误的来源选择。
    输出使用临时目录，失败清理也不会接触仓库 assets 或 tests/data。
    通过 validate 交叉检查候选，而不是仅断言 prepare 返回的字段。
    """
    # /// @brief 每个测试拥有独立源、完整包与线上清单。
    def setUp(self):
        """夹具两文件内容不同，文件路径固定且期望字节可直接核对。

        包摘要先于源变更计算，代表发布时的真实旧内容。
        未选择源随后可独立变更，保证测试确实检测选择边界。
        """
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        self.package = self.root / 'assets.zip'
        # 两份资源只有一份被选择，能够检测误把整个分支资源全部发布。
        for name in ('a.lua', 'b.lua'):
            (self.source / name).write_text('old-' + name)
        with zipfile.ZipFile(self.package, 'w') as archive:
            for name in ('a.lua', 'b.lua'):
                archive.write(self.source / name, 'assets/' + name)
        manifest = {'version': 'v0.6.2', 'package': {'url': '/download/assets.zip',
                    'sha256': 'sha256:' + digest(self.package)}, 'files': []}
        for name in ('a.lua', 'b.lua'):
            manifest['files'].append({'path': name, 'url': '/old/' + name,
                                     'sha256': 'sha256:' + digest(self.source / name),
                                     'size': (self.source / name).stat().st_size})
        self.live = self.root / 'live'
        self.baseline = self.live / 'download/assets/manifest.json'
        self.baseline.parent.mkdir(parents=True)
        # 清单位于模拟线上根，发布器按固定入口读取。
        self.baseline.write_text(json.dumps(manifest))
        self.output = self.root / 'output'

    # /// @brief 单文件更新必须保留未选择条目，并为冷启动重建完整包。
    def test_selected_overlay(self):
        """一次真实叠加应同时满足已安装与首次安装两种客户端场景。

        保留旧条目 URL 覆盖增量下载，ZIP 正文覆盖完整包一致性。
        改写基线后复用同一候选必须失败，覆盖准备与发布间的竞态。
        """
        (self.source / 'a.lua').write_text('new-a')
        (self.source / 'b.lua').write_text('unselected-change')
        manifest, plan = prepare(self.source, self.baseline, self.package,
                                 ['a.lua'], 'v0.6.2', 1, self.output)
        # 新版本标记与旧版客户端快速路径兼容，不能继续使用 v0.6.2。
        self.assertEqual(manifest['version'], 'v0.6.2-assets.1')
        # 一个差异文件和一个完整包，未选择变更不能进入计划。
        self.assertEqual(len(plan['uploads']), 2)
        self.assertEqual(manifest['files'][1]['url'], '/old/b.lua')
        with zipfile.ZipFile(self.output / plan['uploads'][-1]['key']) as archive:
            self.assertEqual(archive.read('assets/a.lua'), b'new-a')
            self.assertEqual(archive.read('assets/b.lua'), b'old-b.lua')
        # 同一候选还要通过服务器离线校验，不只检查生成器输出格式。
        validate(self.output, self.live)
        # 模拟另一发布在准备后改变清单，候选文件保持不变。
        self.baseline.write_text('{}')
        with self.assertRaisesRegex(ValueError, 'baseline changed'):
            validate(self.output, self.live)

    # /// @brief 模拟选中资产在读取摘要和源复制之间被编辑器保存。
    def test_source_change_during_prepare(self):
        """候选的单文件、清单和全量包必须引用相同的暂存字节。

        真实 copyfile 在新内容写完后打开源文件。旧实现会先读取原摘要，
        因而后续的发布预检拒绝；当前准备完成的候选需直接通过校验。
        """
        original = self.source / 'a.lua'
        # 初始内容也异于基线，确保旧实现会先记下错误摘要后继续复制。
        original.write_bytes(b'before-copy')
        # 保留真实复制器，mock 只负责固定竞态发生的时点。
        real_copy = shutil.copyfile

        def edit_at_copy(source, destination):
            # 只改变可编辑源，保留复制产生的所有其他内容和副作用。
            if source == original:
                original.write_bytes(b'after-copy-with-different-size')
            return real_copy(source, destination)

        with patch('prepare_assets.shutil.copyfile', side_effect=edit_at_copy):
            manifest, plan = prepare(self.source, self.baseline, self.package,
                                     ['a.lua'], 'v0.6.2', 1, self.output)
        # 计划以单文件作为第一项。比对实际文件、清单摘要和 ZIP 解压后的字节。
        staged = self.output / plan['uploads'][0]['key']
        expected = b'after-copy-with-different-size'
        self.assertEqual(staged.read_bytes(), expected)
        self.assertEqual(manifest['files'][0]['sha256'], 'sha256:' + digest(staged))
        with zipfile.ZipFile(self.output / plan['uploads'][-1]['key']) as archive:
            self.assertEqual(archive.read('assets/a.lua'), expected)
        # 运行正式发布预检，确认客户端可使用该候选，无需任何线上访问。
        validate(self.output, self.live)

    # /// @brief 发布计划绑定实际解析的基线，即使准备期间读到了新版本。
    def test_baseline_change_during_prepare(self):
        """准备完成后不应以最新清单的摘要掩盖并发发布。"""
        original = self.source / 'a.lua'
        original.write_bytes(b'selected change')
        # 捕获开始准备时的基线作为唯一正确身份。
        old_digest = digest(self.baseline)
        real_copy = shutil.copyfile

        def change_baseline_at_copy(source, destination):
            # 删除原清单结构，模拟新一轮发布接管同一个入口路径。
            if source == original:
                self.baseline.write_text('{}')
            return real_copy(source, destination)

        with patch('prepare_assets.shutil.copyfile', side_effect=change_baseline_at_copy):
            _, plan = prepare(self.source, self.baseline, self.package,
                              ['a.lua'], 'v0.6.2', 1, self.output)
        # 只有原摘要能让独立发布器拒绝覆盖期间出现的新基线。
        self.assertEqual(plan['baseline_sha256'], old_digest)
        with self.assertRaisesRegex(ValueError, 'baseline changed'):
            validate(self.output, self.live)

    # /// @brief 禁止同版本重复发布以及无内容变化的发布。
    def test_noop_and_version_mismatch(self):
        """无变化不可消耗修订号，跨正式版本不可沿用另一版本基线。"""
        with self.assertRaisesRegex(ValueError, 'version mismatch'):
            prepare(self.source, self.baseline, self.package, ['a.lua'], 'v0.6.1', 1, self.output)
        with self.assertRaisesRegex(ValueError, 'no content changes'):
            prepare(self.source, self.baseline, self.package, ['a.lua'], 'v0.6.2', 1, self.output)

    # /// @brief ZIP 内容必须符合已发布包摘要，不能使用错误的 CI 基线。
    def test_corrupt_package(self):
        self.package.write_bytes(b'corrupted')
        with self.assertRaisesRegex(ValueError, 'package checksum mismatch'):
            prepare(self.source, self.baseline, self.package, ['a.lua'], 'v0.6.2', 1, self.output)

    # /// @brief 路径契约同时覆盖 Windows 盘符和 POSIX 根目录逃逸。
    def test_unsafe_paths(self):
        for path in ('../x', '/x', 'C:/x', 'a\\b', 'a//b', 'a/./b'):
            with self.subTest(path=path), self.assertRaises(ValueError):
                safe_path(path)

    # /// @brief 暂存文件被篡改后不能继续发布原清单。
    def test_tampered_stage(self):
        """篡改发生在准备后，检验发布前仍会重新核对完整正文。"""
        (self.source / 'a.lua').write_text('new-a')
        _, plan = prepare(self.source, self.baseline, self.package,
                          ['a.lua'], 'v0.6.2', 1, self.output)
        # 篡改实际暂存正文，确保发布器不能仅相信生成器返回值。
        (self.output / plan['uploads'][0]['key']).write_text('tampered')
        with self.assertRaisesRegex(ValueError, 'staged checksum mismatch'):
            validate(self.output, self.live)


if __name__ == '__main__':
    unittest.main()
