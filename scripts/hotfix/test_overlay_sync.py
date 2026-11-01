#!/usr/bin/env python3
"""用隔离 Git 远端验证 HTTPS 同步、原有凭据配置及重复执行行为。"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


# /// @brief 实际执行同步脚本及 Git 克隆、提交和推送，不接触线上仓库。
class OverlaySyncTest(unittest.TestCase):
    """将固定 HTTPS 地址映射到临时裸仓，GitHub 元数据使用小型替身。

    这样既覆盖脚本的真实 shell 环境，又无需部署凭据或修改系统 Git 配置。
    原有 URL 映射与 credential helper 必须同时保留，不能被同步器覆盖。
    """

    # /// @brief 为每个测试建立独立远端、脚本替身与认证配置。
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.script = Path(__file__).resolve().parents[1] / 'ci/update-gentoo-overlay.sh'
        self.git = shutil.which('git')
        # 禁用宿主的全局和系统配置，避免测试读到用户的私有 URL 重写。
        # 测试提交使用固定本地身份，不复用开发者的真实姓名或邮箱。
        self.env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull,
                        GIT_CONFIG_NOSYSTEM='1', GIT_AUTHOR_NAME='Test',
                        GIT_AUTHOR_EMAIL='test@example.invalid',
                        GIT_COMMITTER_NAME='Test', GIT_COMMITTER_EMAIL='test@example.invalid',
                        RUNNER_TEMP=str(self.root))
        # 夹具身份不继承真实 Actions 环境，也不依赖当前用户的部署密钥。
        for key in ('GIT_CONFIG_COUNT', 'GIT_SSH_COMMAND', 'MMM_GENTOO_OVERLAY_SSH_KEY'):
            self.env.pop(key, None)
        seed = self.root / 'seed'
        seed.mkdir()
        # main 必须已存在，才能覆盖生产同步器的单分支浅克隆入口。
        # 后面的裸仓既接收首次 push，也接收同版本幂等重试的检查。
        self.run_git('-C', str(seed), 'init', '-b', 'main')
        (seed / 'scripts').mkdir()
        # 生成器只创建可通过语法检查的 ebuild，产品配方本身不属于本测试职责。
        (seed / 'scripts/update-source-release.py').write_text(
            "from pathlib import Path\n"
            "p = Path(__file__).resolve().parents[1] / 'app-editors/musicmapmaker-next/musicmapmaker-next-0.6.4.ebuild'\n"
            "p.parent.mkdir(parents=True, exist_ok=True)\n"
            "p.write_text('EAPI=8\\n')\n")
        self.run_git('-C', str(seed), 'add', '.')
        self.run_git('-C', str(seed), 'commit', '-m', 'fixture')
        self.remote = self.root / 'overlay.git'
        # 发布目标与工作副本分离，防止只在本地提交却没有实际写入远端。
        self.run_git('clone', '--bare', str(seed), str(self.remote))
        bindir = self.root / 'bin'
        bindir.mkdir()
        # 只替换只读 API；提交、分支选择和非强制推送仍使用真实 Git。
        gh = bindir / 'gh'
        gh.write_text("#!/bin/sh\ncase \"$2\" in\n"
                      "  */commits/*) printf '%040d\\n' 1 ;;\n"
                      "  *) printf '%s\\n' \"${FIXTURE_TAG:-v0.6.4}\" ;;\n"
                      "esac\n")
        gh.chmod(0o700)
        # 仅本次子进程看到 API 替身，原进程及后续测试不受 PATH 修改影响。
        self.env['PATH'] = str(bindir) + os.pathsep + self.env['PATH']
        # 此映射模拟 Runner 已有 Git 配置；脚本若重设配置计数将无法克隆。
        # 映射只匹配一个完整 overlay URL，其他地址不会意外落到夹具远端。
        # 第二项为空 helper，确保测试不调用宿主的外部密码管理器。
        self.env.update(GIT_CONFIG_COUNT='2',
                        GIT_CONFIG_KEY_0='url.' + self.remote.as_uri() + '.insteadOf',
                        GIT_CONFIG_VALUE_0='https://github.com/xiangCaO233/mmm-gentoo-overlay.git',
                        GIT_CONFIG_KEY_1='credential.helper', GIT_CONFIG_VALUE_1='')

    # /// @brief 所有 Git 操作使用临时配置，失败直接暴露实际错误输出。
    def run_git(self, *args):
        # stderr 合入测试结果，失败时保留 Git 的真实诊断而不吞掉返回值。
        return subprocess.check_output([self.git, *args], env=self.env,
                                       stderr=subprocess.STDOUT, text=True).strip()

    # /// @brief 同步脚本仍从项目根执行，生成器工作目录由脚本自身约束。
    def sync(self):
        # 普通正式版本不需要构造 DEB，但仍经过 Release 身份和远端 tag 检查。
        # 同步器内部的临时目录会自行清理，整个夹具另外由 addCleanup 回收。
        return subprocess.run(['bash', str(self.script), 'v0.6.4'],
                              cwd=self.script.parents[2], env=self.env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    # /// @brief HTTPS 冷启动能完成真实推送；已有配方重试不制造重复提交。
    def test_https_push_and_idempotent_retry(self):
        before = self.run_git('--git-dir', str(self.remote), 'rev-parse', 'main')
        first = self.sync()
        self.assertEqual(first.returncode, 0, first.stdout)
        after = self.run_git('--git-dir', str(self.remote), 'rev-parse', 'main')
        self.assertNotEqual(before, after)
        # 远端必须确实包含新包，不能仅检查脚本输出的成功消息。
        recipe = 'main:app-editors/musicmapmaker-next/musicmapmaker-next-0.6.4.ebuild'
        self.assertEqual(self.run_git('--git-dir', str(self.remote), 'show', recipe), 'EAPI=8')
        # 复用同一个远端再执行，确认恢复行为不会为相同字节创建新提交。
        second = self.sync()
        self.assertEqual(second.returncode, 0, second.stdout)
        self.assertEqual(after, self.run_git('--git-dir', str(self.remote), 'rev-parse', 'main'))

    # /// @brief 工作流 token 仅留给主仓 API，不能覆盖跨仓 Git 的登录账号。
    def test_ci_token_is_scoped_to_api(self):
        # 使用无权限的占位符模拟 Actions 自动注入值，不读取任何真实凭据。
        # 两类公共 token 和两类 Enterprise token 都必须从 Git 环境移除。
        for name in ('GH_TOKEN', 'GITHUB_TOKEN', 'GH_ENTERPRISE_TOKEN', 'GITHUB_ENTERPRISE_TOKEN'):
            self.env[name] = 'workflow-token-fixture'
        bindir = self.root / 'bin'
        gh = bindir / 'gh'
        # API 替身要求主 token 仍存在，禁止通过全局 unset 顺便破坏主仓 API。
        # 插入到元数据分支之前，保证每次查询都验证认证作用域。
        gh.write_text(gh.read_text().replace('#!/bin/sh\n',
            '#!/bin/sh\n[ "$GH_TOKEN" = workflow-token-fixture ] || exit 92\n'))
        wrapper = bindir / 'git'
        # Git 替身只检查进程环境，然后交给真实 Git 执行克隆、提交和推送。
        # 相比检查脚本文本，这能捕获某条 Git 调用漏用隔离入口的回归。
        # 远端仍为临时裸仓，不访问真实 GitHub 或 Docker 的 credential store。
        wrapper.write_text('#!/bin/sh\n'
            '[ -z "${GH_TOKEN+x}${GITHUB_TOKEN+x}${GH_ENTERPRISE_TOKEN+x}${GITHUB_ENTERPRISE_TOKEN+x}" ] || exit 91\n'
            'exec "$FIXTURE_REAL_GIT" "$@"\n')
        wrapper.chmod(0o700)
        self.env['FIXTURE_REAL_GIT'] = self.git
        # 脚本必须完整成功，证明 API 与跨仓 Git 可以使用不同凭据来源。
        # 若在父 shell 清空 token，API 替身会先于远端写入报错。
        result = self.sync()
        self.assertEqual(result.returncode, 0, result.stdout)

    # /// @brief 已过期的发布身份在任何跨仓写入前被拒绝。
    def test_stale_release_does_not_push(self):
        before = self.run_git('--git-dir', str(self.remote), 'rev-parse', 'main')
        self.env['FIXTURE_TAG'] = 'v0.6.5'
        # 模拟主仓已发布更高版本；旧同步任务不能回退 overlay 的发布身份。
        result = self.sync()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(before, self.run_git('--git-dir', str(self.remote), 'rev-parse', 'main'))


if __name__ == '__main__':
    # 复用 hotfix preflight 的 unittest discover，不另建访问真实网络的验证入口。
    unittest.main()
