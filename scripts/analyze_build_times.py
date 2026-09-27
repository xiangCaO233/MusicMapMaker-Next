#!/usr/bin/env python3
"""Parse Clang -ftime-trace JSON files and print top-10 slowest compilation units.

Usage:
    python3 scripts/analyze_build_times.py [build_dir]
"""

import json
import os
import sys
from pathlib import Path


# 返回源文件与时间跟踪文件的唯一配对，供后续逐个解析。
# 编译数据库是权威映射，避免仅按文件名猜测对象与源码关系。
def find_time_traces(build_dir: Path) -> list[tuple[Path, Path]]:
    """Find all .cpp source files with corresponding .json time traces."""
    compile_commands = build_dir / "compile_commands.json"
    # 未导出编译数据库通常表示构建目录尚未配置，直接给出诊断。
    if not compile_commands.exists():
        print(f"[analyze_build_times] compile_commands.json not found in {build_dir}")
        return []

    with open(compile_commands, encoding="utf-8") as f:
        # JSON 解析失败应暴露给调用者，因为数据库损坏不是无结果状态。
        commands = json.load(f)

    # seen 按 trace 路径去重，兼容同一翻译单元出现在多个编译条目中。
    results: list[tuple[Path, Path]] = []
    seen = set()
    for entry in commands:
        # output 决定 Clang 时间文件位置，file 用于最终报告可读源码名。
        output = entry.get("output", "")
        source = entry.get("file", "")
        if not output or not source:
            # 跳过无法建立可靠配对的非标准数据库条目。
            continue
        # Clang 将 -ftime-trace 文件放在对象输出旁并替换扩展名为 .json。
        trace = Path(output).with_suffix(".json")
        if trace.exists() and str(trace) not in seen:
            seen.add(str(trace))
            results.append((Path(source), trace))
    return results


def parse_trace_duration_sec(trace_path: Path) -> float:
    """Extract total wall time in seconds from a Clang -ftime-trace JSON."""
    try:
        # 每个 trace 独立容错，单个并行编译留下的截断文件不影响汇总。
        with open(trace_path, encoding="utf-8") as f:
            data = json.load(f)
    except (json.JSONDecodeError, OSError):
        # 零值由调用方过滤，同时保留其他可解析翻译单元。
        return 0.0

    # Clang 事件时长单位为微秒，优先使用覆盖整个翻译单元的汇总事件。
    events = data.get("traceEvents", [])
    max_dur_us = 0
    for event in events:
        name = event.get("name", "")
        # "Total Source" is the most accurate single-TU wall time in newer Clang
        # Fall back to the largest "Total*" event
        if name.startswith("Total"):
            dur_us = event.get("dur", 0)
            if name == "Total Source":
                # 新版 Clang 的 Total Source 是最直接的单翻译单元墙钟时间。
                return dur_us / 1_000_000.0
            if dur_us > max_dur_us:
                # 旧版格式没有 Total Source 时保存最大的 Total 类事件。
                max_dur_us = dur_us
    return max_dur_us / 1_000_000.0


def main() -> None:
    # 可选首参数允许分析任意现有构建树，默认与项目常规布局一致。
    build_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("build")
    if not build_dir.exists():
        print(f"[analyze_build_times] directory not found: {build_dir}")
        return

    traces = find_time_traces(build_dir)
    if not traces:
        # GCC/MSVC 不生成该 JSON，空结果属于正常跨编译器行为。
        return  # No time-trace files — skip silently (GCC/MSVC build)

    # 只保留正时长记录，防止损坏文件占据排行榜位置。
    results: list[tuple[Path, float]] = []
    for source, trace in traces:
        dur = parse_trace_duration_sec(trace)
        if dur > 0:
            results.append((source, dur))

    # 降序排序后截取十项，完整数量仍显示在标题中。
    results.sort(key=lambda x: x[1], reverse=True)

    # 固定宽度边框让 CI 日志中的耗时段容易被肉眼定位。
    print(f"\n{'=' * 72}")
    print(
        f"  Compile Time Top 10  (total: {len(results)} translation units)"
    )
    print(f"{'=' * 72}")
    for i, (source, dur) in enumerate(results[:10], 1):
        # 秒数保留两位小数，兼顾短翻译单元与大型源码的可读性。
        print(f"  {i:2}. {dur:7.2f}s  {source}")
    print(f"{'=' * 72}\n")


if __name__ == "__main__":
    # 作为构建后脚本直接执行，导入时不主动扫描文件系统。
    main()
