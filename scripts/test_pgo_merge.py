"""离线验证 PGO 下载身份、样本门槛和事务输出。

每个 HTTP 请求只访问回环地址和用例拥有的文件。真实 clang 运行两次以上，
以可区分的函数调用次数生成原始样本，再由 llvm-profdata 检查合并权重。
"""

from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest

from pgo_merge import EXIT_NO_PROFILES


SCRIPT = Path(__file__).with_name("pgo_merge.py").resolve()


@contextmanager
def serve_profiles(routes):
    """提供精确到查询参数的回环响应，并在用例结束后释放套接字。

    使用请求原文选择响应，可制造 URL 路径相同、查询参数不同的真实样本。
    """
    # 独立记录每一个请求。测试不通过最终文件名猜测服务是否提供了某个样本，
    # 因为旧缓存缺陷会只下载一份，然后把同一文件两次传给 LLVM。
    requests = []

    # 服务只返回夹具提供的字节，没有任意文件名的磁盘访问路径。
    # 同一路径的两份查询结果能完全独立指定内容。
    class Handler(BaseHTTPRequestHandler):
        """只读取预置的内容，未知请求以 404 失败。"""

        def do_GET(self):
            # 保留原始路径和查询参数，不能用 basename 或解码后路径做这里的映射。
            requests.append(self.path)
            content = routes.get(self.path)
            # 失配 URL 必须可见，返回默认响应会掩盖客户端拼接错误。
            if content is None:
                self.send_error(404)
                return
            self.send_response(200)
            # 明确 Content-Length，使 urllib 也执行它自己的完整性检查。
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)

        def log_message(self, *_):
            """避免测试输出混入 HTTP 请求日志，断言直接读取请求列表。"""

    # 使用系统分配的端口，避免多个本地探针同跑时相互占用。
    # 监听只在当前 with 作用域有效，操作结束后应立即释放端口。
    with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield f"http://127.0.0.1:{server.server_port}", requests
        finally:
            # 等回环处理停止后再清理夹具，避免残留线程访问临时路径。
            server.shutdown()
            thread.join()


class PgoMergeTest(unittest.TestCase):
    """从真实采样读取计数，并检查旧输出和每次请求的临时缓存。"""

    @classmethod
    def setUpClass(cls):
        """构建带插桩探针，四次运行分别产生不同的调用计数。"""
        # LLVM 原始 profile 版本会随工具链变化，优先从同一安装查找配套工具。
        clang = os.environ.get("MMM_TEST_CLANG") or shutil.which("clang")
        if clang is None:
            raise unittest.SkipTest("clang is unavailable")
        clang = Path(clang).resolve()
        sibling_tool = clang.with_name("llvm-profdata" + clang.suffix)
        cls.profdata = str(sibling_tool) if sibling_tool.is_file() else shutil.which("llvm-profdata")
        # 缺少工具时由 unittest 报告跳过，不能把无法产生原始样本误报为通过。
        if cls.profdata is None:
            raise unittest.SkipTest("llvm-profdata is unavailable")

        # 构建输入和运行产物归此测试类，不在源码资源目录产生 profraw。
        # 各方法只使用已读出的源字节，夹具之间不会改变样本计数。
        sandbox = tempfile.TemporaryDirectory()
        cls.addClassCleanup(sandbox.cleanup)
        root = Path(sandbox.name)
        source = root / "profile.c"
        # 预编译探针以参数控制运行次数，每次执行都在原生进程中写出样本。
        executable = root / ("profile.exe" if os.name == "nt" else "profile")
        # probe 的每次进入都会递增函数计数；主函数按参数产生已知权重。
        source.write_text(
            "#include <stdlib.h>\n"
            "static volatile int total;\n"
            "int probe(void) { return ++total; }\n"
            "int main(int argc, char** argv) {\n"
            "    if (argc != 2) return 1;\n"
            "    for (int n = atoi(argv[1]); n > 0; --n) probe();\n"
            "    return 0;\n"
            "}\n", encoding="utf-8"
        )
        # 真实插桩覆盖 LLVM 版本、函数哈希和计数结构，避免单靠 mock merge 通过。
        subprocess.run([str(clang), "-fprofile-instr-generate", str(source),
                        "-o", str(executable)], check=True, capture_output=True)
        # 多个样本函数哈希相同而命中数不同，可以精确判断合并器有没有重复计权。
        cls.samples = []
        for count in range(1, 5):
            sample = root / f"{count}.profraw"
            # 独立输出禁止 LLVM 的进程内自动累积干扰每份样本的已知计数。
            subprocess.run([str(executable), str(count)], check=True,
                           env={**os.environ, "LLVM_PROFILE_FILE": str(sample)})
            cls.samples.append(sample.read_bytes())

    def setUp(self):
        """每一轮输出可预置旧制品，所有缓存都受独立临时根控制。"""
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        # 同时设置进程常见的临时目录环境变量，避免平台选择其他根导致清理断言失效。
        self.cache = self.root / "tmp"
        self.cache.mkdir()
        self.output = self.root / "merged.profdata"
        # 旧内容用可识别字节模拟，以检出失败后错误覆盖或清空。
        self.output.write_bytes(b"previous profile")

    def run_merge(self, *args):
        """运行真实 CLI 并确认返回时所有本轮暂存空间已清理。"""
        # 子进程退出码是调用方使用的契约，本用例不绕过 CLI 访问实现内部函数。
        # 时间上界也保证网络或 LLVM 工具意外卡住时测试会返回诊断。
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--output", str(self.output),
             "--profdata", self.profdata, *args],
            capture_output=True, text=True, timeout=40,
            env={**os.environ, **dict.fromkeys(("TMPDIR", "TEMP", "TMP"), str(self.cache))},
        )
        # 在检查返回码前检查缓存，使错误退出和清理失败能分别定位。
        # 输出所在卷也应没有写出失败留下的候选目录。
        self.assertFalse(list(self.cache.iterdir()), result.stdout)
        # 临时缓存和输出所在卷的事务目录分开，两者都不能遗留文件。
        self.assertFalse(list(self.root.glob(".pgo-output-*")), result.stdout)
        return result

    def assert_function_count(self, expected):
        """用 LLVM 读取最终制品，确认四份样本按正确次数贡献权重。"""
        # 使用正式 LLVM 读取索引，而不是直接从合并日志寻找样本个数。
        # 具体函数计数可以识别四份制品仍然存在、但其中一个被重复计入的场景。
        details = subprocess.check_output(
            [self.profdata, "show", "--counts", "--function=probe", str(self.output)],
            text=True,
        )
        self.assertRegex(details, rf"Function count: {expected}\b")

    def test_remote_url_identity_and_merge(self):
        """扁平化别名、相同 basename 与不同查询参数应各入列一次。"""
        # 旧实现会把 a/b 和 a_b 压平到一个名字，另两份则只在查询上不同。
        # 带 URL 片段的重复链接不会产生额外传输，也不能增加相同样本的权重。
        routes = {
            "/profiles/": (b'<a href="a/">sub</a><a href="a_b.profraw">flat</a>'
                           b'<a href="b.profraw?v=1">v1</a><a href="b.profraw?v=2">v2</a>'
                           b'<a href="b.profraw?v=2#same-response">duplicate</a>'),
            "/profiles/a/": b'<a href="b.profraw">nested</a>',
            "/profiles/a/b.profraw": self.samples[0],
            "/profiles/a_b.profraw": self.samples[1],
            "/profiles/b.profraw?v=1": self.samples[2],
            "/profiles/b.profraw?v=2": self.samples[3],
        }
        with serve_profiles(routes) as (base, requests):
            result = self.run_merge("--source-url", base + "/profiles/")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # 请求顺序可不同，但每份制品只能访问一次。随后还要检查合并后的权重。
        self.assertCountEqual(requests, routes.keys())
        self.assert_function_count(10)

    def test_remote_sample_shortage_keeps_previous_output(self):
        """已有一份下载但达不到门槛时，返回专用状态并保留旧制品。"""
        # 让真实下载先发生。仅提供空目录无法覆盖原先合并门槛前后的清理差别。
        routes = {"/profiles/": b'<a href="one.profraw">one</a>',
                  "/profiles/one.profraw": self.samples[0]}
        with serve_profiles(routes) as (base, requests):
            result = self.run_merge("--source-url", base + "/profiles/", "--min-files", "2")
        # 请求确实发生，验证清理包含下载后提前退出。
        # 旧输出预先存在。因此缺少新制品也必须反映在状态中，而不能静默成功。
        # 这里不修改门槛后重试，以免下一轮成功掩盖上一轮状态。
        self.assertIn("/profiles/one.profraw", requests)
        # 样本不足有专用返回值，上一轮文件即便存在也不能骗过配置的成功条件。
        self.assertEqual(result.returncode, EXIT_NO_PROFILES, result.stdout)
        self.assertEqual(self.output.read_bytes(), b"previous profile")

    def test_remote_merge_failure_keeps_previous_output(self):
        """工具收到下载的坏样本时，退出前回收候选和缓存。"""
        # HTTP 正常返回，故障由 LLVM 格式处理产生，确保验证了已经填充的缓存。
        routes = {"/profiles/": b'<a href="bad.profraw">bad</a>',
                  "/profiles/bad.profraw": b"invalid instrumentation data"}
        with serve_profiles(routes) as (base, requests):
            result = self.run_merge("--source-url", base + "/profiles/")
        self.assertIn("/profiles/bad.profraw", requests)
        self.assertEqual(result.returncode, 1, result.stdout)
        # 要求工具诊断，排除下载本身失败导致用例过早结束。
        self.assertIn("llvm-profdata merge failed", result.stdout)
        self.assertEqual(self.output.read_bytes(), b"previous profile")

    def test_real_premerged_download(self):
        """预合并文件须能由当前工具读取，验证失败不更新缓存输出。"""
        # 由同一个工具生成有效预合并制品，一次请求得到兼容索引。
        # 再提供同后缀的无效响应，确认只看后缀不能跳过 LLVM 验证。
        raw = self.root / "valid.profraw"
        raw.write_bytes(self.samples[0])
        # 保持本地源和最终输出路径独立，避免下载测试碰巧重用了已有文件。
        valid = self.root / "valid.profdata"
        subprocess.run([self.profdata, "merge", "-o", str(valid), str(raw)], check=True)
        with serve_profiles({"/valid.profdata": valid.read_bytes(),
                             "/invalid.profdata": b"not a profdata file"}) as (base, _):
            result = self.run_merge("--source-url", base + "/valid.profdata")
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assert_function_count(1)
            # 保存成功发布的索引作为比较基线，后续错误不得覆盖任何原始字节。
            original = self.output.read_bytes()
            result = self.run_merge("--source-url", base + "/invalid.profdata")
        self.assertEqual(result.returncode, 1, result.stdout)
        # 返回码正确还不够，回读输出必须与有效版本完全一致。
        self.assertEqual(self.output.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
