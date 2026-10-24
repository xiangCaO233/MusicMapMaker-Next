#!/usr/bin/env python3
"""统计当前 CMake/Ninja 构建中，自维护头文件实际影响的编译单元。

读取编译器已经生成的 Ninja 依赖，不以文本 include 数量代替真实传播范围。
先完成构建，再运行本工具；修改前后必须复用相同平台、配置和目标集合。
输出包括对象文件列表，便于进一步区分真实消费者与经聚合头传入的依赖。
"""

import argparse
import json
from pathlib import Path
import subprocess


def collect_fanout(source_root: Path, build_root: Path) -> dict:
    """按当前编译数据库筛选依赖，排除切换分支后遗留在 Ninja 中的旧目标。

    同一头文件即使经多条 include 链到达同一个对象，也只统计一次。
    结果只覆盖 Modules 下的项目头，不读取或分析第三方源码。
    """
    # 使用编译器实际登记的输出目标，不扫描所有 CMakeFiles 下的历史对象。
    # 编译数据库与 Ninja 依赖都来自同一构建树，报告不会合并不同配置。
    commands = json.loads((build_root / "compile_commands.json").read_text())
    # CMake 的对象输出使用绝对路径；Ninja 的键则相对构建目录。
    # 统一成绝对路径后比较，允许用户使用仓库外的构建目录。
    active = {
        str((Path(command["directory"]) / command["output"]).resolve())
        for command in commands
    }
    deps = subprocess.check_output(
        ["ninja", "-C", str(build_root), "-t", "deps"], text=True
    )
    # 集合在解析阶段消除重复路径，不能把一条依赖的多个到达路径重复计数。
    # 对象是否活跃由当前 compile_commands 决定，文件是否还在磁盘上不作依据。
    fanout: dict[str, set[str]] = {}
    recorded: set[str] = set()
    current = None
    for line in deps.splitlines():
        # 只接受 Ninja 对象记录头；空行和诊断信息不建立新的对象归属。
        if line and not line.startswith(" "):
            current = None
            if ": #deps " in line:
                candidate = str((build_root / line.split(": #deps ", 1)[0]).resolve())
                if candidate in active:
                    current = candidate
                    recorded.add(candidate)
            continue
        if current is None or not line.startswith("    "):
            continue
        # 依赖通常为绝对路径，相对形式仍按编译器工作目录解释。
        # 只保留自维护头文件，源文件、系统头和生成文件不参与排名。
        header = (build_root / line.strip()).resolve()
        if not header.is_relative_to(source_root / "Modules"):
            continue
        if header.suffix not in (".h", ".hpp"):
            continue
        key = str(header.relative_to(source_root))
        fanout.setdefault(key, set()).add(str(Path(current).relative_to(build_root)))
    # 先按扇出降序、同数按路径排序，使两次报告具有稳定的比较顺序。
    # 每个对象列表只用于解释结果，不承诺其对象大小或编译耗时相同。
    # 编译单元数是修改头文件后的重编译范围，不能直接当成耗时百分比。
    headers = [
        {"path": path, "translation_units": len(objects), "objects": sorted(objects)}
        for path, objects in sorted(fanout.items(), key=lambda item: (-len(item[1]), item[0]))
    ]
    return {
        "source_root": str(source_root),
        "build_root": str(build_root),
        "translation_units": len(active),
        "recorded_translation_units": len(recorded),
        "headers": headers,
    }


def main() -> None:
    """生成可留作基线的 JSON，并打印限量排名，不修改源码或构建依赖。"""
    source_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=source_root / "build")
    parser.add_argument("--output", type=Path, help="可选的 JSON 报告路径")
    parser.add_argument("--limit", type=int, default=30)
    # 不自动构建或触碰头文件时间戳；统计行为本身不能引入新的构建工作。
    # 缺失编译数据库或 Ninja 失败时直接退出，不生成貌似有效的零值报告。
    args = parser.parse_args()
    report = collect_fanout(source_root, args.build_dir.resolve())
    # 同时报告已记录和活动目标数；未构建完的数据库不应冒充完整基线。
    print(f"Translation units: {report['recorded_translation_units']}/{report['translation_units']}")
    for header in report["headers"][: max(0, args.limit)]:
        print(f"{header['translation_units']:4d}  {header['path']}")
    # 不同报告应由调用者保存为不同文件，保留基线供后续批次复核。
    if args.output is not None:
        # 输出位置由调用者指定，通常使用构建树，不散落临时文件。
        args.output.resolve().write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
