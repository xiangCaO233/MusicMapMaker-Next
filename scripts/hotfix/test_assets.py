"""验证资源选择边界、完整包一致性和并发发布检测，不连接服务器。"""

import json
from pathlib import Path
import tempfile
import unittest
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
