"""验证发布恢复的不可变约束、正文校验及可见进度，不访问真实网络。"""

import contextlib
import hashlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import MagicMock, patch

from publish_assets import publish_record, TransferProgress


class PublishRecordTest(unittest.TestCase):
    """用独立本地候选覆盖上传与复用两条发布路径。

    客户端只模拟 S3 交互，实际摘要由测试文件独立计算。
    失败不得留下可被本地清单引用的直连副本。
    """
    def setUp(self):
        """每个测试独享暂存目录与站点根目录，不污染其他运行。"""
        # 真实临时文件参与摘要计算，避免仅靠 mock 返回值通过。
        # 每个用例结束后由 addCleanup 回收，不修改仓库或远端文件。
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.stage = Path(self.temp.name) / 'stage'
        self.root = Path(self.temp.name) / 'root'
        self.key = 'download/hotfix/program/v0.6.4/r1/linux/program'
        self.data = b'original-validated-build'
        source = self.stage / self.key
        source.parent.mkdir(parents=True)
        source.write_bytes(self.data)
        self.record = dict(key=self.key, sha256=hashlib.sha256(self.data).hexdigest(), size=len(self.data))
        # 对象存储行为通过独立 mock 隔离，测试不读取任何凭据。
        # 上传对象名保持真实目录形状，覆盖本地镜像路径拼接。
        self.client = MagicMock()
        self.client.list_objects_v2.return_value = {}
        self.client.get_object.return_value = {'Body': io.BytesIO(self.data)}
        self.publisher = MagicMock()
        self.transfer = object()

    def publish(self):
        """捕获进度输出，校验生产入口的文件系统副作用。"""
        # 只捕获测试调用的 stdout，仍允许异常传回断言边界。
        # 日志不参与发布成功判定，文件正文与请求另有独立断言。
        with contextlib.redirect_stdout(io.StringIO()) as output:
            publish_record(self.client, 'bucket', 'cdn.example', self.stage, self.root,
                           self.record, self.publisher, self.transfer)
        return output.getvalue()

    def existing(self):
        """已有对象必须同时匹配摘要及长度，不能仅靠路径复用。"""
        # 精确键存在而非仅前缀存在，避免将相邻对象误判为已上传。
        # 元数据和长度分别填入，测试可以只篡改其中一个字段。
        self.client.list_objects_v2.return_value = {'Contents': [{'Key': self.key}]}
        self.client.head_object.return_value = {
            'Metadata': {'sha256': self.record['sha256']}, 'ContentLength': len(self.data)}

    def test_upload_and_verify(self):
        """上传配置和回调传给 SDK，公开校验完成后才建立镜像。"""
        # 回调实例和传输配置必须真正传递给 SDK 调用。
        # 仅打印一条开始日志不代表上传器实际具有可见进度。
        output = self.publish()
        args = self.client.upload_file.call_args.kwargs
        self.assertIs(args['Config'], self.transfer)
        self.assertIsInstance(args['Callback'], TransferProgress)
        self.assertEqual(args['ExtraArgs']['Metadata']['sha256'], self.record['sha256'])
        self.publisher.verify_public.assert_called_once_with('cdn.example', '/' + self.key, self.record['sha256'])
        self.assertEqual((self.root / self.key).read_bytes(), self.data)
        self.assertIn('对象完成', output)

    def test_resume_existing_object(self):
        """恢复跳过上传，但仍完整回读正文并检查公开 CDN。"""
        # 跳过上传可以节省恢复流量，但不能跳过完整性和公开入口校验。
        # 两个独立请求断言确保恢复没有退化成只检查对象元数据。
        self.existing()
        self.assertIn('复用已有对象', self.publish())
        self.client.upload_file.assert_not_called()
        self.client.get_object.assert_called_once()
        self.publisher.verify_public.assert_called_once()

    def test_immutable_conflict(self):
        """同修订不同摘要必须拒绝，不能静默覆盖原对象。"""
        # 保持对象长度正确，只破坏摘要，暴露仅检查长度的错误实现。
        # 冲突必须发生在任何上传或直连副本写入之前。
        self.existing()
        self.client.head_object.return_value['Metadata']['sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'immutable object conflict'):
            self.publish()
        self.client.upload_file.assert_not_called()
        self.assertFalse((self.root / self.key).exists())

    def test_corrupt_body(self):
        """元数据存在仍不能替代正文；损坏正文阻断本地镜像。"""
        # 元数据完全匹配，仍返回错误正文，独立验证读取路径。
        # 如果实现信任元数据而省略下载，此例就会错误地产生镜像。
        self.existing()
        self.client.get_object.return_value = {'Body': io.BytesIO(b'tampered')}
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            self.publish()
        self.publisher.verify_public.assert_not_called()
        self.assertFalse((self.root / self.key).exists())

    def test_public_failure(self):
        """公开 CDN 校验失败时不得推进到本地清单可见的状态。"""
        # R2 上传和回读成功之后，公开入口仍是独立的验收门槛。
        # CDN 错误必须传播给上层，不能在日志中吞掉后继续发布清单。
        self.publisher.verify_public.side_effect = ValueError('public checksum mismatch')
        with self.assertRaisesRegex(ValueError, 'public checksum mismatch'):
            self.publish()
        self.assertFalse((self.root / self.key).exists())

    def test_progress_retry_and_finish(self):
        """SDK 回退计数不能产生负字节，完成时立即输出进度。"""
        # 固定时间只检验完成分支，不能靠经过五秒意外触发输出。
        # 负回调模拟 SDK 撤销重试字节，不应让计数小于零。
        with patch('publish_assets.time.monotonic', return_value=0), contextlib.redirect_stdout(io.StringIO()) as output:
            progress = TransferProgress('public-key', 100)
            progress(50)
            progress(-70)
            self.assertEqual(progress.sent, 0)
            self.assertEqual(output.getvalue(), '')
            progress(100)
            self.assertIn('100/100 bytes', output.getvalue())

    def test_progress_interval(self):
        """尚未完成的对象也按真实时间输出，避免弱链路下整步静默。"""
        # 用时钟注入推进时间，测试不 sleep，也不拖慢 CI。
        # 对象仅传送一成即可看到进度，覆盖真实上传迟缓的情况。
        with patch('publish_assets.time.monotonic', side_effect=[0, 6]), contextlib.redirect_stdout(io.StringIO()) as output:
            TransferProgress('public-key', 100)(10)
            self.assertIn('10/100 bytes', output.getvalue())


if __name__ == '__main__':
    unittest.main()
