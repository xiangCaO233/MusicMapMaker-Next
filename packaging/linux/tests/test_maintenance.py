#!/usr/bin/env python3
"""在构建树内验证 DEB 维护脚本的源迁移、回滚和卸载边界。

测试不提供生产路径覆盖开关，而是复制模板后替换固定路径。
每次调用都把 /etc/apt 与包内 keyring 路径改为独立临时目录。
这样既能执行真实 shell/awk，也不会写入开发机的 APT 配置。
生成的日志、脚本与源文件位于 build/test_output，退出后统一清理。
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[3]
TEMPLATES = REPOSITORY / "packaging/linux"


class MaintenanceTest(unittest.TestCase):
    """每个用例拥有独立根目录，覆盖真实维护脚本而非重写其解析逻辑。

    所有写操作均落在构建输出树，不使用源码测试资源目录。
    setUp 创建的 keyring 仅测试路径与生命周期；签名验证由仓库测试负责。
    """

    def setUp(self):
        """搭建隔离 APT 环境并生成两个可直接执行的维护脚本。

        模板路径替换只用于测试副本，生产脚本继续使用固定系统路径。
        显式断言系统路径已经消失，避免未来模板改动造成测试逃逸。
        """
        output = REPOSITORY / "build/test_output"
        output.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="deb-maintenance-", dir=output)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.apt = self.root / "etc/apt"
        self.parts = self.apt / "sources.list.d"
        self.parts.mkdir(parents=True)
        self.key = self.root / "usr/share/keyrings/mmm-archive-keyring.gpg"
        self.key.parent.mkdir(parents=True)
        self.key.write_bytes(b"packaged-key-fixture")
        self.canonical = self.parts / "mmm.sources"
        self.scripts = {}
        for name in ("postinst", "postrm"):
            # 生产脚本没有环境重定向开关；测试只修改自己的副本。
            # 替换整个目录前缀可以同时覆盖 sources.list 和源目录。
            template = (TEMPLATES / f"{name}.in").read_text(encoding="utf-8")
            template = template.replace("/etc/apt", str(self.apt))
            template = template.replace("/usr/share/keyrings/mmm-archive-keyring.gpg", str(self.key))
            template = template.replace("@MMM_DEB_ARCHITECTURE@", "amd64")
            self.assertNotIn("/etc/apt", template.replace(str(self.apt), "ISOLATED_APT"))
            self.assertNotIn("/usr/share/keyrings", template.replace(str(self.key), "ISOLATED_KEY"))
            script = self.root / name
            script.write_text(template, encoding="utf-8")
            self.scripts[name] = script

    def run_script(self, name, action, *, success=True, env=None):
        """执行真实脚本，并将错误作为用例失败证据返回。

        不使用 shell=True，动作参数无法变成额外命令。
        调用超时用于测试失败诊断，不用于维护脚本的生产同步。
        """
        result = subprocess.run(["sh", str(self.scripts[name]), action],
                                text=True, capture_output=True, env=env, timeout=10)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            # 失败用例必须证明脚本实际拒绝了操作，不能仅靠后续内容比较。
            # dpkg 通过非零状态识别维护阶段失败，stderr 保留诊断上下文。
            self.assertNotEqual(result.returncode, 0, result.stderr)
        return result

    def write(self, name, text):
        """写入一个候选源文件；资源和结果同属临时隔离根。

        以 UTF-8 保留管理员注释，便于比较迁移后无关源的内容。
        """
        path = self.parts / name
        path.write_text(text, encoding="utf-8")
        return path

    def snapshot(self):
        """捕获全部配置内容，用于失败、重入及错误恢复动作的比较。

        排除已被 cleanup 清除的事务目录；用相对路径稳定断言。
        """
        return {str(p.relative_to(self.apt)): p.read_bytes()
                for p in self.apt.rglob("*") if p.is_file()}

    def test_clean_install_and_configure_idempotency(self):
        """首次安装只注册单个严格签名源，重复配置字节稳定。

        同时断言配置显式使用架构、suite/component 以及包内公钥。
        """
        self.run_script("postinst", "configure")
        source = self.canonical.read_text()
        self.assertEqual(source, "Types: deb\nURIs: https://mmm.xiang233.top/apt/\n"
                         "Suites: stable\nComponents: main\nArchitectures: amd64\n"
                         f"Signed-By: {self.key}\n")
        # 第二次 configure 可能遇到已规范化的 canonical；快照检验整个目录。
        # 这也捕捉残留暂存区、重复标准段和无关源被格式化的问题。
        first = self.snapshot()
        self.run_script("postinst", "configure")
        self.assertEqual(first, self.snapshot())

    def test_list_migration_preserves_unrelated_lines(self):
        """旧 .list 可以混合选项、禁用行与无关软件源。

        域名相似、路径相似、查询参数以及普通说明注释不得误匹配。
        主 sources.list 与目录文件使用相同过滤入口。
        """
        other = ("# administrator comment about https://mmm.xiang233.top/apt/\n"
                 "deb https://deb.debian.org/debian trixie main\n"
                 "deb https://mmm.xiang233.top/apt-other/ stable main\n"
                 "deb https://mmm.xiang233.top/apt/?channel=other stable main\n"
                 "deb https://mmm.xiang233.top.evil/apt/ stable main\n")
        old = self.write("mixed.list", other +
                         "deb [arch=amd64 signed-by=/old.gpg] http://mmm.xiang233.top/apt stable main\n"
                         "# deb-src https://mmm.xiang233.top/apt/ old main\n")
        main = self.apt / "sources.list"
        main.write_text("deb https://mmm.xiang233.top/apt/ stable main\n" + other)
        self.run_script("postinst", "configure")
        self.assertEqual(old.read_text(), other)
        self.assertEqual(main.read_text(), other)

    def test_mixed_deb822_stanzas_and_multiline_uris(self):
        """混合文件的独立段与同段多个 URI 都应保留其他软件源。

        Enabled: no 属于旧源状态，迁移统一创建启用的 canonical。
        折行 URI 与大小写字段保持可解析，其他字段不能跟随 MMM 被删除。
        """
        unrelated = ("# keep stanza\nTypes: deb\nURIs: https://deb.debian.org/debian\n"
                     "Suites: trixie\nComponents: main\n")
        path = self.write("mixed.sources", unrelated + "\nTypes: deb\n"
                          "uRiS: http://mmm.xiang233.top/apt\n"
                          " https://mirror.example/repository\n"
                          " https://mmm.xiang233.top/apt/\n"
                          "Suites: stable\nComponents: main\nEnabled: no\n\n"
                          "Types: deb\nURIs: https://mmm.xiang233.top/apt/\n"
                          "Suites: obsolete\nComponents: main\n")
        self.run_script("postinst", "configure")
        text = path.read_text()
        self.assertIn(unrelated, text)
        self.assertIn("URIs: https://mirror.example/repository\n", text)
        self.assertIn("Enabled: no\n", text)
        self.assertNotIn("mmm.xiang233.top", text)
        self.assertNotIn("obsolete", text)

    def test_generated_sources_parse_with_real_apt(self):
        """让本机 APT 解析迁移结果，验证 deb822 输出而不触发联网更新。

        indextargets 只检查现有源和列表；所有配置与缓存目录明确隔离。
        禁用系统 sourceparts，开发机的私有源或无效源不会影响断言。
        """
        self.run_script("postinst", "configure")
        lists = self.root / "lists"
        lists.mkdir()
        (lists / "partial").mkdir()
        # 不执行 update，测试关注字段和 Signed-By 引用的解析边界。
        # 仓库签名与公钥有效性由独立的 signed repository 测试承担。
        result = subprocess.run([
            "apt-get", "indextargets", "-o", f"Dir::Etc::sourcelist={self.canonical}",
            "-o", "Dir::Etc::sourceparts=-", "-o", f"Dir::State::lists={lists}",
            "-o", "Debug::NoLocking=1",
        ], text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("Malformed", result.stderr)

    def test_canonical_can_already_contain_unrelated_stanza(self):
        """canonical 文件名不表示整个文件由本包独占。

        管理员放入其中的源、注释与原有权限需要随配置和卸载保留。
        """
        other = "# local mirror\nTypes: deb\nURIs: https://example.org/apt\nSuites: local\nComponents: main\n"
        self.canonical.write_text(other)
        self.canonical.chmod(0o640)
        self.run_script("postinst", "configure")
        self.assertIn(other, self.canonical.read_text())
        self.assertEqual(self.canonical.stat().st_mode & 0o777, 0o640)
        self.run_script("postrm", "remove")
        self.assertIn(other, self.canonical.read_text())
        self.assertNotIn("mmm.xiang233.top", self.canonical.read_text())

    def test_unrelated_file_is_byte_preserved(self):
        """未含 MMM 的文件不能被输出格式统一或改动 EOF 换行。

        包括管理员独立公钥引用与 permissions，维护逻辑不能改变其信任。
        """
        path = self.write("unrelated.sources", "Types: deb\nURIs: https://example.org\n"
                          "Suites: stable\nSigned-By: /custom.gpg")
        path.chmod(0o600)
        before = path.read_bytes()
        self.run_script("postinst", "configure")
        self.assertEqual(before, path.read_bytes())
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_missing_or_empty_key_leaves_sources_unchanged(self):
        """公钥缺失或空文件必须在源修改前报错。

        源 migration 不可留下已移除的历史条目或半生成 canonical。
        """
        self.write("old.list", "deb http://mmm.xiang233.top/apt stable main\n")
        original = self.snapshot()
        for empty in (False, True):
            with self.subTest(empty=empty):
                # 两种失败都发生在源解析前；源内容和 canonical 缺失状态一致。
                # 第二个分支重建空 key，不依赖第一个分支删除后的 inode。
                if empty:
                    self.key.write_bytes(b"")
                else:
                    self.key.unlink()
                self.run_script("postinst", "configure", success=False)
                self.assertEqual(original, self.snapshot())

    def test_unsafe_late_file_does_not_commit_earlier_file(self):
        """后续源的符号链接拒绝改写，不应提前提交已过滤的其他文件。

        链接指向隔离根中的文件，故测试不会读取或修改系统位置。
        """
        self.write("aaa.list", "deb https://mmm.xiang233.top/apt stable main\n")
        target = self.root / "outside.sources"
        target.write_text("Types: deb\nURIs: https://mmm.xiang233.top/apt/\nSuites: stable\n")
        (self.parts / "zzz.sources").symlink_to(target)
        # 失败源按 glob 排序晚于正常 MMM 文件，覆盖延迟预检的重要边界。
        # 若脚本逐个读取逐个提交，aaa.list 会已被提前清空。
        before = self.snapshot()
        self.run_script("postinst", "configure", success=False)
        self.assertEqual(before, self.snapshot())
        self.assertFalse(self.canonical.exists())

    def test_duplicate_uri_fields_fail_before_commit(self):
        """含 MMM 的异常重复 URIs 字段不能猜测合并语义。

        无关格式异常不归维护脚本审计，测试只覆盖会改写的段。
        """
        self.write("aaa.list", "deb https://mmm.xiang233.top/apt/ stable main\n")
        self.write("broken.sources", "Types: deb\nURIs: https://mmm.xiang233.top/apt/\n"
                   "URIs: https://example.org/apt/\nSuites: stable\n")
        before = self.snapshot()
        self.run_script("postinst", "configure", success=False)
        self.assertEqual(before, self.snapshot())

    def test_commit_failure_restores_already_replaced_files(self):
        """第二次 rename 失败时，首次替换也需从事务备份恢复。

        PATH 只替换测试进程的 mv，备份恢复使用真正的 cp。
        故障发生在准备完成后，能区分预检失败与实际提交回滚。
        """
        self.write("aaa.list", "deb https://mmm.xiang233.top/apt/ stable main\n")
        self.write("bbb.list", "deb https://mmm.xiang233.top/apt/ stable main\n")
        before = self.snapshot()
        binary = self.root / "bin"
        binary.mkdir()
        counter = self.root / "mv-counter"
        wrapper = binary / "mv"
        wrapper.write_text("#!/bin/sh\n" + f"counter='{counter}'\n"
                           "count=0\n[ ! -f \"$counter\" ] || count=$(cat \"$counter\")\n"
                           "count=$((count + 1))\nprintf '%s' \"$count\" > \"$counter\"\n"
                           "[ \"$count\" -ne 2 ] || exit 1\nexec /bin/mv \"$@\"\n")
        wrapper.chmod(0o755)
        # 测试故障仅作用于该子进程；不会替换系统工具或后续用例的 PATH。
        # 第二次提交返回失败，而 rollback 所需 cp/rm 仍使用真实工具。
        environment = dict(os.environ, PATH=f"{binary}:{os.environ['PATH']}")
        self.run_script("postinst", "configure", success=False, env=environment)
        self.assertEqual(before, self.snapshot())
        self.assertFalse(self.canonical.exists())

    def test_remove_and_purge_without_payload_key(self):
        """普通卸载清理源，避免包 payload 公钥被 dpkg 删除后的 APT 错误。

        purge 重入无需公钥，用户 pin 与其余源仍然存在。
        单纯 MMM 文件可删除；混合文件不能按文件名整体删除。
        """
        self.run_script("postinst", "configure")
        mixed = self.write("mixed.list", "deb https://mmm.xiang233.top/apt/ stable main\n"
                           "deb https://example.org/apt stable main\n")
        preferences = self.apt / "preferences.d"
        preferences.mkdir()
        pin = preferences / "mmm"
        # pin 可以阻止本产品更新，但仍是用户策略，卸载脚本不能擅自移除。
        # 快照包含 preferences.d，任何跨出源配置范围的清理都会被捕捉。
        pin.write_text("Package: musicmapmaker-next\nPin: version *\nPin-Priority: -1\n")
        # 模拟 dpkg 已卸载 payload，此时 postrm 不能依赖包内公钥或 helper。
        # 若 remove 只留下 canonical，下一次 APT 更新会引用缺失的 key。
        # 清理源而保留 user pin，使 remove/purge 均能在此状态重入。
        self.key.unlink()
        self.run_script("postrm", "remove")
        self.assertFalse(self.canonical.exists())
        self.assertEqual(mixed.read_text(), "deb https://example.org/apt stable main\n")
        after_remove = self.snapshot()
        self.run_script("postrm", "purge")
        self.run_script("postrm", "remove")
        self.assertEqual(after_remove, self.snapshot())
        self.assertTrue(pin.exists())

    def test_upgrade_and_abort_actions_keep_new_source(self):
        """旧版本 postrm 的升级和恢复动作不能删除新版本的标准源。

        postinst 的非 configure 动作同样不应试图读取包公钥。
        对 key 缺失的情形也保证成功，符合错误恢复阶段可用工具边界。
        """
        self.run_script("postinst", "configure")
        self.key.unlink()
        before = self.snapshot()
        for action in ("upgrade", "failed-upgrade", "abort-install", "abort-upgrade", "disappear"):
            with self.subTest(action=action):
                self.run_script("postrm", action)
                self.assertEqual(before, self.snapshot())
        for action in ("abort-upgrade", "abort-remove", "abort-deconfigure", "triggered"):
            self.run_script("postinst", action)
            self.assertEqual(before, self.snapshot())

    def test_architecture_fallback_and_reject_invalid_token(self):
        """未配置架构时采用 dpkg 值；非法配置必须在写文件前失败。

        对本机值仅比较字段内容，不假设测试机器为 amd64。
        """
        script = self.scripts["postinst"]
        template = script.read_text()
        script.write_text(template.replace("architecture='amd64'", "architecture=''"))
        self.run_script("postinst", "configure")
        native = subprocess.check_output(["dpkg", "--print-architecture"], text=True).strip()
        self.assertIn(f"Architectures: {native}\n", self.canonical.read_text())
        before = self.snapshot()
        script.write_text(template.replace("architecture='amd64'", "architecture='bad value'"))
        self.run_script("postinst", "configure", success=False)
        self.assertEqual(before, self.snapshot())


if __name__ == "__main__":
    unittest.main()
