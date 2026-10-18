#!/usr/bin/env python3
"""将不可变 DEB 上传至既有 Cloudflare R2；只在持有凭据的服务器运行。

输入与 APT 生成器共享身份校验，上传成功后再发布软件源索引。
历史版本永久保留，拒绝同名对象内容变更，不删除桶中的任何对象。
凭据由既有下载发布工具读取，本工具不会复制或打印凭据内容。
"""

import argparse
import hashlib
import importlib.util
from pathlib import Path
from urllib.request import Request, urlopen

from generate_repository import digest, fail, metadata


# /// @brief 发布全部 DEB 并核对对象内容和公开下载内容。
# /// @warning 桶及域名来自受保护的配置，不能由包控制字段指定。
def main():
    # 上传器只在持有既有凭据的服务器运行，不要求本机拥有云访问权。
    # 同一版本的归档必须保持不可变，客户端和 Gentoo 共用相同摘要。
    # 发布失败只停止索引更新，不撤回已经存在的历史对象。
    # CLI 只接收公开域名和输入包；不提供凭据参数或任意对象路径。
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", required=True)
    parser.add_argument("debs", nargs="+")
    args = parser.parse_args()
    # 复用已有严格权限和字段校验，不执行配置文件中的 shell 内容。
    helper = Path("/home/podman/.local/share/mmm-cdn/publish-r2.py")
    spec = importlib.util.spec_from_file_location("mmm_existing_cdn", helper)
    # helper 路径为受维护的固定私有工具路径，不从命令行动态执行代码。
    publisher = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(publisher)
    values = publisher.read_credentials("/home/podman/.config/mmm-cdn/credentials.env")
    # 域名匹配必须先于网络写入，防止软件源链接与上传目标属于不同仓库。
    if args.base_url != "https://" + values["CDN_DOMAIN"]:
        fail("CDN domain differs from protected server configuration")
    # SDK 仅在生产上传时加载，仓库生成与离线单元测试无需此依赖。
    import boto3
    from botocore.config import Config
    # 使用 SDK 默认 TLS 验证和受限重试，不通过公开 CDN 传输上传凭据。
    client = boto3.client(
        "s3", endpoint_url=values["R2_ENDPOINT"],
        aws_access_key_id=values["R2_ACCESS_KEY_ID"],
        aws_secret_access_key=values["R2_SECRET_ACCESS_KEY"], region_name="auto",
        config=Config(retries={"max_attempts": 3},
                      request_checksum_calculation="when_required",
                      response_checksum_validation="when_required"))
    # 全部包先确定身份和摘要；路径只来自经过白名单校验的控制字段。
    # 大文件摘要按块计算，内存不会随历史包数量增长。
    records = []
    for filename in args.debs:
        # 主生成器提供隔离暂存包，控制字段解析也拒绝其他产品和路径逃逸。
        source = Path(filename).resolve()
        key, _ = metadata(source, "musicmapmaker-next")
        records.append((source, key, digest(source), source.stat().st_size))
    for source, key, checksum, size in records:
        # 精确前缀列表处理对象不存在的正常情况，不吞掉认证或权限错误。
        objects = client.list_objects_v2(Bucket=values["R2_BUCKET"], Prefix=key)
        exists = any(item["Key"] == key for item in objects.get("Contents", []))
        if exists:
            # 版本化对象不可覆盖；差异必须由新的包修订号表达。
            # 旧对象必须已有可信摘要元数据，不能把未知文件接管为本项目版本。
            head = client.head_object(Bucket=values["R2_BUCKET"], Key=key)
            if head["ContentLength"] != size or head.get("Metadata", {}).get("sha256") != checksum:
                fail("existing CDN package has conflicting contents: " + key)
        else:
            # 保留原有对象键；不使用 website 部署的清理操作管理 DEB 历史。
            client.upload_file(str(source), values["R2_BUCKET"], key, ExtraArgs={
                "ContentType": "application/vnd.debian.binary-package",
                "ContentDisposition": "attachment",
                "CacheControl": "public, max-age=31536000, immutable",
                "Metadata": {"sha256": checksum}})
        # 检查上传期间的输入稳定性，再验证 R2 实际字节，不能仅相信元数据。
        if digest(source) != checksum:
            fail("package changed during CDN upload")
        body = client.get_object(Bucket=values["R2_BUCKET"], Key=key)["Body"]
        # StreamingBody 生命周期在验证完后结束，不将连接留给后续包。
        actual = hashlib.sha256()
        for block in iter(lambda: body.read(1024 * 1024), b""):
            actual.update(block)
        body.close()
        if actual.hexdigest() != checksum:
            fail("R2 package checksum mismatch")
        # 公开 CDN 下载也必须通过校验，否则不允许软件源切换索引。
        # 产品 User-Agent 与现有上传器保持一致，不使用浏览器验证页面作为包内容。
        request = Request(args.base_url + "/" + key,
                          headers={"User-Agent": "MusicMapMaker-Publisher/1.0"})
        actual = hashlib.sha256()
        # 完整正文核验防止公开地址被错误路由到 HTML 页面或过期缓存。
        with urlopen(request, timeout=60) as response:
            for block in iter(lambda: response.read(1024 * 1024), b""):
                actual.update(block)
        if actual.hexdigest() != checksum:
            fail("public CDN package checksum mismatch")
        # 成功输出仅含公开对象路径，不包含 endpoint、桶或认证信息。
        print("CDN verified: " + args.base_url + "/" + key, flush=True)


if __name__ == "__main__":
    main()
