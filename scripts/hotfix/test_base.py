"""用独立 Git 仓库验证 release 基线与合并契约，不改当前工作树。"""

import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from validate_base import git, validate


# /// @brief 构造最小 Git 历史，断言身份而非分支名称猜测。
class HotfixBaseTest(unittest.TestCase):
    """每个测试只创建本地仓库，不查询真实 GitHub Release。

    夹具固定作者身份，避免依赖开发者全局 Git 设置。
    第一父链错误和正常合并是不同测试，防止误把所有 merge 拒绝。
    tag 创建与后续提交明确分开，可检测 tag 身份被改写。
    """
    # /// @brief 初始化正式版本并打 tag，返回其精确提交。
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        # 夹具独立仓库避免用户分支、签名配置和已有 tag 干扰。
        self.run_git('init', '-b', 'main')
        self.run_git('config', 'user.email', 'test@example.invalid')
        self.run_git('config', 'user.name', 'Test')
        (self.root / 'CMakeLists.txt').write_text('set(APPVER "0.6.3")\n')
        self.commit('release')
        self.base = git(self.root, 'rev-parse', 'HEAD')
        self.run_git('tag', 'v0.6.3')
        # 名称符合规则，但后续测试仍可以构造错误的第一父链。
        self.run_git('checkout', '-b', 'hotfix')

    # /// @brief Git 命令按 argv 执行，输出只在失败时供 unittest 定位。
    def run_git(self, *args):
        subprocess.run(['git', '-C', str(self.root), *args], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    # /// @brief 提交夹具变更，确保第一父链具有真实对象身份。
    def commit(self, message):
        # 提交真实 Git 对象，验证器查询历史而非模拟列表。
        self.run_git('add', '.')
        self.run_git('commit', '-m', message)

    # /// @brief 第一个维护提交显式写入基线。
    def pin(self):
        # 声明必须在 tag 后首个维护提交中出现，而不是任意后续提交补写。
        (self.root / 'hotfix-base.json').write_text(json.dumps({'tag': 'v0.6.3', 'commit': self.base}))
        self.commit('declare release base')

    # /// @brief release 起点之后的普通提交和普通功能合并均应允许。
    def test_merge_allowed(self):
        # 强制 merge commit，不能靠快进通过掩盖第二父链场景。
        self.pin()
        self.run_git('checkout', '-b', 'feature')
        (self.root / 'feature.txt').write_text('change')
        self.commit('feature')
        # 功能合并不得改变固定基线和第一维护提交的声明。
        self.run_git('checkout', 'hotfix')
        self.run_git('merge', '--no-ff', 'feature', '-m', 'merge feature')
        self.assertEqual(validate(self.root)['commit'], self.base)

    # /// @brief 在开发提交之后补写基线文件仍应被拒绝。
    def test_development_fork_rejected(self):
        # 版本文本仍正确；只检查 APPVER 或 ancestor 会错误放行。
        (self.root / 'development.txt').write_text('before pin')
        self.commit('development')
        self.pin()
        with self.assertRaisesRegex(ValueError, 'first hotfix commit'):
            validate(self.root)

    # /// @brief 同名 tag 被移动时，固定提交校验必须阻止构建。
    def test_moved_tag_rejected(self):
        # pin 不变而同名 tag 移动，模拟远端发布身份被重写。
        self.pin()
        self.run_git('tag', '-f', 'v0.6.3')
        with self.assertRaisesRegex(ValueError, 'pinned commit'):
            validate(self.root)

    # /// @brief 更高正式发布混入维护主线后必须迁移基线。
    def test_new_release_rejected(self):
        # 即使源码未变，更高正式 tag 已是祖先也不能沿用旧基线。
        self.pin()
        self.run_git('tag', 'v0.6.4')
        with self.assertRaisesRegex(ValueError, 'newer release tag'):
            validate(self.root)


if __name__ == '__main__':
    unittest.main()
