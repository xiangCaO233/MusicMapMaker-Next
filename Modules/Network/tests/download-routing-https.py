"""使用本地可信 TLS 服务验证真实 curl 的选路和续传边界，不依赖外网。"""
import hashlib
import http.server
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import threading

# 每个场景启动独立测试进程，不能由前一场景的进程内地址缓存掩盖探测。
# 夹具由 CTest 传入套件程序及输出目录，工作目录不决定任何测试资源路径。
# OpenSSL 仅生成临时测试证书，生产证书和用户信任库完全不参与。
# 端口使用操作系统分配的回环临时端口，不固定占用开发者端口。
suite, output, openssl = sys.argv[1:4]
root = Path(output).resolve()
root.mkdir(parents=True, exist_ok=True)
payload = bytes(range(256)) * 8192
expected = hashlib.sha256(payload).hexdigest()
config = root / 'openssl.cnf'
config.write_text('[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n[dn]\nCN=downloads.xiang233.top\n[ext]\nsubjectAltName=DNS:downloads.xiang233.top,DNS:mmm.xiang233.top\nbasicConstraints=critical,CA:TRUE\n')
certificate, key = root / 'ca.pem', root / 'key.pem'
# SAN 保持真实请求主机名，不能只信任 CA 而跳过主机名验证。
# 短期证书只服务本次构建，不引入需要长期维护的过期资源。
# 生成的密钥只服务于当前构建树，不写到源码测试资源目录或 Git。
subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-config', str(config), '-keyout', str(key), '-out', str(certificate)], check=True, capture_output=True)
# 第二个身份用于分别区分链校验失败与主机名校验失败。
# 错主机名场景显式信任该 CA，仍必须因 SAN 不匹配而拒绝连接。
# 错信任场景服务端 SAN 正确，但客户端只信任另一个 CA，必须拒绝链。
bad_config, bad_certificate, bad_key = root / 'bad.cnf', root / 'bad-ca.pem', root / 'bad-key.pem'
bad_config.write_text(config.read_text().replace('downloads.xiang233.top', 'wrong-host.invalid'))
subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-config', str(bad_config), '-keyout', str(bad_key), '-out', str(bad_certificate)], check=True, capture_output=True)


class Handler(http.server.BaseHTTPRequestHandler):
    """只模拟单一不可变文件；故障只施加给首个正式续传请求。"""
    protocol_version = 'HTTP/1.1'

    def log_message(self, *_):
        # 正常成功日志由测试驱动汇总，避免多线程日志混淆请求顺序。
        pass

    # 每次请求均由新的状态解析 Range，不能借用前一个连接的偏移。
    # 探测与正式请求采用相同 payload，但只有正式续传会注入一次故障。
    # 首次故障之后恢复正常，才能检查客户端是否安全切换而非无限重试。
    def do_GET(self):
        request_range = self.headers.get('Range', '')
        self.server.requests.append(request_range)
        host = self.headers.get('Host', '')
        self.server.hosts.append(host)
        # 源站故障只施加给 R2 域名，以主机头证明手动模式没有暗中回退。
        if self.server.mode in ['r2-failure', 'auto-fallback'] and host == 'downloads.xiang233.top':
            self.send_response(503)
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
            self.close_connection = True
            return
        start, end = 0, len(payload) - 1
        if request_range:
            first, last = request_range.removeprefix('bytes=').split('-', 1)
            start, end = int(first), min(int(last), end) if last else end
        probing = request_range == 'bytes=0-1048575'
        fault = not probing and start != 0 and not self.server.fault_used
        if fault:
            self.server.fault_used = True
        mode = self.server.mode
        status, etag = (206 if request_range else 200), '"fixture"'
        # 如果服务器忽略 Range，curl 可能在正文回调前返回 RANGE_ERROR。
        # 错位的 206 则由生产正文边界检查负责拒绝；两者都必须从零重启。
        if fault and mode == 'ignore-range':
            status, start, end = 200, 0, len(payload) - 1
        elif fault and mode == 'wrong-offset':
            start += 1
        elif fault and mode == 'changed-etag':
            etag = '"replacement"'
        # body 的实际切片与声明区间一致，错误偏移场景只破坏起点契约。
        # 不能靠故意损坏正文让最终摘要失败来替代 Range 边界测试。
        # changed-etag 只改变实体标识，确保拒绝发生在真正追加数据之前。
        data = payload[start:end + 1]
        self.send_response(status)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('ETag', etag)
        self.send_header('Connection', 'close')
        # 主动断线使错误表现为 PARTIAL_FILE，不靠等待超时模拟故障。
        # 完整总长保持固定，重复或缺失字节都会由最终摘要捕获。
        if status == 206:
            self.send_header('Content-Range', f'bytes {start}-{end}/{len(payload)}')
        self.end_headers()
        try:
            if fault and mode == 'disconnect':
                # 声明完整余量却只发送一段，验证真正的 PARTIAL_FILE 恢复。
                self.wfile.write(data[:65536])
                self.wfile.flush()
                self.connection.shutdown(socket.SHUT_RDWR)
            else:
                self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, OSError):
            # 客户端主动拒绝错误 Range 时断开连接是预期行为。
            pass
        self.close_connection = True


# 每个结果包含真实请求序列，便于追踪哪一段内容被重下或续传。
# 不同场景使用独立目标文件，不能由之前的成功输出掩盖后续失败。
# 预期摘要在故障注入之外计算，避免客户端和夹具共同接受错误正文。
results = []
for mode in ['normal', 'ignore-range', 'wrong-offset', 'changed-etag', 'disconnect', 'fake-ip', 'warm-cache', 'wrong-host', 'wrong-trust', 'origin', 'r2', 'r2-failure', 'auto-fallback', 'switch-origin', 'switch-r2']:
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    # 客户端拒绝错误响应时会提前断开，服务端后台连接不应阻止进程退出。
    # 主监听线程仍在 finally 中关闭并 join，不能泄漏端口影响后续用例。
    server.daemon_threads = True
    server.mode, server.fault_used, server.requests, server.hosts = mode, False, [], []
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(bad_certificate if mode == 'wrong-host' else certificate, bad_key if mode == 'wrong-host' else key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    target = root / (mode + '.bin')
    # CONNECT_TO 仅改变测试 socket 目的地，证书与 SNI 仍按原官方域名验证。
    mapping = f'downloads.xiang233.top:443:127.0.0.1:{server.server_port}'
    addresses = ['198.18.0.1'] if mode == 'fake-ip' else ['1.1.1.1', '8.8.8.8']
    # 候选只是夹具输入，CONNECT_TO 保证不访问它们的真实公网服务。
    # 因此无外网 Runner 也可执行相同生产路径，不会扫描第三方服务。
    # 清除测试进程的显式代理环境，避免回环夹具被开发机代理截获。
    # 此设置只影响子测试进程，不修改应用、桌面代理或系统 DNS。
    # fake-ip 场景另外验证生产代码对假地址整组退出主动优选。
    env = {k: v for k, v in os.environ.items() if k.lower() not in ['http_proxy', 'https_proxy', 'all_proxy']}
    # 只控制测试进程的会话入口；生产程序不读取这个环境变量。
    env['MMM_TEST_DOWNLOAD_ROUTE'] = mode
    # 暖缓存场景使用同进程两次独立下载，第二次必须没有测速请求。
    # TLS 失败场景也保留候选重试，不能在换 IP 时偷偷降低验证要求。
    trusted = bad_certificate if mode in ['wrong-host', 'wrong-trust'] else certificate
    command = [suite, 'DownloadRoutingTest', '--fetch-twice' if mode == 'warm-cache' else '--fetch', 'https://downloads.xiang233.top/download/fixture.bin', str(target), str(len(payload)), str(trusted), mapping, '-', *addresses]
    try:
        process = subprocess.run(command, capture_output=True, text=True, env=env, timeout=45)
        actual = hashlib.sha256(target.read_bytes()).hexdigest() if target.exists() else ''
        if mode == 'r2-failure':
            # 自动模式可回退，但用户强制 R2 后失败就应失败，不能偷换源站。
            assert process.returncode != 0 and set(server.hosts) == {'downloads.xiang233.top'}, (process.stderr, server.hosts)
            results.append({'mode': mode, 'valid': True, 'hosts': server.hosts})
            continue
        if mode in ['wrong-host', 'wrong-trust']:
            # 拒绝必须发生在接收 HTTP 正文前；非零退出码但写入文件同样不算通过。
            # 没有 GET 请求证明验证并非依赖下载后 SHA256 才发现错误。
            assert process.returncode != 0 and target.stat().st_size == 0 and not server.requests, (mode, process.stdout, process.stderr, server.requests)
            results.append({'mode': mode, 'valid': True, 'requests': server.requests})
            continue
        # 返回码、完整内容及实际请求序列共同验证，不能只检查 helper 返回成功。
        assert process.returncode == 0 and actual == expected, (mode, process.stdout, process.stderr, server.requests, actual)
        if mode == 'origin':
            # 手动直连完全跳过 R2 探测，直接读取源站同名对象。
            assert server.requests == [''] and server.hosts == ['mmm.xiang233.top'], (server.requests, server.hosts)
        elif mode == 'switch-r2':
            # 从源站切回时，探针也必须重设 URL 和偏移，不能把源站速度误认作 R2。
            assert server.hosts[0] == 'mmm.xiang233.top' and set(server.hosts[1:]) == {'downloads.xiang233.top'}, (server.requests, server.hosts)
            assert server.requests.count('bytes=0-1048575') == 2, server.requests
        elif mode == 'r2':
            assert set(server.hosts) == {'downloads.xiang233.top'}, server.hosts
        elif mode in ['auto-fallback', 'switch-origin']:
            # 切换后的第一次源站请求必须没有 Range，不能追加旧路由的不完整正文。
            assert server.hosts[0] == 'downloads.xiang233.top' and server.hosts[-1] == 'mmm.xiang233.top' and server.requests[-1] == '', (server.requests, server.hosts)
        elif mode == 'fake-ip':
            # 假地址应整组退出测速，正式请求直接使用系统连接路径。
            assert server.requests == [''], server.requests
        elif mode == 'warm-cache':
            # 第二次只保留一条完整 GET，首轮两个探针不能被重复执行。
            # 最终文件摘要仍与原 payload 一致，复用不能截断或重复写入前缀。
            assert server.requests.count('bytes=0-1048575') == 2 and server.requests[-1] == '', server.requests
        elif mode in ['ignore-range', 'wrong-offset', 'changed-etag']:
            # 最终无 Range 请求证明发生截断重启，而不是盲目拼接。
            assert server.fault_used and server.requests[-1] == '', server.requests
        # 中断恢复必须从已收到的前缀加 64KiB 开始，而非重新下载或漏掉一段。
        # 该偏移来自服务端主动截断位置，与客户端内部计数实现无关。
        elif mode == 'disconnect':
            assert 'bytes=1114112-' in server.requests, server.requests
        results.append({'mode': mode, 'valid': True, 'requests': server.requests})
    finally:
        # 即使断言失败也关闭监听器；失败日志不能以占用端口作为后续副作用。
        # 测试输出保留在构建树，便于本机和 CI 检查同一失败证据。
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)

# 记录保持机器可读，方便本机与固定 Runner 使用同一套验收数据。
(root / 'results.json').write_text(json.dumps(results, indent=2))
print(json.dumps(results))
