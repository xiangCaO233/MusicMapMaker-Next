#!/usr/bin/env python3
"""用生产入口遍历指定曲库；原谱只读，逐文件保存机器可读覆盖报告。"""
import argparse
import concurrent.futures
import json
import os
import subprocess
from pathlib import Path
from ibmsc_grid_check import check


# 每个任务使用独立的输出编号，不依赖曲名作为文件名。
# 相同曲名及不同字符编码不会因此覆盖彼此。
def run_one(job, args):
    """读取一个源谱并通过生产 writer 导出，返回检查数据与失败状态。"""
    index, source = job
    # PMS 必须保留九键通道语义，其余标准后缀统一写 BMS。
    suffix = '.pms' if source.suffix.lower() == '.pms' else '.bms'
    output = args.output / f'{index:06d}{suffix}'
    log = args.output / f'{index:06d}.log'
    row = dict(source=str(source), output=str(output))
    # 输入也检测，避免把原谱已有高分辨率当作 writer 新增缺陷。
    try:
        if source.suffix.lower() != '.osu':
            before = check(source)
            row['input_rows'] = len(before)
            row['input_nonzero'] = sum(min(r['d64'], r['d48']) > 0 for r in before)
        # stdout 和 stderr 合流，便于失败时保留生产入口的原始诊断。
        # 各进程隔离配置目录，避免并发测试抢写客户端配置。
        with log.open('w') as stream:
            proc = subprocess.run([str(args.suite), 'BmsFormatTest',
                                   '--export-myo2' if args.myo2 else '--export', str(source), str(output)],
                                  stdout=stream, stderr=subprocess.STDOUT,
                                  timeout=args.timeout, env={**os.environ,
                                  'MMM_CONFIG_ROOT': str(args.output / 'config' / str(index))})
        # 30=读取失败，31=保存拒绝，32=物件数变化，33=普通模式时间误差。
        # 非零退出始终计入失败，不能因为磁盘上存在文件就算通过。
        row['returncode'] = proc.returncode
        # 毫秒差来自重新载入后的音符及长条两端，不是网格检测结果。
        # 兼容导出的主动位移也记录，避免将零网格偏移宣称为无损。
        stats = Path(str(output) + '.json')
        if stats.exists():
            row.update(json.loads(stats.read_text()))
            stats.unlink()
        # 独立 Python 检查器只读取文本，不调用 C++ 的量化 helper。
        if output.exists():
            after = check(output)
            row['output_rows'] = after
            # iBMSC 最大格线 1920 对应 BMSGridLimit=192/1920=0.1。
            # 同一份输出检查两种设置，避免只通过默认较粗阈值。
            row['output_1920_rows'] = check(output, 0.1)
            row['output_nonzero'] = sum(min(r['d64'], r['d48']) > 0 for r in after)
            # 全量结果在 JSONL 中保留；大文件无须重复留在磁盘。
            if not args.keep_exports:
                output.unlink()
        # 成功行已记入报告；只保留需要追踪的失败进程日志。
        if proc.returncode == 0:
            log.unlink()
    # 启动失败、超时、文本错误同样进入覆盖清单，不中断剩余曲库。
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        row['error'] = str(error)
    return row


# 批量入口将枚举范围和输出位置显式交给调用方。
def main():
    """遍历支持的谱面后缀，输出逐文件 JSONL 及最终汇总。"""
    # 默认并发只限制独立导出进程，不修改项目内部线程配置。
    # 每份源谱仅有一个任务，因此无需跨线程写入同一文件。
    # 工作目录必须位于构建树等可清理位置，不应指向源谱目录。
    # 每次全量运行应使用新的输出目录，以便保留前一次失败证据。
    # 可用 --keep-exports 留下派生谱面供 iBMSC 手动交叉验证。
    # 默认删除派生产物，以免大曲库转换占用与源库相近的磁盘空间。
    # 此脚本不会复制音频，验证范围是谱面序列化及其网格。
    # 测试套件路径必须指向已经构建完成的固定版本。
    # 运行期间不要重新链接该套件，否则子进程可能无法启动。
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('roots', nargs='+', type=Path)
    parser.add_argument('--suite', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--workers', type=int, default=8)
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--keep-exports', action='store_true')
    parser.add_argument('--myo2', action='store_true')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.suite = args.suite.resolve()
    # 输出不能位于输入曲库中，防止复扫自己的产物或误删源谱。
    for root in args.roots:
        if args.output.is_relative_to(root.resolve()):
            parser.error('output must be outside input roots')
    args.output.mkdir(parents=True, exist_ok=True)
    # resolve 后去重，重叠根目录不会重复统计同一份源谱。
    # 不自动展开压缩包，也不猜测未声明的非标准扩展名。
    sources = sorted({p.resolve() for root in args.roots for p in root.rglob('*')
                      if p.is_file() and p.suffix.lower() in {'.bms', '.bme', '.bml', '.pms', '.osu'}})
    print(f'Inventoried {len(sources)} charts', flush=True)
    summary = dict(total=len(sources), completed=0, export_ok=0, failed=0,
                   input_nonzero_charts=0, output_nonzero_charts=0, output_warning_charts=0)
    # 逐条刷新便于长时间运行时检查进度；完成顺序不影响文件编号。
    # 汇总只有全部任务结束后才写出，以免把中间状态误认为最终结果。
    with (args.output / 'report.jsonl').open('w') as report, concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        jobs = [pool.submit(run_one, job, args) for job in enumerate(sources)]
        for future in concurrent.futures.as_completed(jobs):
            row = future.result()
            report.write(json.dumps(row, ensure_ascii=False) + '\n')
            report.flush()
            summary['completed'] += 1
            # 失败数与非零偏移数分别累计，两者不能相互抵消。
            summary['export_ok' if row.get('returncode') == 0 else 'failed'] += 1
            summary['input_nonzero_charts'] += row.get('input_nonzero', 0) > 0
            summary['output_nonzero_charts'] += row.get('output_nonzero', 0) > 0
            # 兼容模式要求原工具没有警告行，不能只看舍入后的建议距离。
            summary['output_warning_charts'] += bool(row.get('output_rows', []) or row.get('output_1920_rows', []))
            if summary['completed'] % 100 == 0:
                print(summary, flush=True)
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2))
    print(summary)
    # 保留失败退出码，让自动化调用方不能忽略拒绝导出的谱面。
    return int(summary['failed'] > 0 or summary['output_nonzero_charts'] > 0 or
               (args.myo2 and summary['output_warning_charts'] > 0))


if __name__ == '__main__':
    raise SystemExit(main())
