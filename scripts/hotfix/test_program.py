"""验证跨平台热修复聚合身份与下载清单，不加载真实程序或 SDK。"""

import json
import hashlib
import importlib.util
import shutil
import zipfile
from pathlib import Path
import tempfile
import unittest

from prepare_program import prepare, validate


# /// @brief 模拟四平台完整构建后的固定布局。
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
        # 用真实 ZIP/ELF 头经过生产校验器，不把任意文本当作有效 APK。
        # 夹具来源与修订分别记录，确保桌面通过不能掩盖安卓仍为旧版。
        android = self.release / 'android-arm64'
        android.mkdir()
        self.apk = android / 'MusicMapMaker-Next-0.6.5-hotfix.1-android-arm64.apk'
        with zipfile.ZipFile(self.apk, 'w') as archive:
            archive.writestr('lib/arm64-v8a/libmain.so', b'\x7fELF' + bytes(14) + (183).to_bytes(2, 'little'))
        self.android_record = dict(source=self.commit, variant='release', abi='arm64-v8a',
                                   versionName='0.6.5-hotfix.1', baseVersion='0.6.5', hotfix_revision=1,
                                   bytes=self.apk.stat().st_size, sha256=hashlib.sha256(self.apk.read_bytes()).hexdigest())
        (android / 'build-info.json').write_text(json.dumps(self.android_record))
        self.live = self.root / 'live'
        self.baseline = self.live / 'download/check/check.json'
        self.baseline.parent.mkdir(parents=True)
        self.baseline.write_text(json.dumps({'version': 'v0.6.5',
                                  'platforms': {p: {} for p in ('windows', 'linux', 'macos', 'android')}}))
        # 暂存目录开始不存在，覆盖真实首次候选创建流程。
        self.stage = self.root / 'stage'

    # /// @brief 正常候选必须包含七个可校验制品及更高修订身份。
    def test_program_manifest(self):
        document, plan = prepare(self.release, self.baseline, 'v0.6.5', 1, self.commit, self.stage)
        self.assertEqual(document['version'], 'v0.6.5')
        # 正式版本不变但修订提升，符合客户端同版本更新契约。
        self.assertEqual(document['hotfix_revision'], 1)
        self.assertEqual(len(plan['uploads']), 7)
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
            prepare(self.release, self.baseline, 'v0.6.5', 2, self.commit, self.stage)

    # /// @brief 任意平台更新器缺失都会阻断整个发布。
    def test_missing_updater_rejected(self):
        # 缺失在发布候选阶段暴露，不等用户下载后才报安装错误。
        (self.release / 'macos-arm64/MusicMapMaker-Updater').unlink()
        with self.assertRaisesRegex(ValueError, 'missing release binary'):
            prepare(self.release, self.baseline, 'v0.6.5', 1, self.commit, self.stage)

    # /// @brief 普通版本升级遗留的旧基线修订不阻止新正式版本的第一轮热修复。
    def test_old_release_revision_ignored(self):
        # 网站普通发布器保留未知字段，新协议必须自行判断扩展字段的作用域。
        old = json.loads(self.baseline.read_text())
        old.update(hotfix_base_tag='v0.6.2', hotfix_revision=99)
        # 遗留字段数值很大，新版本修订一仍应通过作用域判定。
        # 当前正式 version 保持 v0.6.3，只有旧扩展身份属于另一基线。
        self.baseline.write_text(json.dumps(old))
        prepare(self.release, self.baseline, 'v0.6.5', 1, self.commit, self.stage)
        validate(self.stage, self.live)

    # /// @brief 同基线同修订不能重复发布，防止相同身份对应两份程序。
    def test_same_revision_rejected(self):
        # 构建记录本身正确，拒绝来自线上已经存在的相同修订身份。
        old = json.loads(self.baseline.read_text())
        old.update(hotfix_base_tag='v0.6.5', hotfix_revision=1)
        self.baseline.write_text(json.dumps(old))
        with self.assertRaisesRegex(ValueError, 'non-increasing'):
            prepare(self.release, self.baseline, 'v0.6.5', 1, self.commit, self.stage)


    def test_android_is_required(self):
        """@brief 缺少 APK 必须阻断整个程序热修复，不回退成三平台发布。

        桌面主程序和更新器均保留有效夹具，因此错误只来自安卓闭包。
        不能靠缺少无关桌面文件让这个必要平台测试碰巧失败。
        """
        self.apk.unlink()
        # 记录仍然存在，拒绝必须来自实际安装包缺失，而非其他旧前提。
        with self.assertRaisesRegex(ValueError, 'APK'):
            prepare(self.release, self.baseline, 'v0.6.5', 1, self.commit, self.stage)

    def test_android_source_and_revision(self):
        """@brief 桌面来源正确时，安卓旧提交或旧修订也必须分别阻断。

        每个反例重新复制原记录，避免先前损坏掩盖当前字段的检查。
        APK 大小和摘要保持原值，源码与修订必须作为独立身份核验。
        """
        for key, value in (('source', 'b' * 40), ('hotfix_revision', 0), ('hotfix_revision', 2)):
            record = dict(self.android_record)
            record[key] = value
            (self.apk.parent / 'build-info.json').write_text(json.dumps(record))
            # 不改变 APK 字节，让拒绝明确对应本次变更的来源或修订字段。
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                prepare(self.release, self.baseline, 'v0.6.5', 1, self.commit, self.stage)

    def test_android_platform_cannot_be_omitted(self):
        """@brief 全局修订不能升高而遗漏线上 Android 节点。

        当前普通版本已经发布安卓，热修复不能缩小其既有平台集合。
        缺平台必须明确失败，不能静默把全局修订只应用到桌面。
        """
        old = json.loads(self.baseline.read_text())
        del old['platforms']['android']
        self.baseline.write_text(json.dumps(old))
        with self.assertRaisesRegex(ValueError, 'requires Android'):
            prepare(self.release, self.baseline, 'v0.6.5', 1, self.commit, self.stage)

    def test_server_revalidates_apk(self):
        """@brief 暂存后的安装包或元数据变化必须在服务器切清单前拒绝。

        校验分别破坏来源记录与正文，证明两种独立输入都被复核。
        先恢复记录再破坏 APK，避免第二个断言仍然只命中来源错误。
        """
        document, _ = prepare(self.release, self.baseline, 'v0.6.5', 1, self.commit, self.stage)
        staged = self.stage / document['platforms']['android']['url'].lstrip('/')
        record_path = staged.parent / 'build-info.json'
        record = json.loads(record_path.read_text())
        record['source'] = 'c' * 40
        record_path.write_text(json.dumps(record))
        # 元数据不计作可执行制品，但仍需参与来源闭包的服务器复核。
        with self.assertRaisesRegex(ValueError, 'source'):
            validate(self.stage, self.live)
        record_path.write_text(json.dumps(self.android_record))
        staged.write_bytes(staged.read_bytes() + b'changed')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            validate(self.stage, self.live)

    def test_server_adjacent_validator(self):
        """@brief 实际服务器工具布局同样复用 APK 核验，不依赖源码树存在。

        tools 只复制发布命令实际传输的文件，不能借本仓目录满足依赖。
        验证调用真实动态载入入口，覆盖邻接文件路径的部署契约。
        """
        prepare(self.release, self.baseline, 'v0.6.5', 1, self.commit, self.stage)
        tools = self.root / 'tools'
        tools.mkdir()
        source = Path(__file__).resolve().parent
        for name in ('prepare_program.py', 'prepare_assets.py'):
            shutil.copyfile(source / name, tools / name)
        shutil.copyfile(source.parent / 'ci/android-release.py', tools / 'android-release.py')
        # 动态模块采用独立名称，邻接路径必须从这份真实暂存脚本计算。
        spec = importlib.util.spec_from_file_location('staged_program_validator', tools / 'prepare_program.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.validate(self.stage, self.live)


if __name__ == '__main__':
    unittest.main()
