#!/usr/bin/env python3
"""复现 iBMSC MyO2GridCheck（BMSE 端点模式），只读检查 BMS 文本。

参考 aqtq314/iBMSC 2d43b6fe84463fd911554b1518ac0e58983399b4：
Form1.vb OpenBMS、GCD、MyO2GridCheck。默认 BMSGridLimit=1。
注意：这不是毫秒往返误差；原算法在变长小节仍减 measure*192，故保留此行为。
D48/D64 每次累加后进行 VB CInt 的偶数舍入，不能最后才 round(sum)。
"""
import argparse
import bisect
import json
import math
import re
from collections import defaultdict
from pathlib import Path

# 顺序之外的通道集合与 MyO2GridCheck 的八侧键位枚举一致。
# 其中包含隐藏键和长条端点，不能只扫描普通 1x/2x 通道。
CHANNELS = {'01', '03', '04', '06', '07', '08', '09'} | {
    f'{side}{key}' for side in range(1, 9) for key in (6, 1, 2, 3, 4, 5, 8, 9)
}
ROW = re.compile(r'^#(\d{3})([0-9A-Fa-f]{2}):(.*)$')


def gcd(a, b, limit=1.0):
    """与 VB 浮点欧几里得循环及可配置终止阈值一致。"""
    # 这里必须保留浮点余数，转成整数会改变原工具的筛选结果。
    high, low = max(a, b), min(a, b)
    while low >= limit:
        high, low = low, high - math.floor(high / low) * low
    return high


def check(path, grid_limit=1.0):
    """返回原工具会显示的每个小节／通道行，不筛掉非零偏移。"""
    # 标准 BMS 最多三位小节号，未声明的长度均为四拍。
    # 仅复现网格算术，不解析 WAV 资源，也不执行 RANDOM/IF。
    # 含条件分支的原文件结果只供诊断，生产读取失败仍须单独统计。
    # 检查器不对损坏谱面补数据，文本错误由批量入口记录。
    # 网格距离单位为 1/48 拍，而非毫秒。
    # 返回列表保持稳定排序，方便逐文件比较报告。
    lengths = [192.0] * 1000
    rows = []
    # 只解析 ASCII 协议字段；CP932 标题不会影响通道位置。
    for line in Path(path).read_bytes().decode('ascii', errors='replace').splitlines():
        match = ROW.match(line.strip())
        if not match:
            continue
        measure, channel, data = int(match[1]), match[2].upper(), match[3].strip()
        if channel == '02':
            lengths[measure] = float(data) * 192.0
        elif channel in CHANNELS:
            rows.append((measure, channel, data))
    # 先解析所有变长小节，再累积绝对位置，避免依赖文本行顺序。
    bottoms = [0.0]
    for length in lengths:
        bottoms.append(bottoms[-1] + length)
    # 同小节同通道的重复行合并参与统计，BGM 复音也逐个计数。
    groups = defaultdict(list)
    for measure, channel, data in rows:
        # 按原文本的两字符槽数计算分母，不先约分以免改变浮点误差。
        count = len(data) / 2
        for index in range(len(data) // 2):
            if data[index * 2:index * 2 + 2] == '00':
                continue
            position = bottoms[measure] + lengths[measure] * index / count
            # 位于边界的事件属于下一小节，对应原编辑器的定位行为。
            actual_measure = bisect.bisect_right(bottoms, position) - 1
            # 原算法在变长小节也减固定 192，而非真实小节底部。
            # 这是兼容复现，不在检查工具中悄悄修正其历史行为。
            relative = position - actual_measure * 192
            # 原工具跳过零点，但把负位置夹到零后参与后续计算。
            if abs(relative) > 0:
                groups[actual_measure, channel].append(max(0.0, relative))
    result = []
    for (measure, channel), positions in sorted(groups.items()):
        common, d64, d48 = 192.0, 0, 0
        # VB 的 Integer 累加每次取整，事件顺序会影响结果。
        for position in sorted(positions):
            common = gcd(common, position, grid_limit)
            d64 = round(d64 + abs(position - round(position / 3) * 3))
            d48 = round(d48 + abs(position - round(position / 4) * 4))
        # 只有低公约数行显示于原工具；建议相等时选 48 分。
        # 因而没有报告行并不意味着谱面没有事件。
        if common < 3:
            result.append(dict(measure=measure, channel=channel,
                               grid=round(192 / common), d64=d64, d48=d48,
                               suggested=64 if d48 > d64 else 48))
    return result


def main():
    """只读检查指定文件，原工具出现警告行时返回失败状态。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', nargs='+', type=Path)
    parser.add_argument('--grid-limit', type=float, default=1.0)
    args = parser.parse_args()
    if not math.isfinite(args.grid_limit) or args.grid_limit <= 0:
        parser.error('grid-limit must be positive and finite')
    bad = False
    for path in args.files:
        rows = check(path, args.grid_limit)
        # 两列是互斥候选网格，要求建议列为零，不要求两列同时零。
        nonzero = sum(min(r['d64'], r['d48']) > 0 for r in rows)
        # 距离列的整数零不能消除高分辨率警告，严格通过必须没有报告行。
        bad |= bool(rows)
        # 输出原始两列及建议，让调用者能自行复核判定依据。
        print(json.dumps(dict(path=str(path), rows=rows,
                              grid_warning_rows=len(rows), nonzero_suggested_rows=nonzero), ensure_ascii=False))
    return int(bad)


if __name__ == '__main__':
    raise SystemExit(main())
