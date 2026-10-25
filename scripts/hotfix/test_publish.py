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


class DirectUploadTest(unittest.TestCase):
    """本地真实候选配合受控 HTTP 响应验证 Runner 的直传边界。

    复用原对象夹具而不重复执行其测试，签名与上传各有独立断言。
    限时签名使用测试占位符，日志断言要求它始终不被输出。
    """
    def setUp(self):
        """原计划独立于许可保存，避免两边被同一错误输入同时污染。"""
        PublishRecordTest.setUp(self)
        import json
        # 许可文件独立命名，不混入候选资源或公开程序清单。
        # 实际签名只存在私有临时文件，测试值始终是不可用占位符。
        self.grants = self.stage / 'private-grants.json'
        self.url = 'https://test.r2.cloudflarestorage.com/object?signature=private-test'
        self.grant = dict(**self.record, url=self.url)
        (self.stage / 'program-plan.json').write_text(json.dumps({'uploads': [self.record]}))
        self.write_grants()

    # 对象元数据准备与发布路径共用，测试方法本身保持相互独立。
    existing = PublishRecordTest.existing

    def write_grants(self, items=None):
        """只更改许可输入，原计划与源文件继续保持独立。"""
        import json
        # 此 helper 只负责输入，不替生产上传器修正路径或摘要。
        # 可以故意构造重复条目，验证其在任何网络写入之前被拒绝。
        self.grants.write_text(json.dumps({'uploads': [self.grant] if items is None else items}))

    def test_direct_streaming(self):
        """HTTP 请求读取真实候选字节，签名地址不进入进度日志。"""
        from direct_upload import upload
        # side_effect 消费真实的文件读包装，验证请求体及签名元数据一致。
        # 响应仅代表上传成功，固定清单不属于 Runner 的写入范围。
        def send(request, timeout):
            self.assertEqual(request.data.read(), self.data)
            self.assertEqual(request.get_header('X-amz-meta-sha256'), self.record['sha256'])
            self.assertEqual(request.get_header('Content-length'), str(len(self.data)))
            response = MagicMock()
            response.__enter__.return_value.status = 200
            return response
        with patch('direct_upload.urlopen', side_effect=send) as opener, contextlib.redirect_stdout(io.StringIO()) as output:
            upload(self.stage, self.grants)
        opener.assert_called_once()
        self.assertNotIn('signature=', output.getvalue())
        self.assertFalse((self.stage / 'program.json').exists())

    def test_direct_corrupt_source(self):
        """来源正文被修改时，任何 HTTP 请求都不能发生。"""
        from direct_upload import upload
        # 保持许可和原计划不变，只改变文件正文，避免同源错误抵消。
        # 通过 mock 的零调用证明失败不只是事后检测。
        (self.stage / self.key).write_bytes(b'changed')
        with patch('direct_upload.urlopen') as opener, self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            upload(self.stage, self.grants)
        opener.assert_not_called()

    def test_direct_bad_endpoint(self):
        """许可不能把候选程序发送到第三方任意域名。"""
        from direct_upload import upload
        # 其他字段仍然合法，失败必须来自网络目标检查。
        # 即使远端提供 URL，也不自动信任其主机或明文协议。
        self.grant['url'] = 'https://untrusted.example/object'
        self.write_grants()
        with patch('direct_upload.urlopen') as opener, self.assertRaisesRegex(ValueError, 'untrusted upload endpoint'):
            upload(self.stage, self.grants)
        opener.assert_not_called()

    def test_direct_bad_plan(self):
        """许可的摘要与本地原计划不同，即使文件存在也必须拒绝。"""
        from direct_upload import upload
        # 限时许可不能重新定义来源文件身份，原计划才是本地依据。
        # 若实现仅校验许可与文件，此处仍必须在上传前失败。
        self.grant['sha256'] = '0' * 64
        self.write_grants()
        with patch('direct_upload.urlopen') as opener, self.assertRaisesRegex(ValueError, 'differs from original plan'):
            upload(self.stage, self.grants)
        opener.assert_not_called()

    def test_direct_duplicate_grant(self):
        """全部条目预检防止重复许可造成一部分上传之后才报错。"""
        from direct_upload import upload
        # 首条合法也不能先上传，完整预检应发现后续重复项。
        # 否则弱网络重试可能让同一对象被不必要地反复覆盖。
        self.write_grants([self.grant, self.grant])
        with patch('direct_upload.urlopen') as opener, self.assertRaisesRegex(ValueError, 'differs from original plan'):
            upload(self.stage, self.grants)
        opener.assert_not_called()

    def test_sign_only_missing(self):
        """签名限定对象身份和三十分钟有效期，不写入程序清单。"""
        from publish_assets import sign_uploads
        # 服务器候选校验作为独立前置门槛，SDK 身份只由服务器加载。
        # 测试分别断言参数闭包，避免给未知长度或其他桶签发许可。
        self.client.generate_presigned_url.return_value = self.url
        with patch('publish_assets.validate_bundle', return_value=[({}, {'uploads': [self.record]}, 'check')]), patch('publish_assets.load_backend', return_value=(self.client, 'bucket', 'domain', None)):
            result = sign_uploads(self.stage, self.root)
        self.assertEqual(result, {'uploads': [self.grant]})
        call = self.client.generate_presigned_url.call_args
        # 有效期固定，防止恢复文件意外变成长期访问凭据。
        self.assertEqual(call.kwargs['ExpiresIn'], 1800)
        self.assertEqual(call.kwargs['HttpMethod'], 'PUT')
        self.assertEqual(call.kwargs['Params']['ContentLength'], len(self.data))
        self.assertEqual(call.kwargs['Params']['Metadata']['sha256'], self.record['sha256'])

    def test_sign_reuses_existing(self):
        """已有同摘要对象无需签发覆盖许可，最终发布仍需正文验收。"""
        from publish_assets import sign_uploads
        self.existing()
        with patch('publish_assets.validate_bundle', return_value=[({}, {'uploads': [self.record]}, 'check')]), patch('publish_assets.load_backend', return_value=(self.client, 'bucket', 'domain', None)):
            self.assertEqual(sign_uploads(self.stage, self.root), {'uploads': []})
        self.client.generate_presigned_url.assert_not_called()

    def test_sign_rejects_existing_conflict(self):
        """同名对象的内容不同，不能通过直传入口获得覆盖能力。"""
        from publish_assets import sign_uploads
        self.existing()
        self.client.head_object.return_value['ContentLength'] += 1
        with patch('publish_assets.validate_bundle', return_value=[({}, {'uploads': [self.record]}, 'check')]), patch('publish_assets.load_backend', return_value=(self.client, 'bucket', 'domain', None)), self.assertRaisesRegex(ValueError, 'immutable object conflict'):
            sign_uploads(self.stage, self.root)
        self.client.generate_presigned_url.assert_not_called()


if __name__ == '__main__':
    unittest.main()
