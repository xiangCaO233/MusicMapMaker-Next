"""检查器的独立算术夹具，固定 VB 中点舍入及每步累加语义。"""
import tempfile
import unittest
from pathlib import Path
from ibmsc_grid_check import check, gcd


class GridCheckTest(unittest.TestCase):
    """使用手工槽位定义期望，避免复用导出器生成测试输入。"""
    def inspect(self, text, grid_limit=1.0):
        """将微型 BMS 写入独立临时文件并调用文本检查入口。"""
        # 自动清理夹具，不在用户曲库或项目资源中留下生成文件。
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'fixture.bms'
            path.write_text(text)
            return check(path, grid_limit)

    def test_exact_quarters(self):
        """四等分无需出现在高分辨率警告列表中。"""
        self.assertEqual(self.inspect('#00011:01010101\n'), [])

    def test_one_tick_is_one_in_both_columns(self):
        """一 tick 距 64 与 48 分网格的最近点都是一 tick。"""
        rows = self.inspect('#00011:00' + '01' + '00' * 190 + '\n')
        self.assertEqual([(r['grid'], r['d64'], r['d48']) for r in rows], [(192, 1, 1)])

    def test_zero_suggested_is_not_both_zero(self):
        """混合网格必须按整行求和，不能逐物件选择零距离。"""
        # 3 和 4 各自属于不同网格；混在同一行会使两列都非零。
        data = ['00'] * 192
        data[3] = data[4] = '01'
        self.assertEqual(self.inspect('#00011:' + ''.join(data))[0]['d64'], 1)
        self.assertEqual(self.inspect('#00011:' + ''.join(data))[0]['d48'], 1)

    def test_vb_rounds_each_addition(self):
        """浮点中点的累计舍入语义与最后统一舍入不同。"""
        # 0.5 与 1.5 的距离：CInt(0+0.5)=0，再 CInt(0+1.5)=2。
        data = ['00'] * 384
        data[1] = data[3] = '01'
        rows = self.inspect('#00011:' + ''.join(data))
        self.assertEqual((rows[0]['d64'], rows[0]['d48']), (2, 2))

    def test_rounded_zero_still_has_warning(self):
        """建议距离取整为零时，原工具仍可能报告高分辨率。"""
        # 两个点分别为 4.1 和 8.1 tick，48 分距离均会舍入为零。
        # 测试必须保留这条警告，不能用整数距离零判定完全通过。
        data = ['00'] * 1920
        data[41] = data[81] = '01'
        rows = self.inspect('#00011:' + ''.join(data))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]['d48'], 0)

    def test_1920_grid_detects_smaller_residual(self):
        """相同的 4.1 tick 位置在 1920 格设置下暴露额外残差。"""
        # 192 格对应阈值 1，1920 格对应阈值 0.1。
        # 检查器须复现此差异，兼容导出则须在两个设置下都无警告。
        data = ['00'] * 1920
        data[41] = '01'
        text = '#00011:' + ''.join(data)
        self.assertEqual(self.inspect(text), [])
        self.assertTrue(self.inspect(text, .1))

    def test_float_gcd_limit(self):
        """小于阈值的余数终止循环，而非继续求精确公约数。"""
        self.assertEqual(gcd(192, 3), 3)
        self.assertEqual(gcd(192, .5), 192)

    # 这里的两条数据属于不同通道，不会误合并成一个报告项。
    # 夹具不需要音频资源，因为测试对象是位置算术而非完整加载器。
    # 避免通过真实曲库的既有输出倒推期望值。
    def test_long_endpoints_and_hidden_channels(self):
        """长条和隐藏音符必须与普通键一样参与检查。"""
        text = '#00051:00' + '01' + '00' * 190 + '\n#00031:00' + '01' + '00' * 190
        self.assertEqual({r['channel'] for r in self.inspect(text)}, {'31', '51'})


if __name__ == '__main__':
    unittest.main()
