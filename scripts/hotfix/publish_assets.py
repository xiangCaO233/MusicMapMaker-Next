#!/usr/bin/env python3
"""在网站服务器上传已准备的热修复制品，最后切换选定清单。

本入口用于人工发布及失败恢复；程序与资源按候选范围分别校验。
凭据复用服务器已有受保护配置，日志只包含公开对象路径。
"""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import sys
import threading
import time
from urllib.parse import quote

from prepare_assets import digest, safe_path
from prepare_program import validate as validate_program


# /// @brief 校验本地候选与当前线上清单，返回待上传记录。
def validate(stage, root):
    """把候选视为不可信输入，在访问 R2 前完成全部本地校验。

    plan 只允许当前版本目录，不能用于写入程序文件或网站配置。
    新清单必须包含基线全部资源，未改条目必须保持原值。
    新条目和完整包都必须匹配已校验的暂存内容。
    root 中清单的精确摘要必须仍等于准备时的基线。
    任一检查失败都要求重新准备候选，不在发布阶段修补清单。
    """
    # 不读取任何凭据的校验步骤也可在开发机离线测试。
    stage = Path(stage).resolve()
    current = Path(root) / "download/assets/manifest.json"
    plan = json.loads((stage / "plan.json").read_text(encoding="utf-8"))
    manifest = json.loads((stage / "manifest.json").read_text(encoding="utf-8"))
    if digest(current) != plan["baseline_sha256"]:
        raise ValueError("published baseline changed; prepare again")
    base = manifest["base_version"]
    revision = manifest["asset_revision"]
    if not re.fullmatch(r"v\d+\.\d+\.\d+", base) or not isinstance(revision, int) or revision <= 0:
        raise ValueError("invalid release identity")
    old = json.loads(current.read_text(encoding="utf-8"))
    if old.get("base_version", old["version"]) != base or revision <= old.get("asset_revision", 0):
        raise ValueError("release is not newer than baseline")
    prefix = f"download/hotfix/assets/{base}/r{revision}/"
    seen = set()
    for record in plan["uploads"]:
        key = str(safe_path(record["key"]))
        # 发布目录只允许本次修订，不接受计划任意写入网站文件。
        if not key.startswith(prefix) or key in seen:
            raise ValueError("invalid or duplicate upload key")
        seen.add(key)
        source = stage / key
        if source.is_symlink() or not source.resolve().is_relative_to(stage):
            raise ValueError("upload source escapes staging root")
        if digest(source) != record["sha256"] or source.stat().st_size != record["size"]:
            raise ValueError("staged checksum mismatch")
    # 每个新引用必须对应一个已验证制品，未变资源必须完整保留旧条目。
    old_entries = {e["path"]: e for e in old["files"]}
    names = [str(safe_path(e['path'])) for e in manifest['files']]
    if len(names) != len(set(names)) or not plan['uploads']:
        raise ValueError('duplicate candidate paths or empty publication')
    if manifest['version'] != f'{base}-assets.{revision}':
        raise ValueError('manifest version does not match release identity')
    # 候选资源名还需独立去重，禁止按条目顺序反复覆盖同一路径。
    records = {r["key"]: r for r in plan["uploads"]}
    if not set(old_entries).issubset({e["path"] for e in manifest["files"]}):
        raise ValueError("candidate removed baseline files")
    for entry in manifest["files"] + [manifest["package"]]:
        if entry.get("path") in old_entries and entry == old_entries[entry["path"]]:
            continue
        # 新制品 URL 采用本次版本目录，不能引用另一修订的未验证对象。
        key = next((k for k in records if entry["url"] == "/" + quote(k, safe="/")), None)
        if key is None or entry["sha256"] != "sha256:" + records[key]["sha256"] or entry["size"] != records[key]["size"]:
            raise ValueError("candidate references unverified contents")
    return manifest, plan


# /// @brief 收集本次选定的资源及程序清单，保证全部上传前均通过预检。
def validate_bundle(stage, root):
    """一个入口允许仅程序、仅资产或两者同时更新。

    资源清单先发布，程序清单最后发布，使新程序不会先看到缺失的新资产。
    各候选均绑定独立原清单摘要，失败时保留暂存制品用于重新准备。
    两个固定 JSON 无法跨 CDN 原子提交，因此资源必须兼容旧正式版本。
    """
    # 文件存在性只决定范围，各份清单仍独立完成全部内容校验。
    documents = []
    if (Path(stage) / 'manifest.json').exists():
        document, plan = validate(stage, root)
        documents.append((document, plan, 'download/assets/manifest.json'))
    if (Path(stage) / 'program.json').exists():
        document, plan = validate_program(stage, root)
        documents.append((document, plan, 'download/check/check.json'))
    if not documents:
        raise ValueError('publication has no candidate manifests')
    # 返回顺序是资源、程序，资源必须兼容同一正式版本旧修订。
    return documents


# /// @brief 把 SDK 的分片回调汇总为有限频率的可见进度。
class TransferProgress:
    """只打印公开对象名及字节数，不接触凭据或请求头。

    SDK 可能在多个线程调用回调；重试时还可能回退已发送计数。
    锁只保护本地计数和输出，不阻塞网络线程等待另一项传输完成。
    """
    def __init__(self, key, size):
        """每个对象独立计数，开始阶段由调用方另行输出。"""
        self.key, self.size, self.sent = key, size, 0
        self.last = time.monotonic()
        self.lock = threading.Lock()

    def __call__(self, count):
        """最多每五秒输出一次；达到总字节数时立即报告。"""
        with self.lock:
            self.sent = max(0, self.sent + count)
            now = time.monotonic()
            # 回调代表数据交给 SDK，不代表 R2 已确认写入成功。
            # 上传完成后的正文和公开 CDN 校验仍由调用方独立执行。
            if now - self.last >= 5 or self.sent >= self.size:
                print(f'[hotfix] 上传 {self.key}: {self.sent}/{self.size} bytes', flush=True)
                self.last = now


# /// @brief 上传并验证一个不可变对象，成功后才写入本地直连副本。
def publish_record(client, bucket, domain, stage, root, record, publisher, transfer):
    """恢复复用同摘要对象，不允许覆盖同修订的不同内容。

    每个阶段均立即输出，SSH 保活期间也能判断当前是在上传还是校验。
    传输配置由正式入口统一创建，测试可注入独立客户端验证事务边界。
    """
    key = record['key']
    source = Path(stage) / key
    print(f'[hotfix] 检查对象 {key} ({record["size"]} bytes)', flush=True)
    objects = client.list_objects_v2(Bucket=bucket, Prefix=key)
    if any(e['Key'] == key for e in objects.get('Contents', [])):
        head = client.head_object(Bucket=bucket, Key=key)
        if (head.get('Metadata', {}).get('sha256') != record['sha256']
                or head['ContentLength'] != record['size']):
            raise ValueError('immutable object conflict')
        print(f'[hotfix] 复用已有对象 {key}', flush=True)
    else:
        # 小于 64MiB 的程序走单次 PUT，避免默认十路分片在弱链路上拥塞。
        # 更大的对象仍支持分片，最大并发由统一 TransferConfig 限制。
        client.upload_file(str(source), bucket, key, Config=transfer,
                           Callback=TransferProgress(key, record['size']), ExtraArgs={
            'Metadata': {'sha256': record['sha256']},
            'ContentType': 'application/octet-stream',
            'CacheControl': 'public, max-age=31536000, immutable'})
    print(f'[hotfix] 回读 R2 正文 {key}', flush=True)
    body = client.get_object(Bucket=bucket, Key=key)['Body']
    actual = hashlib.sha256()
    try:
        for block in iter(lambda: body.read(1024 * 1024), b''):
            actual.update(block)
    finally:
        # 网络或摘要失败同样释放连接，避免恢复时积累失效 socket。
        body.close()
    if actual.hexdigest() != record['sha256'] or digest(source) != record['sha256']:
        raise ValueError('uploaded object checksum mismatch')
    print(f'[hotfix] 校验公开 CDN {key}', flush=True)
    publisher.verify_public(domain, '/' + quote(key, safe='/'), record['sha256'])
    target = Path(root) / key
    target.parent.mkdir(parents=True, exist_ok=True)
    # 只有两处正文验证完成后才推进本地镜像，保留原有不可变约束。
    if target.exists() and digest(target) != record['sha256']:
        raise ValueError('local immutable object conflict')
    shutil.copyfile(source, target)
    print(f'[hotfix] 对象完成 {key}', flush=True)


# /// @brief 服务器加载固定 R2 身份，调用者永远不接触原始密钥。
def load_backend():
    """程序与资源发布共享同一受保护配置，不接受外部桶或端点参数。

    此函数仅在服务器执行；CI 只接收限时、限定对象的上传许可。
    返回客户端与公开域名，不返回可被调用方意外序列化的凭据字典。
    """
    # 只加载受维护的固定 helper，凭据读取保持现有权限校验。
    helper = Path("/home/podman/.local/share/mmm-cdn/publish-r2.py")
    spec = importlib.util.spec_from_file_location("mmm_cdn", helper)
    publisher = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(publisher)
    values = publisher.read_credentials("/home/podman/.config/mmm-cdn/credentials.env")
    import boto3
    from botocore.config import Config
    # 显式连接和读取超时使断链能够失败返回，而非无限等待。
    # 标准重试次数有上界，候选原件保持不变，便于之后恢复。
    client = boto3.client("s3", endpoint_url=values["R2_ENDPOINT"],
                          aws_access_key_id=values["R2_ACCESS_KEY_ID"],
                          aws_secret_access_key=values["R2_SECRET_ACCESS_KEY"],
                          region_name="auto", config=Config(signature_version="s3v4", connect_timeout=15, read_timeout=120,
                          retries={"mode": "standard", "total_max_attempts": 4},
                          request_checksum_calculation="when_required",
                          response_checksum_validation="when_required"))
    # 桶与公开域名来自受保护配置，workflow 输入不得覆盖。
    bucket = values["R2_BUCKET"]
    domain = values["CDN_DOMAIN"]
    return client, bucket, domain, publisher


# /// @brief 服务器验证候选后签发单对象上传许可，不切换任何清单。
def sign_uploads(stage, root):
    """许可仅有效三十分钟，固定对象、长度、摘要元数据及缓存头。

    CI 用已有候选直接上传 R2，避开网站服务器的慢速上传出口。
    后续 publish 仍完整回读 R2/CDN 正文，签名成功不代表发布完成。
    本函数的 JSON 输出含限时许可，工作流必须只写私有临时文件。
    """
    documents = validate_bundle(stage, root)
    client, bucket, _, _ = load_backend()
    items = []
    for record in [r for _, plan, _ in documents for r in plan['uploads']]:
        key = record['key']
        # 已有对象只在摘要与长度匹配时复用，不重新签发覆盖许可。
        objects = client.list_objects_v2(Bucket=bucket, Prefix=key)
        if any(e['Key'] == key for e in objects.get('Contents', [])):
            head = client.head_object(Bucket=bucket, Key=key)
            if (head.get('Metadata', {}).get('sha256') != record['sha256']
                    or head['ContentLength'] != record['size']):
                raise ValueError('immutable object conflict')
            continue
        params = dict(Bucket=bucket, Key=key, ContentLength=record['size'],
                      Metadata={'sha256': record['sha256']},
                      ContentType='application/octet-stream',
                      CacheControl='public, max-age=31536000, immutable')
        url = client.generate_presigned_url('put_object', Params=params,
                                           ExpiresIn=1800, HttpMethod='PUT')
        items.append(dict(**record, url=url))
    # 不记录 URL 到日志；SDK 长期访问密钥始终保留在服务器。
    return {'uploads': items}


# /// @brief 在持有网站发布锁时执行上传和清单切换。
def publish(stage, root):
    """使用服务器现有身份发布不可变资源，再更新固定清单入口。

    密钥始终留在网站服务器，候选目录和主仓不保存凭据。
    每个新对象要同时通过 R2 正文和公开 CDN 正文校验。
    对象部分上传成功时可以重跑，已有相同内容会复用。
    已有不同内容会失败，不允许修订号悄悄对应另一份资源。

    固定资源清单必须最后发布，客户端不会看到尚不可下载的新对象。
    CDN 清单使用既有 no-store 发布帮助器，制品使用 immutable 缓存。
    本地源清单原子替换前保留摘要命名的旧副本，便于人工恢复。
    SDK 或公开访问失败不改动本地当前清单。
    """
    # 服务器锁串行资源热更新，CI 另与完整网站发布共用 debsrv 发布锁。
    import fcntl
    with Path("/home/podman/.local/share/mmm-cdn/publish.lock").open("a") as lock:
        print('[hotfix] 等待发布锁', flush=True)
        fcntl.flock(lock, fcntl.LOCK_EX)
        print('[hotfix] 已取得发布锁，校验候选', flush=True)
        # 全部候选先预检，程序不合法时不能先上传部分资产。
        documents = validate_bundle(stage, root)
        client, bucket, domain, publisher = load_backend()
        from boto3.s3.transfer import TransferConfig
        # 采用公开 SDK 配置接口；与正文摘要校验相互独立。
        # 小程序单连接传输，大对象最多双路并发，减少弱链路拥塞。
        transfer = TransferConfig(multipart_threshold=64 * 1024**2,
                                  multipart_chunksize=16 * 1024**2,
                                  max_concurrency=2, preferred_transfer_client='classic')
        records = [record for _, plan, _ in documents for record in plan['uploads']]
        print(f'[hotfix] 共 {len(records)} 个制品，'
              f'{sum(r["size"] for r in records)} bytes', flush=True)
        for record in records:
            publish_record(client, bucket, domain, stage, root, record, publisher, transfer)
        # 完整部署尚未共用锁时也能检测其是否改变了本次基线。
        validate_bundle(stage, root)
        for document, plan, manifest_key in documents:
            # 固定入口保存原字节副本，备份名由原摘要唯一确定。
            current = Path(root) / manifest_key
            backup = current.parent / (current.name + '.' + plan['baseline_sha256'] + '.backup')
            if not backup.exists():
                shutil.copyfile(current, backup)
            # 只改本次上传对象的 URL，保留未选择资产及平台元数据。
            publisher.rewrite_resources(document, domain, {
                '/' + r['key']: (r['sha256'], r['size']) for r in plan['uploads']})
            # JSON 公开校验通过后再切换直连入口，失败保留当前本地清单。
            print(f'[hotfix] 发布清单 {manifest_key}', flush=True)
            checksum = publisher.publish_json(client, bucket, manifest_key, document)
            publisher.verify_public(domain, '/' + manifest_key, checksum)
            if digest(current) != plan['baseline_sha256']:
                raise ValueError('baseline changed during publication')
            publisher.atomic_json(current, document)
            print(f'[hotfix] 清单已切换 {manifest_key}', flush=True)
            # 前一份清单完成后处理下一份，程序入口总是最后发布。


# /// @brief CLI 默认为校验，显式 apply 才访问 R2 并切换资源清单。
def main():
    """默认 validate 可用于 CI 预检，不依赖服务器 SDK 或权限。

    apply 分支仅在服务器导入 boto3 和凭据帮助器。
    命令行不允许指定 R2 桶、endpoint 或任意凭据路径。
    所有远端身份由固定受保护配置决定。
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True)
    parser.add_argument("--root", required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--apply", action="store_true")
    mode.add_argument("--sign-uploads", action="store_true")
    args = parser.parse_args()
    if args.sign_uploads:
        # 私有输出只能通过 SSH 写入 Runner 临时文件，不能上传为 Artifact。
        json.dump(sign_uploads(args.stage, args.root), sys.stdout)
    elif args.apply:
        publish(args.stage, args.root)
    else:
        # 离线校验模式不导入 boto3，也不读取服务器凭据。
        validate_bundle(args.stage, args.root)


if __name__ == "__main__":
    main()
