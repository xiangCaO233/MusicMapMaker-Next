#!/usr/bin/env python3
"""隔离验证发布脚本的本地预检和远端调用边界，不进行网络连接。"""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


# /// @brief 实际发布入口，所有测试均通过外部 CLI 调用。
SCRIPT = Path(__file__).resolve().parent / "publish_repository.sh"
PROJECT = SCRIPT.parents[2]


# /// @brief 每个测试模拟自己的 SSH/SCP，不需要任何生产主机身份。
class PublishTest(unittest.TestCase):
    # /// @brief 创建仅供本测试的签名公钥身份，并导出到微型 DEB。
    @classmethod
    def setUpClass(cls):
        # fixture 依赖是真实系统工具，网络工具随后由本测试替换。
        for tool in ("bash", "dpkg-deb", "gpg", "gpgconf", "tar"):
            if not shutil.which(tool):
                cls.skipTest(cls, f"required fixture tool unavailable: {tool}")
        cls.identity = tempfile.TemporaryDirectory(prefix="mmm-publish-key-")
        # 临时身份与用例数据分开，测试失败仍能在类清理时释放 agent。
        cls.home = Path(cls.identity.name) / "gnupg"
        cls.home.mkdir(mode=0o700)
        # 私钥不上传；helper 只读取包内公钥，与本地个人钥环完全隔离。
        cls.checked(["gpg", "--homedir", str(cls.home), "--batch", "--pinentry-mode", "loopback",
                     "--passphrase", "", "--quick-generate-key", "MMM publish test", "rsa2048", "sign", "1d"])
        listing = cls.checked(["gpg", "--homedir", str(cls.home), "--with-colons", "--list-keys"]).decode()
        cls.fingerprint = next(line.split(":")[9] for line in listing.splitlines() if line.startswith("fpr:"))
        # 导出二进制公钥后，DEB fixture 的信任材料与预期完整指纹关联。
        cls.public = cls.checked(["gpg", "--homedir", str(cls.home), "--export", cls.fingerprint])

    # /// @brief 清理测试 agent 与身份；不影响个人 GnuPG agent。
    @classmethod
    def tearDownClass(cls):
        cls.checked(["gpgconf", "--homedir", str(cls.home), "--kill", "all"])
        cls.identity.cleanup()

    # /// @brief fixture 构造必须真实成功，错误保留 stderr 供定位。
    @staticmethod
    def checked(arguments):
        result = subprocess.run(arguments, cwd=PROJECT, capture_output=True, check=False)
        if result.returncode:
            # unittest 的断言表达工具失败，无需应用级异常恢复分支。
            unittest.TestCase().fail(result.stderr.decode())
        return result.stdout

    # /// @brief 建立本用例的 mock 工具和调用日志，PATH 仅在子进程中生效。
    def setUp(self):
        # 子进程 PATH 不修改父进程环境，其它测试或用户命令不受影响。
        self.directory = tempfile.TemporaryDirectory(prefix="mmm-publish-test-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.log = self.root / "network.jsonl"
        self.log.touch()
        # 空日志是预检失败没有联网的独立证据，不只依据脚本提示推断。
        # mock 只记录操作，不执行远端命令；临时服务器目录也是本用例数据。
        mock = '''#!/usr/bin/env python3
import json, os, pathlib, sys
log = pathlib.Path(os.environ["MMM_MOCK_LOG"])
with log.open("a") as output:
    output.write(json.dumps([pathlib.Path(sys.argv[0]).name, *sys.argv[1:]]) + "\\n")
if pathlib.Path(sys.argv[0]).name == "ssh":
    command = sys.argv[-1]
    if "mktemp -d" in command:
        print(os.environ.get("MMM_MOCK_JOB", "/home/podman/.local/share/mmm-apt/incoming.ABC123"))
        sys.exit(int(os.environ.get("MMM_MOCK_STAGING_STATUS", "0")))
    elif command.startswith("python3 "):
        sys.exit(int(os.environ.get("MMM_MOCK_PUBLISH_STATUS", "0")))
'''
        for name in ("ssh", "scp"):
            # 两个 mock 都使用固定实现，不回退执行真实同名工具。
            path = self.bin / name
            path.write_text(mock, encoding="utf-8")
            path.chmod(0o755)
        self.env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}", MMM_MOCK_LOG=str(self.log))
        self.counter = 0
        # 同一用例能生成多个包，计数器只用于输入文件唯一命名。

    # /// @brief 生成包含指定公钥的真实 DEB，不执行包载荷或安装维护脚本。
    # /// @param package 允许故意构造旧探针包以检查产品身份拒绝。
    # /// @param key 是否包含公钥，缺失情况必须在联网前失败。
    def deb(self, package="musicmapmaker-next", key=True):
        # 包版本架构是可比较的确定值，断言不依赖宿主实际安装状态。
        self.counter += 1
        tree = self.root / f"fixture-{self.counter}"
        (tree / "DEBIAN").mkdir(parents=True)
        control = (f"Package: {package}\nVersion: 0.6.2-1\nArchitecture: amd64\n"
                   "Maintainer: MMM test <test@example.invalid>\nDescription: publish fixture\n")
        (tree / "DEBIAN/control").write_text(control, encoding="utf-8")
        if key:
            # 与真实安装包保持同一路径，tar 仅提取这一项进行指纹检查。
            key_dir = tree / "usr/share/keyrings"
            key_dir.mkdir(parents=True)
            (key_dir / "mmm-archive-keyring.gpg").write_bytes(self.public)
        # 本地文件名含空格和分号，验证数组传参不会进行额外 shell 解释。
        target = self.root / f"fixture {self.counter};safe.deb"
        self.checked(["dpkg-deb", "--root-owner-group", "--build", str(tree), str(target)])
        return target

    # /// @brief 运行实际 bash 脚本，生产值只用于 mock 日志，绝不发送网络请求。
    def publish(self, *arguments):
        # 只传测试环境到本次进程，secret 不写入日志或命令行。
        return subprocess.run(["bash", str(SCRIPT), "--signing-key", self.fingerprint, *map(str, arguments)],
                              cwd=PROJECT, env=self.env, capture_output=True, check=False)

    # /// @brief 返回可解析的调用记录，避免只匹配 stdout 推断网络动作。
    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    # /// @brief 所有 DEB 先验签身份，再上传唯一文件名并清理本次 job。
    def test_multiple_packages_and_cleanup(self):
        # 同时验证上传数量与远端归档参数，防止只处理第一个 DEB。
        first, second = self.deb(), self.deb()
        result = self.publish(first, second)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        calls = self.calls()
        # 第一次 SSH 必须请求唯一 mktemp；后续发布与清理使用同一目录。
        self.assertIn("mktemp -d /home/podman/.local/share/mmm-apt/incoming.XXXXXX", calls[0][-1])
        self.assertEqual(len([call for call in calls if call[0] == "scp"]), 4)
        publication = next(call[-1] for call in calls if call[-1].startswith("python3 "))
        self.assertIn("--output /home/podman/data/caddy/web/mmm-apt", publication)
        # 正式入口必须启用 CDN 校验，不能回退到只发布服务器本地归档。
        self.assertIn("--cdn-base-url https://downloads.xiang233.top", publication)
        self.assertIn("--cdn-uploader", publication)
        # 标准仓库根独立于网站部署，mock 不应看到任何服务重启调用。
        self.assertIn("package-0.deb", publication)
        self.assertIn("package-1.deb", publication)
        # 本地元字符文件名完全不出现在远端命令中。
        self.assertNotIn("fixture ", publication)
        self.assertEqual(calls[-1][-1], "rm -rf -- /home/podman/.local/share/mmm-apt/incoming.ABC123")
        self.assertIn("HTTPSURL=https://mmm.xiang233.top/apt/", result.stdout.decode())
        self.assertEqual(result.stdout.decode().count("IMPORTED=musicmapmaker-next|0.6.2-1|amd64"), 2)

    # /// @brief 发布失败仍清理本次 job，并保留发布退出码，禁止删除仓库。
    def test_remote_failure_preserves_repository(self):
        # 返回非标准错误码，证明 cleanup 没有把失败改成成功或其它状态。
        self.env["MMM_MOCK_PUBLISH_STATUS"] = "37"
        result = self.publish(self.deb())
        self.assertEqual(result.returncode, 37)
        self.assertTrue(self.calls()[-1][-1].startswith("rm -rf -- /home/podman/.local/share/mmm-apt/incoming."))
        # 成功 metadata 不能在失败后出现，避免 CI 将错误发布标为完成。
        self.assertNotIn("HTTPSURL=", result.stdout.decode())
        self.assertFalse(any("rm -rf -- /home/podman/data" in call[-1] for call in self.calls()))

    # /// @brief 参数的 shell 注入、相对路径和非法端口均在网络前拒绝。
    def test_unsafe_options_do_not_connect(self):
        # 每种拒绝值覆盖不同边界：SSH 目的地、端口、路径、身份与根目录。
        deb = self.deb()
        unsafe = (("--remote", "podman@host;id"), ("--port", "22;id"),
                  ("--repo", "/tmp/repo;id"), ("--repo", "/tmp/../repo"),
                  ("--gnupg-home", "/tmp/home $(id)"), ("--signing-key", "12345678"),
                  ("--port", "65536"), ("--repo", "/"))
        for option, value in unsafe:
            with self.subTest(option=option, value=value):
                result = self.publish(option, value, deb)
                self.assertNotEqual(result.returncode, 0)
        # 每次参数拒绝都必须完全没有调用 SSH 或 SCP。
        self.assertEqual(self.calls(), [])

    # /// @brief 旧探针包、错钥和缺钥都不能进入生产暂存目录。
    def test_package_and_key_preflight(self):
        # 公钥缺失和公钥不匹配是不同错误，两者均必须在创建远端 job 前拒绝。
        old = self.deb(package="musicmapmaker-next-debianruntime")
        no_key = self.deb(key=False)
        ordinary = self.deb()
        for arguments in ((old,), (no_key,), ("--signing-key", "0" * 40, ordinary), ("relative.deb",)):
            with self.subTest(arguments=arguments):
                result = self.publish(*arguments)
                self.assertNotEqual(result.returncode, 0)
        # 即使最后一个包失败，此前包也只能完成本地检查，不能提前上传。
        self.assertNotEqual(self.publish(ordinary, old).returncode, 0)
        self.assertEqual(self.calls(), [])

    # /// @brief SSH 意外返回其它路径时既不上传，也不能拿该路径执行删除。
    def test_unexpected_staging_path_is_not_used(self):
        # 模拟远端异常输出，不能把服务器网站根误当作可删除临时目录。
        self.env["MMM_MOCK_JOB"] = "/home/podman/data/caddy/web"
        result = self.publish(self.deb())
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(self.calls()), 1)
        self.assertIn("unexpected remote staging path", result.stderr.decode())

    # /// @brief 创建 job 的 SSH 失败即使有 stdout，也不能授权清理该输出路径。
    def test_failed_staging_stdout_is_not_cleaned(self):
        # 模拟服务器错误信息在 stdout 中恰好包含站点目录的危险场景。
        self.env["MMM_MOCK_JOB"] = "/home/podman/data/caddy/web"
        self.env["MMM_MOCK_STAGING_STATUS"] = "44"
        result = self.publish(self.deb())
        # 原退出码必须保留；不可信输出既不进入上传路径也不进入删除命令。
        self.assertEqual(result.returncode, 44)
        self.assertEqual(len(self.calls()), 1)

    # /// @brief 帮助属于只读操作，不需要 DEB、私钥、网络或服务状态。
    def test_help(self):
        # --help 是发布前了解接口的入口，必须立即完成并保持网络日志为空。
        result = self.publish("--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("--gnupg-home", result.stdout.decode())
        self.assertEqual(self.calls(), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
