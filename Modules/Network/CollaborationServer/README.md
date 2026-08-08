# CollaborationServer

该模块构建独立可执行文件 `MusicMapMaker-CollaborationServer`。服务只维护公开房间目录，并在参与者之间转发 WebRTC 的 SDP/ICE 协商消息；谱面操作与资源数据始终通过双方的 DataChannel 传输。

启动方式：

```text
MusicMapMaker-CollaborationServer --config /path/to/config.json
```

配置将信令监听地址、监听端口、TLS、容量与 ICE 服务分开。客户端同样只配置服务器地址、信令端口和 TLS 开关，不依赖固定 URL。`ice.address`、`stunPort`、`turnPort` 与 TURN 长期凭据会被服务端组合成 libdatachannel 支持的 ICE URI，并在房间配对时下发。`enableTurnTcp` 默认为 `false`，仅在使用支持 TURN/TCP 的 ICE 后端时开启。

生产部署建议由 Caddy 或 Nginx 在 HTTPS 入口代理 `/mmm-collaboration` 到本服务的回环地址；Coturn 需要独立开放 STUN/TURN 监听端口以及受限的 UDP relay 端口范围。

## P2P 与 TURN 回退

客户端始终使用可靠有序的 WebRTC DataChannel 传输协作数据。ICE 会先尝试本机候选和 STUN 发现的公网候选；只有双方网络无法直接打洞时，才会选择 TURN relay 候选。因此信令服务不承载谱面与资源流量，TURN 也不是默认数据路径。

当前客户端使用的 ICE 后端只启用已经验证的 TURN/UDP，生产配置必须保持 `enableTurnTcp` 为 `false`。服务端应开放 TCP/UDP 3478 和 UDP 49160～49200；若 Coturn 位于一对一 NAT 后，还必须用 `external-ip=公网地址/私网地址` 保持 relay 端口号原样映射。

## Coturn 部署

`deploy/musicmapmaker-coturn.service` 固定使用已经通过长期凭据 Allocate、ChannelBind 和双向 relay 验证的 `coturn/coturn:4.15.0-r0`。不要把生产 unit 改回浮动的 `latest`；升级 Coturn 时必须先在外网执行完整 TURN 认证与 relay 数据测试。Coturn 4.17 改变了 nonce 默认行为，不能只用 STUN Binding 成功作为升级验收。

把 `deploy/turnserver.conf.example` 复制到 `~/.config/coturn/turnserver.conf` 后，替换公网地址、私网地址、realm 和随机长期凭据。相同用户名与密码需要写入协作服务器 JSON；凭据不得提交到仓库或输出到进程诊断日志，诊断完成后应立即轮换。
