#!/usr/bin/env python3
"""用微型 DEB 与临时 GnuPG 身份验收仓库生成器。

测试只访问系统工具与临时目录，不安装软件，不连接网络，不读取个人密钥。
运行入口：python3 scripts/apt/test_generate_repository.py
"""

import gzip
import fcntl
import getpass
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


# /// @brief 定位公开脚本入口，测试从任意调用位置都使用绝对路径。
SCRIPT = Path(__file__).resolve().parent / "generate_repository.py"
# /// @brief 所有外部测试命令统一在项目根执行。
PROJECT = SCRIPT.parents[2]


# /// @brief 覆盖签名信任链、真实架构和增量发布边界。
class RepositoryTest(unittest.TestCase):

    @classmethod
    # /// @brief 共享一次测试钥生成，避免每个独立用例重复消耗熵和时间。
    # /// @warning 生成的身份仅在本测试进程的 TemporaryDirectory 生命周期内有效。
    def setUpClass(cls):
        for command in ("dpkg-deb", "apt-ftparchive", "gpg", "gpgv", "gpgconf", "flock", "apt-get", "apt-cache"):
            if not shutil.which(command):
                # 环境缺少发布工具时明示跳过，不能误报测试已经覆盖签名。
                cls.skipTest(cls, f"required command unavailable: {command}")
        cls.key_temp = tempfile.TemporaryDirectory(prefix="mmm-apt-test-key-")
        # /// @brief 所有测试实例共享的临时身份根，不与输出仓库混存。
        cls.home = Path(cls.key_temp.name) / "gnupg"
        cls.home.mkdir(mode=0o700)
        # GnuPG 要求身份目录只能由当前用户访问，避免无关权限警告掩盖失败。
        # 此钥仅用于临时目录；生产流程绝不能复用测试生成逻辑。
        cls.command(["gpg", "--homedir", str(cls.home), "--batch", "--pinentry-mode", "loopback",
                     "--passphrase", "", "--quick-generate-key", "MMM repository test",
                     "rsa2048", "sign", "1d"])
        public = cls.command(["gpg", "--homedir", str(cls.home), "--batch", "--with-colons",
                              "--list-keys"]).stdout.decode()
        # 从机器可读 fpr 记录获取完整指纹，测试同样禁止短 key ID。
        cls.fingerprint = next(line.split(":")[9] for line in public.splitlines()
                               if line.startswith("fpr:"))
        # 签名者来自本次刚生成的主钥，不接受宿主环境的任何默认身份。

    @classmethod
    # /// @brief 释放测试 agent，再删除临时钥，避免残留测试身份。
    def tearDownClass(cls):
        # 仅停止显式测试 homedir 对应的 agent，不触碰用户 agent。
        cls.command(["gpgconf", "--homedir", str(cls.home), "--kill", "all"])
        cls.key_temp.cleanup()

    @staticmethod
    # /// @brief 执行测试命令；返回完整诊断供断言失败时展示。
    # /// @return 未自动抛错的进程结果，让每个负面用例断言明确失败行为。
    def command(arguments):
        # 参数逐项传递，临时目录的空格或特殊字符不能变成 shell 表达式。
        return subprocess.run(arguments, cwd=PROJECT, capture_output=True, check=False)

    # /// @brief 每个用例创建独立输出根，发布状态不会在用例之间传播。
    def setUp(self):
        # 清理注册早于构建包，断言失败也不会把二进制遗留在源码资源目录。
        self.temp = tempfile.TemporaryDirectory(prefix="mmm-apt-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        # /// @brief 本用例的唯一数据根，fixture、仓库与 APT 缓存均限制于此。
        self.repo = self.root / "repository"
        self.counter = 0
        # /// @brief 单调用内唯一的 fixture 编号，允许构造多个同版本输入。

    # /// @brief 构建可解析微型包，输入文件名故意不包含真实架构。
    # /// @param payload 改变此值用于制造同身份不同内容的冲突输入。
    # /// @return 构建成功的临时 DEB，永远不安装到测试宿主机。
    def deb(self, version="1.0-1", architecture="amd64", payload="fixture", package="musicmapmaker-next"):
        self.counter += 1
        tree = self.root / f"fixture-{self.counter}"
        (tree / "DEBIAN").mkdir(parents=True)
        (tree / "opt" / "mmm").mkdir(parents=True)
        # 保留 Depends 等控制字段，以验证扫描工具没有丢失包契约。
        control = (f"Package: {package}\nVersion: {version}\nArchitecture: {architecture}\n"
                   "Maintainer: MMM test <test@example.invalid>\nDepends: libc6\n"
                   "Description: miniature repository fixture\n")
        (tree / "DEBIAN" / "control").write_text(control, encoding="utf-8")
        (tree / "opt" / "mmm" / "mmm").write_text(payload, encoding="utf-8")
        # 包内文件不执行，只负责提供真实 data.tar 内容参与归档校验。
        target = self.root / f"input-{self.counter}.deb"
        result = self.command(["dpkg-deb", "--root-owner-group", "--build", str(tree), str(target)])
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        return target

    # /// @brief 通过公共 CLI 发布，避免用内部函数测试替代实际调用契约。
    # /// @param extra 追加可覆盖参数，支持故意指定不存在签名钥的负面场景。
    # /// @return CLI 的原始退出码和输出，失败用例自行检查诊断内容。
    def generate(self, *debs, extra=()):
        return self.command([sys.executable, str(SCRIPT), "--output", str(self.repo),
                             "--signing-key", self.fingerprint, "--gnupg-home", str(self.home),
                             *extra, *(str(deb) for deb in debs)])

    # /// @brief 要求外部流程成功，保留 stderr 让故障定位具备上下文。
    def success(self, result):
        self.assertEqual(result.returncode, 0, result.stderr.decode())

    # /// @brief 读取客户端访问的原始索引，用于检查内容和压缩一致性。
    # /// @param architecture 目标客户端架构，与输入 DEB 文件名没有联系。
    def index(self, architecture="amd64"):
        return self.repo / "dists" / "stable" / "main" / f"binary-{architecture}" / "Packages"

    # /// @brief 验证两种签名、公钥导出及 Release→Packages→DEB 的校验链。
    def test_signed_release_and_package_checksums(self):
        # 这个用例独立验签与校验 hash，生成器自身成功退出不能替代验收。
        self.success(self.generate(self.deb()))
        distribution = self.repo / "dists" / "stable"
        keyring = self.repo / "mmm-archive-keyring.gpg"
        # gpgv 只有导出的公钥可用；不会从个人 keyring 偷渡信任关系。
        verifier = ["gpgv", "--homedir", str(self.home), "--keyring", str(keyring)]
        self.success(self.command(verifier + [str(distribution / "InRelease")]))
        self.success(self.command(verifier + [str(distribution / "Release.gpg"), str(distribution / "Release")]))
        release = (distribution / "Release").read_text()
        # 直接核对 Release 中的实际字节尺寸及相对路径，防止扫描根错误。
        packages = self.index().read_bytes()
        checksum = hashlib.sha256(packages).hexdigest()
        self.assertIn(f"{checksum} {len(packages):16d} main/binary-amd64/Packages", release)
        # 按哈希地址必须与普通索引完全一致，gzip 解压也必须等价。
        self.assertEqual((self.index().parent / "by-hash" / "SHA256" / checksum).read_bytes(), packages)
        # SHA512 是当前 APT 的优先选择，不能仅测试 SHA256 成功后误判完整。
        stronger = hashlib.sha512(packages).hexdigest()
        self.assertEqual((self.index().parent / "by-hash" / "SHA512" / stronger).read_bytes(), packages)
        self.assertEqual(gzip.decompress(self.index().with_suffix(".gz").read_bytes()), packages)
        fields = dict(line.split(": ", 1) for line in packages.decode().splitlines() if ": " in line)
        # 用 Packages 给出的 Filename 查找包，确保下载路径可从仓库根解析。
        archive = self.repo / fields["Filename"]
        self.assertEqual(fields["SHA256"], hashlib.sha256(archive.read_bytes()).hexdigest())
        self.assertEqual(fields["Size"], str(archive.stat().st_size))
        # 控制字段仍由 apt-ftparchive 维护，必须保留依赖关系。
        self.assertEqual(fields["Depends"], "libc6")

    # /// @brief CDN 页面与相对 APT 路径一致，上传失败不能更换签名索引。
    def test_cdn_links_and_upload_failure_preserve_release(self):
        # 第一次发布仍使用真实签名；页面下载地址必须直接落在 CDN。
        package = self.deb()
        self.success(self.generate(package, extra=("--cdn-base-url", "https://cdn.example.test")))
        self.assertIn('href="https://cdn.example.test/pool/', (self.repo / "index.html").read_text())
        # APT 仍使用标准相对 Filename，服务端只对归档入口重定向 CDN。
        self.assertIn("Filename: pool/", self.index().read_text())
        previous = (self.repo / "dists/stable/InRelease").read_bytes()
        helper = self.root / "failed-upload.py"
        helper.write_text("import sys\nsys.exit(7)\n")
        # 模拟云端上传失败，确认下一版本没有进入既有签名客户端视野。
        result = self.generate(self.deb("2.0-1"), extra=(
            "--cdn-base-url", "https://cdn.example.test", "--cdn-uploader", str(helper),
            "--cdn-python", sys.executable))
        self.assertNotEqual(result.returncode, 0)
        # 保留原签名正文，证明失败并非仅修改了候选包列表。
        self.assertEqual((self.repo / "dists/stable/InRelease").read_bytes(), previous)
        # 生成器只写临时区，失败后旧客户端仍能访问旧版本归档。
        self.assertNotIn("Version: 2.0-1", self.index().read_text())

    # /// @brief 新增版本与新架构不应丢失历史版本或旧哈希索引。
    def test_incremental_multiversion_and_real_architecture(self):
        # 旧索引 hash 在发布前采样，更新后检查的是前一代客户端访问地址。
        self.success(self.generate(self.deb()))
        old = self.index().read_bytes()
        old_hash = hashlib.sha256(old).hexdigest()
        note = self.repo / "operator-notes.txt"
        note.write_text("keep", encoding="utf-8")
        # 仓库外层允许操作者保存其它文件，发布工具只拥有清单中的路径。
        # 两份输入文件均无架构后缀，生成器必须读取 control 判断归属。
        self.success(self.generate(self.deb("2.0-1"), self.deb("2.0-1", "arm64")))
        self.assertEqual(self.index().read_text().count("Package: musicmapmaker-next"), 2)
        # arm64 索引只含对应架构，不能把另一架构同版本包误当成重复项。
        self.assertIn("Architecture: arm64", self.index("arm64").read_text())
        self.assertNotIn("Architecture: amd64", self.index("arm64").read_text())
        self.assertEqual((self.index().parent / "by-hash" / "SHA256" / old_hash).read_bytes(), old)
        self.assertEqual(note.read_text(), "keep")
        # Release 的架构集合是客户端入口，必须随新增架构一并更新。
        self.assertIn("Architectures: amd64 arm64", (self.repo / "dists/stable/Release").read_text())

    # /// @brief 相同归档可重入；不同字节的同版本包必须失败且保留旧发布。
    def test_identical_reimport_and_conflicting_version(self):
        # 正面重入首先成功，再比较冲突前后的已发布 InRelease 字节。
        original = self.deb()
        self.success(self.generate(original))
        self.success(self.generate(original))
        previous = (self.repo / "dists/stable/InRelease").read_bytes()
        # 新包 payload 变化必然影响内容 hash，不依赖归档时间戳差异。
        conflict = self.generate(self.deb(payload="changed"))
        self.assertNotEqual(conflict.returncode, 0)
        self.assertIn("different content", conflict.stderr.decode())
        self.assertEqual((self.repo / "dists/stable/InRelease").read_bytes(), previous)

    # /// @brief 用户创建的索引、公钥以及符号链接都不能被接管。
    def test_unmanaged_collision_and_symlink(self):
        # 未受管公钥即使位于标准输出路径，也不能被生成器覆盖。
        self.repo.mkdir()
        public = self.repo / "mmm-archive-keyring.gpg"
        public.write_bytes(b"operator-owned")
        self.assertNotEqual(self.generate(self.deb()).returncode, 0)
        self.assertEqual(public.read_bytes(), b"operator-owned")
        public.unlink()
        # 再用同一路径测试链接逃逸，链接目标位于仓库根之外。
        target = self.root / "operator-file"
        target.write_bytes(b"outside")
        public.symlink_to(target)
        self.assertNotEqual(self.generate(self.deb()).returncode, 0)
        # 失败后直接读取目标证明不会误修改链接指向的数据。
        self.assertEqual(target.read_bytes(), b"outside")

    # /// @brief 无私钥签名不能发布，已有受管文件改动也不能被静默覆盖。
    def test_wrong_key_and_changed_managed_file(self):
        # 不存在指纹用于模拟发布环境缺钥，不能自动降级为 unsigned 仓库。
        first = self.deb()
        failed = self.generate(first, extra=("--signing-key", "0" * 40))
        self.assertNotEqual(failed.returncode, 0)
        self.assertFalse((self.repo / "dists/stable/InRelease").exists())
        # 同一目录在失败后仍能正确初始化，暂存区不得被当成历史数据。
        self.success(self.generate(first))
        self.index().write_text("external change", encoding="utf-8")
        # 修改受管索引模拟手工编辑，清单校验应要求维护者明确处理。
        changed = self.generate(first)
        self.assertNotEqual(changed.returncode, 0)
        self.assertIn("was removed or changed", changed.stderr.decode())
        self.assertEqual(self.index().read_text(), "external change")

    # /// @brief all 包进入每个显式架构，避免只产生 APT 不会请求的 binary-all。
    def test_all_package_in_requested_architectures(self):
        # 复用一个 all DEB 检查各机器架构入口，不引入多个同身份假包。
        self.success(self.generate(self.deb(architecture="all"),
                                   extra=("--architecture", "amd64", "--architecture", "arm64")))
        for architecture in ("amd64", "arm64"):
            self.assertIn("Architecture: all", self.index(architecture).read_text())

    # /// @brief 同一仓库只允许一个发布者，第二个调用应立即失败。
    def test_concurrent_publisher_is_rejected(self):
        # 持有锁的进程是测试本身，生成器必须识别其它打开文件描述的锁。
        self.repo.mkdir()
        # 用真实内核文件锁模拟另一个发布进程，不依赖睡眠或时序猜测。
        with (self.repo / ".mmm-apt.lock").open("a+b") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            result = self.generate(self.deb())
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("another publisher", result.stderr.decode())
        # 失败调用不能提前发布索引，解锁后同一输入仍应可以正常发布。
        self.assertFalse((self.repo / "dists/stable/InRelease").exists())
        self.success(self.generate(self.deb()))

    # /// @brief 使用真实 APT 的隔离状态目录验证源和候选版本。
    def test_apt_accepts_signed_local_source(self):
        # file URI 在本地完成仓库协议验证，不依赖 DNS、HTTPS 或生产服务。
        self.success(self.generate(self.deb()))
        apt = self.root / "apt-client"
        (apt / "lists/partial").mkdir(parents=True)
        (apt / "cache/archives/partial").mkdir(parents=True)
        (apt / "status").write_text("", encoding="utf-8")
        # 空 status 表示未安装任何包，因此候选版本只能来自刚生成的源。
        sources = apt / "mmm.sources"
        # 所有源与状态路径均显式覆盖，绝不触碰系统 sources 或 dpkg 状态。
        sources.write_text(f"Types: deb\nURIs: file:{self.repo}\nSuites: stable\nComponents: main\n"
                           f"Signed-By: {self.repo}/mmm-archive-keyring.gpg\n", encoding="utf-8")
        options = ["-o", f"Dir::Etc::sourcelist={sources}", "-o", "Dir::Etc::sourceparts=-",
                   "-o", f"Dir::State::lists={apt}/lists", "-o", f"Dir::State::status={apt}/status",
                   "-o", f"Dir::Cache={apt}/cache", "-o", "APT::Architecture=amd64",
                   "-o", "APT::Architectures::=amd64", "-o", "APT::Sandbox::User=" + getpass.getuser(),
                   "-o", "Acquire::AllowInsecureRepositories=false", "-o", "APT::Get::List-Cleanup=false"]
        self.success(self.command(["apt-get", *options, "update"]))
        # apt-cache 复用同一 lists 与 status，避免误读系统缓存得到候选版本。
        policy = self.command(["apt-cache", *options, "policy", "musicmapmaker-next"])
        self.success(policy)
        self.assertIn("Candidate: 1.0-1", policy.stdout.decode())
        # 发布成功后所有权清单包含信任入口，支持下一次安全增量更新。
        manifest = json.loads((self.repo / ".mmm-apt-manifest.json").read_text())
        self.assertIn("dists/stable/InRelease", manifest["files"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
