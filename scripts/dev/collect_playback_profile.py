#!/usr/bin/env python3
"""采集 Linux 播放稳态窗口。先开启渲染性能日志，将程序输出重定向到 --log。

在目标谱面进入待测状态后执行本工具。--pid 指向编辑器本身，--label 记录
谱面、构建、限帧和播放区间。--perf 保留逻辑线程的 CPU 周期采样供 perf report。
采集器只读取日志和内核计数，不向编辑器发送命令。
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import time

# 终端日志带颜色，重定向和关闭颜色的两种模式应解析成同一份结果。
ANSI = re.compile(r"\x1b\[[0-9;]*m")
LOGIC = re.compile(r"LogicProfile tid=(\d+) seconds=([\d.]+) loops=(\d+) updates=(\d+) samples=(\d+) sample_ns=(\d+)")
RENDER = re.compile(r"RenderProfile ([\d.]+)s frames=(\d+) fps=([\d.]+) total\(avg/max\)=([\d.]+)/([\d.]+)ms")


# 推荐流程（Linux，先在编辑器设置中启用渲染性能日志）：
#   MusicMapMaker-Next chart.mmm > /tmp/mmm.log 2>&1
# 定位同一播放位置，等待资源加载和音频预读结束后，在另一终端采集：
#   python3 collect_playback_profile.py --pid PID --log /tmp/mmm.log \
#       --label before-unlimited --output /tmp/before.json --seconds 8 --perf
# 可用 perf report -i /tmp/before.perf 分析调用栈，保留与可执行文件匹配的符号。
# --trace-futex 使用系统 ptrace 权限。attach 被拒绝时工具会报告失败；
# 普通的 CPU/调度统计不需要给编辑器附加调试器。
# 不指定 --gpu-busy 时该序列留空，便于在没有对应 sysfs 节点时采集其余指标。
# 每轮指定唯一输出名并保留原日志，以便回查加载失败或意外触发的资源扫描。
# 对照宜交替运行两个版本，使用相同编译选项、设备、配置和谱面区段。
# 限帧时循环频率受截止时间控制，此时要比较相同工作量下的 CPU 占用。
# 不限帧时线程可持续占满单核，此时比较每秒真实调用数更有意义。
def scheduler(task):
    """读取同一线程的 CPU 执行时间与在运行队列等待 CPU 的时间，单位均为 ns。"""
    return tuple(map(int, (task / "schedstat").read_text().split()[:2]))


def decode(lines, tid):
    """分别丢弃逻辑和渲染首个跨边界窗口，只汇总采集开始以后完整的窗口。"""
    # 逻辑窗口约半秒，渲染窗口约两秒，它们有独立的起止点。
    # 时间分母取日志实际长度，不用窗口数量乘以名义时长。
    logic, render = [], []
    p99 = None
    for raw in lines:
        line = ANSI.sub("", raw)
        match = LOGIC.search(line)
        if match and int(match[1]) == tid:
            logic.append(dict(zip(("seconds", "loops", "updates", "samples", "sample_ns"),
                                  (float(match[2]), *map(int, match.groups()[2:])))))
        if "FrameInterval p99_upper_ms=" in line:
            # 此值是单个窗口最近秩百分位所在桶的上界，不能跨窗口取平均充当全程 P99。
            # -1 表示落入 >=100 ms 的溢出桶，避免给未知长尾捏造一个上界。
            p99 = float(line.rsplit("=", 1)[1])
        match = RENDER.search(line)
        if match:
            render.append(dict(seconds=float(match[1]), frames=int(match[2]), fps=float(match[3]),
                               avg_ms=float(match[4]), max_ms=float(match[5]), p99_upper_ms=p99))
            p99 = None
        if render and "RenderStages avg/max ms:" in line:
            # 同一渲染线程按序发布两行细项，逻辑线程日志可能穿插在其中。
            stages = render[-1].setdefault("stages_avg_max_ms", {})
            for name, avg, peak in re.findall(r"(\w+) ([\d.]+)/([\d.]+)", line):
                stages[name] = [float(avg), float(peak)]
    # 最后一组如果只到达标题或首行阶段，同样不能作为完整帧窗口。
    return logic[1:], [r for r in render[1:] if "platform" in r.get("stages_avg_max_ms", {})]


def main():
    """保存窗口原始数据及按实际时间加权的指标，防止把各窗口的速率简单平均。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--gpu-busy", type=Path, help="目标 GPU 的 gpu_busy_percent 节点")
    # 系统调用追踪会扰动线程调度，与周期采样分开跑同一谱面区间。
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--trace-futex", action="store_true", help="另行使用 strace 记录逻辑线程的 futex 系统调用")
    modes.add_argument("--perf", action="store_true", help="附加 perf 周期采样")
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds < 5:
        parser.error("至少采集 5 秒，以覆盖完整的渲染窗口")
    # 相对路径也可用；提前验证父目录，避免采完一整个区间才发现目标无效。
    if not args.output.parent.is_dir():
        parser.error("输出目录不存在")
    proc = Path("/proc") / str(args.pid)
    with args.log.open("rb") as log:
        # 从末尾查找当前实例已发布的线程号，禁止通过最忙线程猜测目标。
        log.seek(0, os.SEEK_END)
        log.seek(max(0, log.tell() - 262144))
        tids = list(LOGIC.finditer(log.read().decode(errors="replace")))
        if not tids:
            parser.error("日志中缺少 LogicProfile，请开启渲染性能日志并等待半秒")
        tid = int(tids[-1][1])
        task = proc / "task" / str(tid)
        # 每个文件独占一个采集区间，避免混用上次运行遗留的采样。
        raw_perf = args.output.with_suffix(".perf")
        raw_futex = args.output.with_suffix(".futex")
        if (args.output.exists() or (args.perf and (raw_perf.exists() or raw_perf == args.output))
                or (args.trace_futex and (raw_futex.exists() or raw_futex == args.output))):
            parser.error("输出已存在，请更换文件名")
        executable = os.readlink(proc / "exe")
        # schedstat 直接报告该线程在各 CPU 上累计的运行时间，不受迁核影响。
        # 运行队列延迟只包含可运行但未获分配的时间，不包含互斥锁和限帧休眠。
        # 两项都在读钟边界取差，CPU 百分比以单个逻辑核为 100%。
        begin_cpu, begin_wait = scheduler(task)
        start = time.monotonic()
        profiler = None
        tracer = None
        gpu = []
        waits = {}
        try:
            if args.perf:
                profiler = subprocess.Popen(["perf", "record", "-q", "-F", "399", "-g", "-t",
                                             str(tid), "-o", str(raw_perf), "--", "sleep", str(args.seconds)])
            if args.trace_futex:
                # 附加到一个 TID，不使用 -f，避免音频和渲染线程混入同一统计。
                tracer = subprocess.Popen(["strace", "-qq", "-ttt", "-T", "-e", "trace=futex",
                                           "-o", str(raw_futex), "-p", str(tid)])
            while (left := start + args.seconds - time.monotonic()) > 0:
                # 显式指定对应显卡；多 GPU 主机不能默认认为 card0 在渲染。
                # 保留每次百分比读数，设备级利用率也可能包含其他进程的负载。
                if args.gpu_busy:
                    gpu.append(int(args.gpu_busy.read_text()))
                # wchan 是离散观测；futex 样本可提示阻塞，不能换算成精确锁等待时长。
                reason = (task / "wchan").read_text().strip()
                waits[reason] = waits.get(reason, 0) + 1
                time.sleep(min(0.1, left))
            elapsed = time.monotonic() - start
            end_cpu, end_wait = scheduler(task)
            logic, render = decode(log.read().decode(errors="replace").splitlines(), tid)
        finally:
            if tracer:
                # 中断追踪器让它从目标线程分离，编辑器继续正常播放。
                tracer.send_signal(signal.SIGINT)
                if tracer.wait() not in (0, -signal.SIGINT):
                    raise SystemExit("strace 附加或采集失败")
            if profiler:
                # perf 的采样命令自身有截止时间，等待收尾以确保数据文件完整。
                if profiler.wait() != 0:
                    raise SystemExit("perf 采样失败")
    if not logic or not render:
        parser.error("未收到完整统计窗口，请核对日志来源、缓冲及渲染性能开关")
    # 多会话模式下，一个循环可以执行多个 update；分别保留循环数和调用数。
    # 抽样耗时以纳秒表示，使用总耗时除以样本数，避免小窗口获得过高权重。
    # 对于没有活动会话的区间，保留零调用数；不存在的均值输出为 null。
    # 保留每个窗口的更新数量，避免多个画布的平均值掩盖某轮突发更新。
    totals = {k: sum(row[k] for row in logic) for k in logic[0]}
    # 以下调度指标覆盖整个采集区间，日志指标只覆盖完全落在其中的窗口。
    # 两者的边界略有差异，不能将 CPU 百分比直接除以某个局部窗口的调用数。
    result = dict(label=args.label, executable=executable, pid=args.pid, tid=tid, elapsed_s=elapsed,
                  loops_s=totals["loops"] / totals["seconds"], updates_s=totals["updates"] / totals["seconds"],
                  sampled_update_ns=totals["sample_ns"] / totals["samples"] if totals["samples"] else None,
                  logic_cpu_pct=(end_cpu-begin_cpu) / elapsed / 1e7, runqueue_wait_ms=(end_wait-begin_wait) / 1e6,
                  wchan_samples=waits, gpu_busy_pct=gpu, logic_windows=logic, render_windows=render)
    # 原始 trace 文件保留系统调用参数、返回码和耗时，可分辨唤醒与竞争失败。
    # 这里只累计已完成的 WAIT 调用；EAGAIN 表示进入内核时条件已变，无需睡眠。
    # 返回超时或被信号中断也消耗墙钟时间，不应只统计成功获得唤醒的调用。
    # 该值包含进入内核的开销，并不能定位到具体 C++ 互斥锁。
    if args.trace_futex:
        durations = [float(m[1]) for line in raw_futex.read_text().splitlines()
                     if "FUTEX_WAIT" in line and "EAGAIN" not in line
                     and (m := re.search(r"<([0-9.]+)>$", line))]
        result.update(futex_wait_calls=len(durations), futex_wait_ms=sum(durations) * 1000)
    # 包含采集器的运行方式，后续分析可区分探针开销与业务负载。
    result.update(kernel=os.uname().release, perf_file=str(raw_perf) if args.perf else None,
                  futex_file=str(raw_futex) if args.trace_futex else None,
                  gpu_node=str(args.gpu_busy) if args.gpu_busy else None)
    with args.output.open("x") as output:
        json.dump(result, output, indent=2, ensure_ascii=False)
        output.write("\n")


if __name__ == "__main__":
    main()
