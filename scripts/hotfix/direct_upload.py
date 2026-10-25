#!/usr/bin/env python3
"""Runner 使用限时许可直传已验证候选；不加载 R2 密钥或修改线上清单。

许可由发布服务器校验候选后签发，只允许特定对象及相应请求头。
文件必须匹配本地原计划，上传成功后仍需服务器完成全部正文验收。
"""
import argparse
import json
from pathlib import Path
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

from prepare_assets import digest, safe_path
from publish_assets import TransferProgress


# /// @brief 文件读包装只负责进度，不缓存或改写整个程序文件。
class ProgressReader:
    """HTTP 层按需读取，内存占用与单个读取块相当。

    回调只包含公开对象路径与字节数，不包含签名 URL 或请求头。
    文件的关闭由外层上下文管理，连接失败时也能正常释放。
    """
    def __init__(self, stream, key, size):
        """借用调用期间有效的文件句柄及本对象独立的进度计数。"""
        self.stream = stream
        self.progress = TransferProgress(key, size)

    def read(self, size=-1):
        """遵循 HTTPConnection 的文件读取契约，不提前消耗其他对象。"""
        block = self.stream.read(size)
        if block:
            self.progress(len(block))
        return block


# /// @brief 直传前验证全部许可和文件，任何非法条目都不能触发上传。
def upload(stage, grants):
    """阶段只写 R2 不可变对象，固定更新清单始终由服务器最后切换。

    许可不可用来上传任意本地文件；路径、长度、摘要均需匹配原计划。
    所有输入先校验再联网，避免前面的条目上传后才发现后续输入非法。
    """
    stage = Path(stage).resolve()
    expected = {}
    # 程序与资源计划可以共存，但不可将相同键声明为两个不同来源。
    # 此处读取原计划，不从含有签名地址的许可文件推导文件身份。
    for name in ('program-plan.json', 'plan.json'):
        if (stage / name).exists():
            for record in json.loads((stage / name).read_text())['uploads']:
                if record['key'] in expected:
                    raise ValueError('duplicate upload key')
                expected[record['key']] = record
    # 空许可表示服务器已确认所有对象都存在，仍由后续服务器步骤验收。
    # 该输入绝不能成为跳过最终 R2 正文或公开 CDN 校验的标记。
    checked, seen = [], set()
    for item in json.loads(Path(grants).read_text())['uploads']:
        key = str(safe_path(item['key']))
        source = stage / key
        # 禁止越出候选目录或通过符号链接读取其他文件。
        # 原计划和限时许可的大小、摘要必须完全一致。
        if (key in seen or key not in expected or source.is_symlink()
                or not source.resolve().is_relative_to(stage)
                or {k: item[k] for k in ('key', 'size', 'sha256')} != expected[key]):
            raise ValueError('upload grant differs from original plan')
        seen.add(key)
        if source.stat().st_size != item['size'] or digest(source) != item['sha256']:
            raise ValueError('staged checksum mismatch')
        url = urlsplit(item['url'])
        # 签名地址仅属于官方 R2 API，不允许许可把文件发往任意站点。
        # 登录信息、非默认端口、明文协议都不能进入请求。
        if (url.scheme != 'https' or not url.hostname
                or not url.hostname.endswith('.r2.cloudflarestorage.com')
                or url.username or url.password or url.port not in (None, 443)):
            raise ValueError('untrusted upload endpoint')
        checked.append((item, source))
    for item, source in checked:
        print(f'[hotfix] Runner 直传 {item["key"]} ({item["size"]} bytes)', flush=True)
        with source.open('rb') as stream:
            body = ProgressReader(stream, item['key'], item['size'])
            # 请求头与服务器签名一致；不向日志输出 URL 或限时签名。
            # 连接/写入超时会失败返回，工作流不会继续切换清单。
            request = Request(item['url'], data=body, method='PUT', headers={
                'Content-Length': str(item['size']),
                'Content-Type': 'application/octet-stream',
                'Cache-Control': 'public, max-age=31536000, immutable',
                'x-amz-meta-sha256': item['sha256']})
            with urlopen(request, timeout=120) as response:
                if response.status != 200:
                    raise ValueError('R2 upload failed')
        # 重读本地文件确保上传期间来源没有被并发构建更换。
        if digest(source) != item['sha256']:
            raise ValueError('source changed during upload')
        print(f'[hotfix] Runner 直传完成 {item["key"]}', flush=True)


if __name__ == '__main__':
    # 阶段目录和许可均来自本次 workflow，禁止隐式选择另一轮产物。
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', required=True, type=Path)
    parser.add_argument('--grants', required=True, type=Path)
    args = parser.parse_args()
    upload(args.stage, args.grants)
