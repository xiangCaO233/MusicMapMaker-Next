"""验证原生 Linux 更新器的暂存、原子发布及失败后的文件保全。

测试直接执行生产二进制。启动前建立下载源和正式目标，执行后核对
两边的文件身份与内容。临时目录归测试独占，便于并行运行。
"""

from pathlib import Path
import os
import resource
import signal
import subprocess
import sys
import tempfile


def limit_write():
    """让跨盘复制只能写入首段，且以错误码结束。"""
    # 忽略 SIGXFSZ 后，写系统调用返回 EFBIG，由生产错误路径执行清理。
    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
    resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))


def update(updater, downloaded, target, *, fail_write=False):
    """使用刚退出的子进程 PID 执行一次生产更新。

    待更新程序必须先退出。回收一个短命子进程可提供已知退出的有效 PID，
    避免依赖固定 PID 或打断宿主的任何运行进程。
    """
    with subprocess.Popen(["/bin/true"]) as finished:
        finished.wait()
    return subprocess.run(
        [updater, downloaded, target, str(finished.pid)],
        capture_output=True,
        preexec_fn=limit_write if fail_write else None,
        # subprocess 会将 SIGXFSZ 重置为默认，故障注入显式使用忽略语义。
        timeout=10,
        check=False,
    )


def check_success(updater, download_dir, install_dir, mode):
    """验收同卷直接替换和需补权限的暂存替换。

    fixture 提供合法的 shell 程序，更新器能沿正常的安装及重启入口执行。
    检查下载源只在提交后删除，并检查成功标记指向最终路径。
    """
    target = install_dir / "MusicMapMaker-Next"
    target.write_bytes(b"old executable")
    downloaded = download_dir / "downloaded"
    # 新进程启动时立即打开标记，验证更新器准备标记和启动的先后顺序。
    observed = install_dir / "marker-observed"
    observed.unlink(missing_ok=True)
    payload = b'#!/bin/sh\ncat "${0%/*}/.mm_update_success" > "${0%/*}/marker-observed"\n'
    downloaded.write_bytes(payload)
    downloaded.chmod(mode)
    # 即使下载源没有执行位，成功路径也必须让新文件可重新启动。
    marker = install_dir / ".mm_update_success"
    marker.unlink(missing_ok=True)
    result = update(updater, downloaded, target)
    # 成功返回与文件断言共同覆盖权限设置、提交和成功标记三段流程。
    assert result.returncode == 0, result.stderr
    assert target.read_bytes() == payload
    assert target.stat().st_mode & 0o111 == 0o111
    assert marker.read_text() == str(target)
    # 子进程继承了 stderr，subprocess.run 读到 EOF 时其诊断与输出已完整。
    assert observed.read_text() == str(target)
    assert not downloaded.exists()
    assert not list(install_dir.glob(".mmm-update-*"))


def check_copy_failure(updater, download_dir, install_dir, mode):
    """短写必须保住旧程序和下载源，且不能发布成功标记。

    正式目标和下载内容各有独立填充值，防止错误覆盖后仍碰巧匹配。
    二者均超过子进程限额，确保进入复制回退后会实际截断写入。
    """
    target = install_dir / "MusicMapMaker-Next"
    old = b"x" * 4096
    target.write_bytes(old)
    downloaded = download_dir / "downloaded"
    payload = b"y" * 65536
    downloaded.write_bytes(payload)
    downloaded.chmod(mode)
    marker = install_dir / ".mm_update_success"
    marker.unlink(missing_ok=True)

    result = update(updater, downloaded, target, fail_write=True)
    # 验证原始覆盖路径绝不会在返回错误时留下截断的正式目标。
    assert result.returncode == 1 and b"Failed to replace" in result.stderr, result
    # 下载文件与正式目标分别校验；只凭退出码检测不到错误路径已破坏旧安装。
    assert target.read_bytes() == old
    assert downloaded.read_bytes() == payload
    # 后续重试要使用同一个下载文件，失败流程也不能先改变其执行位。
    assert downloaded.stat().st_mode & 0o777 == mode
    # 之前的成功标记已清理，若此处出现标记就是本次失败流程的新副作用。
    assert not marker.exists()
    assert not list(install_dir.glob(".mmm-update-*"))
    downloaded.unlink()


def check_restart_failure(updater, install_dir):
    """已替换但无法装载时，更新器应返回错误并撤销启动标记。

    使用可读、可执行但没有程序格式的文件。它能通过原子替换，
    只会在运行新版本的阶段失败。
    """
    target = install_dir / "MusicMapMaker-Next"
    downloaded = install_dir / "downloaded"
    payload = b"not an executable image"
    downloaded.write_bytes(payload)
    downloaded.chmod(0o755)
    marker = install_dir / ".mm_update_success"
    marker.unlink(missing_ok=True)

    result = update(updater, downloaded, target)
    # 非零退出码和装载错误共同区分替换阶段的错误与启动阶段的错误。
    assert (
        result.returncode == 1
        and b"Cannot execute installed program:" in result.stderr
    ), result
    # 正常安装已提交，按现有契约留给用户手工处理，但不能宣告启动成功。
    assert target.read_bytes() == payload
    assert not marker.exists()


def check_alive_parent_timeout(updater, install_dir):
    """父进程超时仍存活时中止更新，保全目标、下载和已有标记。"""
    target = install_dir / "MusicMapMaker-Next"
    downloaded = install_dir / "downloaded"
    marker = install_dir / ".mm_update_success"
    # 正式目标与下载内容不同，单独对比两者才能发现先安装后报错。
    target.write_bytes(b"old executable")
    downloaded.write_bytes(b"#!/bin/sh\nexit 0\n")
    downloaded.chmod(0o755)
    marker.write_bytes(b"previous marker")
    # 真实子进程由测试独占。测试清理负责回收它，即使断言失败也不会留下进程。
    # 六十秒寿命超过更新器的等待上限与测试超时，保证断言时子进程仍应存活。
    with subprocess.Popen(["/bin/sleep", "60"]) as parent:
        try:
            result = subprocess.run(
                [updater, downloaded, target, str(parent.pid)],
                capture_output=True, timeout=45, check=False,
            )
            # 确认旧进程没有被更新器或测试夹具提前结束。
            assert parent.poll() is None
            assert result.returncode == 1 and b"parent process exited" in result.stderr
            # 检查三个独立文件，捕捉先改名、先消费下载或先写标记的回归。
            assert target.read_bytes() == b"old executable"
            assert downloaded.read_bytes() == b"#!/bin/sh\nexit 0\n"
            assert marker.read_bytes() == b"previous marker"
        finally:
            # 终止对象来自本用例的 Popen，不使用传给生产程序的 PID 重新定位。
            parent.terminate()


def main():
    """在构建树运行同卷测试，另一个可写文件系统存在时补充跨卷。

    用 st_dev 区分挂载来源。容器中缺少独立挂载时，同卷故障注入仍
    能覆盖暂存和清理；直接 rename 的正常路径也始终运行。
    """
    updater, output_root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output_root) as install_path:
        install_dir = Path(install_path)
        # 下载不可执行时同卷也走暂存路径，可以覆盖没有 /dev/shm 的容器。
        check_copy_failure(updater, install_dir, install_dir, 0o600)
        check_success(updater, install_dir, install_dir, 0o755)
        check_success(updater, install_dir, install_dir, 0o600)
        # 故障后仍运行跨卷用例，确认错误标记的清理不影响后续成功更新。
        check_restart_failure(updater, install_dir)
        check_alive_parent_timeout(updater, install_dir)
        other_mount = Path("/dev/shm")
        if (
            os.access(other_mount, os.W_OK | os.X_OK)
            and other_mount.stat().st_dev != install_dir.stat().st_dev
        ):
            with tempfile.TemporaryDirectory(dir=other_mount) as download_path:
                # 源保留执行位可令生产代码先遇到 EXDEV，再进入暂存复制。
                check_copy_failure(updater, Path(download_path), install_dir, 0o755)
                # 在同一路径重试成功后确认下载文件被正常消费。
                check_success(updater, Path(download_path), install_dir, 0o755)
        else:
            print("未找到可写的独立 /dev/shm，已验证同卷直接替换和暂存路径。")


if __name__ == "__main__":
    main()
