"""验证跨平台热修复聚合身份与下载清单，不加载真实程序或 SDK。"""

import json
from pathlib import Path
import tempfile
import unittest

from prepare_program import prepare, validate


# /// @brief 模拟三平台完整构建后的固定布局。
class ProgramHotfixTest(unittest.TestCase):
    """制品内容为可识别文本，校验和仍按完整正文计算。

    测试不声称这些字节可执行，只验证发布身份与校验闭包。
    构建记录绑定提交和修订号，覆盖读取旧聚合产物的错误。
    正式版本保持相同，只有修订号递增，符合客户端比较契约。
    """
    # /// @brief 为每个平台创建主包、更新器和旧程序清单。
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.release = self.root / 'release'
        for directory, files in (
            ('windows-msvc-clang', ('MusicMapMaker-Next.exe', 'MusicMapMaker-Updater.exe')),
            ('linux-gcc14', ('MusicMapMaker-Next', 'MusicMapMaker-Updater')),
            ('macos-arm64', ('MusicMapMaker-Next.app.zip', 'MusicMapMaker-Updater'))):
            (self.release / directory).mkdir(parents=True)
            for filename in files:
                (self.release / directory / filename).write_text(directory + filename)
        # 固定合法长度提交只用于身份核对，不代表真实可执行程序。
        self.commit = 'a' * 40
        (self.release / 'hotfix-build.json').write_text(json.dumps({'commit': self.commit, 'hotfix_revision': 1}))
        self.live = self.root / 'live'
        self.baseline = self.live / 'download/check/check.json'
        self.baseline.parent.mkdir(parents=True)
        self.baseline.write_text(json.dumps({'version': 'v0.6.3',
                                  'platforms': {p: {} for p in ('windows', 'linux', 'macos')}}))
        # 暂存目录开始不存在，覆盖真实首次候选创建流程。
        self.stage = self.root / 'stage'

    # /// @brief 正常候选必须包含六个可校验制品及更高修订身份。
    def test_program_manifest(self):
        document, plan = prepare(self.release, self.baseline, 'v0.6.3', 1, self.commit, self.stage)
        self.assertEqual(document['version'], 'v0.6.3')
        # 正式版本不变但修订提升，符合客户端同版本更新契约。
        self.assertEqual(document['hotfix_revision'], 1)
        self.assertEqual(len(plan['uploads']), 6)
        # 发布前验证器重新读取字节，不只检查生成器返回值。
        validate(self.stage, self.live)

    # /// @brief 原版本客户端不理解修订号，不能向 0.6.2 推送同版本程序更新。
    def test_old_client_release_rejected(self):
        # 0.6.2 到 0.6.3 仍由普通版本更新完成，不能绕过。
        with self.assertRaisesRegex(ValueError, 'v0.6.3'):
            prepare(self.release, self.baseline, 'v0.6.2', 1, self.commit, self.stage)

    # /// @brief 构建提交或修订号不同不能仅通过改清单完成发布。
    def test_stale_build_rejected(self):
        # 修订变了但记录没变，说明正在消费上一轮构建集合。
        with self.assertRaisesRegex(ValueError, 'provenance'):
            prepare(self.release, self.baseline, 'v0.6.3', 2, self.commit, self.stage)

    # /// @brief 任意平台更新器缺失都会阻断整个发布。
    def test_missing_updater_rejected(self):
        # 缺失在发布候选阶段暴露，不等用户下载后才报安装错误。
        (self.release / 'macos-arm64/MusicMapMaker-Updater').unlink()
        with self.assertRaisesRegex(ValueError, 'missing release binary'):
            prepare(self.release, self.baseline, 'v0.6.3', 1, self.commit, self.stage)

    # /// @brief 普通版本升级遗留的旧基线修订不阻止新正式版本的第一轮热修复。
    def test_old_release_revision_ignored(self):
        # 网站普通发布器保留未知字段，新协议必须自行判断扩展字段的作用域。
        old = json.loads(self.baseline.read_text())
        old.update(hotfix_base_tag='v0.6.2', hotfix_revision=99)
        # 遗留字段数值很大，新版本修订一仍应通过作用域判定。
        # 当前正式 version 保持 v0.6.3，只有旧扩展身份属于另一基线。
        self.baseline.write_text(json.dumps(old))
        prepare(self.release, self.baseline, 'v0.6.3', 1, self.commit, self.stage)
        validate(self.stage, self.live)

    # /// @brief 同基线同修订不能重复发布，防止相同身份对应两份程序。
    def test_same_revision_rejected(self):
        # 构建记录本身正确，拒绝来自线上已经存在的相同修订身份。
        old = json.loads(self.baseline.read_text())
        old.update(hotfix_base_tag='v0.6.3', hotfix_revision=1)
        self.baseline.write_text(json.dumps(old))
        with self.assertRaisesRegex(ValueError, 'non-increasing'):
            prepare(self.release, self.baseline, 'v0.6.3', 1, self.commit, self.stage)


if __name__ == '__main__':
    unittest.main()
