#!/usr/bin/env python3
"""@brief 在隔离输出目录验证 Android 发布来源、架构和替换事务。

夹具只模拟 ZIP、ELF 头和构建记录；实际签名另由 CI 的真实 APK 验签覆盖。
输出位于构建树或调用者指定的目录，不向源码资源目录添加二进制文件。
"""
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path
import tempfile
import unittest
import zipfile


# 测试只在集中输出目录产生夹具，不在源码旁缓存动态导入模块的字节码。
sys.dont_write_bytecode = True

# 同目录的发布模块通过文件位置加载，不依赖当前 shell 的工作目录。
SPEC = importlib.util.spec_from_file_location("android_release", Path(__file__).with_name("android-release.py"))
RELEASE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RELEASE)
SOURCE = "a" * 40


class AndroidReleaseTest(unittest.TestCase):
    """@brief 每个场景独占临时目录，失败也不会改写其他测试的候选包。"""

    def setUp(self):
        """@brief 把生成物归入构建输出，禁止使用 tests/data 存放临时 APK。

        CI 可以传入集中输出目录；本机默认使用已有 build/test_output 布局。
        每个测试的生命周期由 TemporaryDirectory 独立管理。
        """
        root = Path(os.environ.get("MMM_TEST_OUTPUT_DIR", Path(__file__).resolve().parents[2] / "build/test_output/android-release"))
        root.mkdir(parents=True, exist_ok=True)
        self.m_temp = tempfile.TemporaryDirectory(dir=root)
        self.addCleanup(self.m_temp.cleanup)
        self.m_root = Path(self.m_temp.name)

    def delivery(self, name="input", abi="arm64-v8a"):
        """@brief 生成带真实摘要与 CRC 的最小交付，避免静态伪造校验成功。

        @param abi 控制 ZIP 内的实际 JNI 目录，用于错误架构反例。
        @return 目录与记录，反例可以单独修改记录而保留原文件。
        ELF 头只覆盖本工具读取的架构字段，不冒充可运行的 Android 应用。
        """
        root = self.m_root / name
        root.mkdir()
        apk = root / "MusicMapMaker-Next-0.6.4-android-arm64.apk"
        with zipfile.ZipFile(apk, "w") as archive:
            archive.writestr(f"lib/{abi}/libmain.so", b"\x7fELF" + bytes(14) + (183).to_bytes(2, "little"))
            archive.writestr("assets/example", b"resource")
        record = dict(source=SOURCE, abi="arm64-v8a", variant="release", versionName="0.6.4",
                      bytes=apk.stat().st_size, sha256=hashlib.sha256(apk.read_bytes()).hexdigest())
        (root / "build-info.json").write_text(json.dumps(record))
        return root, record

    def test_valid_and_replacement(self):
        """@brief 正常包能交付和重复替换，正式标签允许带 v。

        目标只包含白名单文件，不能把相邻缓存误打进 GitHub Release。
        摘要文件必须引用真实 APK 名称，下载后可以直接校验。
        """
        root, record = self.delivery()
        (root / "unrelated-cache").write_text("private cache")
        destination = self.m_root / "release/android-arm64"
        apk = RELEASE.stage_delivery(root, destination, SOURCE, "v0.6.4")
        self.assertEqual({p.name for p in destination.iterdir()}, {apk.name, apk.name + ".sha256", "build-info.json"})
        self.assertEqual((destination / (apk.name + ".sha256")).read_text(), f"{record['sha256']}  {apk.name}\n")
        self.assertEqual(RELEASE.stage_delivery(root, destination, SOURCE), apk)
        self.assertFalse(destination.with_name(".android-arm64.previous").exists())

    def test_github_package_only_preserves_apk(self):
        """@brief 实际调用正式打包入口，确认 APK 与校验清单进入最终附件集合。

        其余平台使用微型归档夹具，避免回归检查复制真实预编译和发布产物。
        package-only 不访问 GitHub，不创建 tag；源码身份使用当前真实 HEAD。
        APK 仍由前置工具验证，发布脚本不能只拷贝一个名字正确的文件。
        """
        repository = Path(__file__).resolve().parents[2]
        source = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repository, text=True).strip()
        delivery, record = self.delivery()
        record["source"] = source
        (delivery / "build-info.json").write_text(json.dumps(record))
        release = self.m_root / "aggregate"
        RELEASE.stage_delivery(delivery, release / "android-arm64", source)
        # 每个平台只放打包契约要求的文件；macOS App ZIP 仍须通过实际 ZIP CRC 检查。
        # 不用伪造命令或替换脚本，确保本测试能够发现正式附件收集遗漏 APK。
        for variant in ("windows-msvc-clang", "windows-mingw-gcc", "windows-mingw-clang",
                        "linux-gcc14", "linux-clang19", "assets", "macos-arm64"):
            folder = release / variant
            folder.mkdir()
            name = "MusicMapMaker-Next.exe" if variant.startswith("windows") else "MusicMapMaker-Next"
            (folder / name).write_bytes(b"fixture")
        mac = release / "macos-arm64"
        (mac / "MusicMapMaker-Next.dmg").write_bytes(b"dmg fixture")
        (mac / "MusicMapMaker-Updater").write_bytes(b"updater fixture")
        with zipfile.ZipFile(mac / "MusicMapMaker-Next.app.zip", "w") as archive:
            archive.writestr("MusicMapMaker-Next.app/fixture", "app fixture")
        output = self.m_root / "github-assets"
        command = ["bash", str(repository / "scripts/ci/publish-github-release.sh"), "--package-only",
                   "--source-release", str(release), "--output-dir", str(output), "--tag", "v0.6.4"]
        result = subprocess.run(command, cwd=repository, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # APK 已经签名压缩，必须逐字节保留，不能重压缩或包一层 ZIP 改变用户安装入口。
        # SHA256SUMS 应直接包含规范 APK 名称，供下载后的用户独立核验。
        apk = output / "MusicMapMaker-Next-0.6.4-android-arm64.apk"
        self.assertEqual(hashlib.sha256(apk.read_bytes()).hexdigest(), record["sha256"])
        self.assertIn(f"{record['sha256']}  {apk.name}", (output / "SHA256SUMS").read_text())

    def test_wrong_source_or_version(self):
        """@brief 拒绝把其他提交或其他版本的包重新命名成当前发布。

        失败发生在目标创建前，既有候选不应受到校验反例影响。
        不以文件名正确代替构建记录身份一致。
        """
        root, _ = self.delivery()
        for source, version in (("b" * 40, "0.6.4"), (SOURCE, "0.6.5"), ("main", None)):
            with self.subTest(source=source, version=version), self.assertRaises(ValueError):
                RELEASE.verify_delivery(root, source, version)

    def test_wrong_variant_or_record(self):
        """@brief 记录中的调试变体、错误 ABI、长度与摘要都必须分别拒绝。

        文件内容保持不变，使每个失败只对应一个独立发布契约。
        每轮从原记录开始，不借前一次错误让后续反例碰巧通过。
        """
        root, original = self.delivery()
        for key, value in (("variant", "debug"), ("abi", "x86_64"), ("bytes", 0), ("sha256", "0" * 64)):
            record = dict(original)
            record[key] = value
            (root / "build-info.json").write_text(json.dumps(record))
            with self.subTest(key=key), self.assertRaises(ValueError):
                RELEASE.verify_delivery(root, SOURCE)

    def test_actual_abi(self):
        """@brief 即使记录声称 ARM64，也要拒绝 ZIP 内的 x86_64 库。

        摘要与大小均真实匹配，失败必须来自实际 JNI 布局。
        这样不能通过只信元数据的实现绕过架构校验。
        """
        root, _ = self.delivery(abi="x86_64")
        with self.assertRaises(ValueError):
            RELEASE.verify_delivery(root, SOURCE)

    def test_failure_preserves_candidate(self):
        """@brief 损坏源包不能覆盖已成功交付的候选。

        使用落地文件的原始字节比较，不能只检查目录仍然存在。
        出错后不得残留会阻挡下一轮交付的 previous 目录。
        """
        root, _ = self.delivery()
        destination = self.m_root / "release/android-arm64"
        apk = RELEASE.stage_delivery(root, destination, SOURCE)
        before = apk.read_bytes()
        source = root / apk.name
        source.write_bytes(source.read_bytes() + b"damaged")
        with self.assertRaises(ValueError):
            RELEASE.stage_delivery(root, destination, SOURCE)
        self.assertEqual(apk.read_bytes(), before)
        self.assertFalse(destination.with_name(".android-arm64.previous").exists())

    def test_link_and_transaction_guards(self):
        """@brief 不覆盖符号链接目标，也不擅自删除中断事务的回滚副本。

        两个反例都使用已验证的源包，确保边界检查确实针对目标目录。
        任何恢复需要保留证据，由调用者明确处理。
        """
        root, _ = self.delivery()
        destination = self.m_root / "android-arm64"
        destination.symlink_to(root, target_is_directory=True)
        with self.assertRaises(ValueError):
            RELEASE.stage_delivery(root, destination, SOURCE)
        destination.unlink()
        destination.with_name(".android-arm64.previous").mkdir()
        with self.assertRaises(ValueError):
            RELEASE.stage_delivery(root, destination, SOURCE)


if __name__ == "__main__":
    unittest.main()
