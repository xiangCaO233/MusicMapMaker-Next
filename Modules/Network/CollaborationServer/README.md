# CollaborationServer

该模块构建独立可执行文件 `MusicMapMaker-CollaborationServer`。服务只维护公开房间目录，并在参与者之间转发 WebRTC 的 SDP/ICE 协商消息；谱面操作与资源数据始终通过双方的 DataChannel 传输。

启动方式：

```text
MusicMapMaker-CollaborationServer --config /path/to/config.json
```

配置将信令监听地址、监听端口、TLS、容量与 ICE 服务分开。客户端同样只配置服务器地址、信令端口和 TLS 开关，不依赖固定 URL。`ice.address`、`stunPort`、`turnPort` 与 TURN 长期凭据会被服务端组合成 libdatachannel 支持的 ICE URI，并在房间配对时下发。`enableTurnTcp` 默认为 `false`，仅在使用支持 TURN/TCP 的 ICE 后端时开启。

生产部署建议由 Caddy 或 Nginx 在 HTTPS 入口代理 `/mmm-collaboration` 到本服务的回环地址；Coturn 需要独立开放 STUN/TURN 监听端口以及受限的 UDP relay 端口范围。
