# 联机写谱架构与实施清单

## 目标与边界

联机写谱采用“房主权威、P2P 直连”的广义 C/S 模型，支持 2～8 个客户端协作。所有参与者运行同一个客户端和同一套协作代码，房主与其他客户端的结构差异仅为 `isHost == true`。

- 房主和访客都在本地维护完整谱面，并根据本地谱面实时渲染。
- 客户端必须先设置默认 Creator；内部仍使用数值 `PeerId` 路由，界面统一展示房主广播的 Creator。
- 访客只向房主提交规范化编辑请求；房主把本地与远端请求放入同一编辑请求队列。
- 房主按单调递增的 `revision` 提交请求，并广播已提交的增量操作。
- 客户端只应用房主提交的操作，不同步渲染数据、鼠标轨迹、ECS 实体编号或资源文件。
- 新加入或落后过多的客户端先接收谱面快照，再从快照版本继续接收增量操作。
- 第一阶段不实现去中心化合并、房主迁移和离线编辑合并。

## 数据流

```text
访客本地操作 ── EditRequest ──┐
                               v
房主本地操作 ─────────────> 房主编辑请求队列
                               │ 排序、校验、分配 revision
                               v
                         CommittedOperation
                               │
                  ┌────────────┴────────────┐
                  v                         v
              房主本地应用              P2P 广播给访客
                  │                         │
                  └────────────┬────────────┘
                               v
                         各客户端本地渲染
```

## 协议模型

每个房间拥有稳定的 `sessionId`，每个客户端拥有本次连接内稳定的 `clientId`。请求使用 `(clientId, clientSequence)` 去重；房主提交后分配全房间唯一且连续的 `revision`。

首批消息类型：

- `JoinRequest` / `JoinAccepted`：协商协议版本、客户端身份和当前房间版本。
- `SnapshotBegin` / `SnapshotChunk` / `SnapshotEnd`：传输初始完整谱面快照及校验值。
- `EditRequest`：访客提交一个或一批规范化编辑操作。
- `CommittedOperation`：房主广播已排序且可直接应用的增量操作。
- `RevisionAck`：访客确认已经连续应用到的版本，用于裁剪重放日志。
- `ResyncRequest`：检测到版本缺口或校验失败时请求从操作日志补发，无法补发时退回快照。
- `PresenceUpdate`：低优先级临时状态，例如光标与当前选区，不进入谱面版本历史。
- `ParticipantIdentity` / `ParticipantLeft`：增量同步 `PeerId` 与 Creator 的展示映射，不进入谱面版本历史。

增量操作必须引用谱面对象的稳定 ID，不得引用进程内的 `entt::entity`。可逆编辑应携带足够的前后状态，使 Undo/Redo 最终也能作为房主提交的普通操作同步。

## 模块边界

- `CollaborationProtocol`：消息 DTO、版本字段、序列化、大小限制和协议兼容性校验。
- `CollaborationPeer`：统一房主/访客状态机；`isHost` 只决定请求是直接入权威队列还是发送给房主。
- `ICollaborationTransport`：可靠有序字节通道抽象，不接触谱面业务。
- `LoopbackTransport`：进程内测试传输，可同时创建 2～8 个无窗口客户端。
- `WebRtcTransport`：基于 libdatachannel 的 DataChannel 实现；信令与谱面数据通道分离。
- `OperationJournal`：房主侧有界增量日志，支持去重、确认、缺口补发和快照回退。
- `BeatmapCollaborationAdapter`：规范化本地编辑命令、应用远端提交，并接入 `BeatmapSession` 的编辑命令队列。

## 实施 TODO

### P0：依赖与构建基础

- [x] 选择 libdatachannel 0.24.3，关闭媒体传输并保留 DataChannel/WebSocket。
- [x] 选择 Mbed TLS 3.6.7，统一 Windows/Linux/macOS TLS 后端。
- [x] 以 Git submodule 方式加入 libdatachannel 与 Mbed TLS 源码。
- [x] 为源码构建导出稳定的 `3rd_*` CMake target。
- [x] 为 `libdatachannel`、`libjuice`、`usrsctp`、`mbedtls` 分别提供预编译查找脚本。
- [x] 扩展头文件和 Debug/RelWithDebInfo 静态库暂存脚本。
- [x] 在 CI 构建机生成 Windows MSVC、MinGW clang、MinGW GCC、Linux GCC、Linux Clang 和 macOS AppleClang arm64 预编译包。
- [x] 同步预编译包回本机，验证 Git LFS 与 `SOURCES_BUILD=OFF` 消费构建。

### P1：可测试的协作内核

- [x] 定义带协议版本和长度限制的二进制消息封包。
- [x] 实现 `CollaborationPeer` 的房主/访客统一状态机。
- [x] 实现房主请求队列、`revision` 分配、请求去重和有界操作日志。
- [x] 实现可靠有序的 `LoopbackTransport`。
- [x] 用单进程 2～8 Peer 测试覆盖人数上限、并发请求、顺序收敛、重复包和版本缺口补发。
- [x] 使用默认 Creator 作为展示身份，拒绝未设置 Creator 的客户端并向全部参与者同步身份表。

### P2：谱面增量接入

- [ ] 为可同步谱面对象补齐跨客户端稳定 ID。
- [ ] 从现有 `LogicCommand` 提取与 UI 坐标、ECS 实体无关的规范化编辑操作。
- [ ] 让房主的本地与远端操作进入同一 `BeatmapSession` 编辑队列。
- [ ] 让所有客户端仅通过 `CommittedOperation` 更新协作谱面状态。
- [ ] 覆盖新增、删除、移动、批量属性修改和 BPM/时间线修改的收敛测试。

### P3：快照与恢复

- [ ] 复用谱面序列化器生成初始快照，增加分块、校验和大小上限。
- [ ] 实现加入期间的“快照版本 + 后续增量”无缝衔接。
- [ ] 实现重连后的增量补发；日志已裁剪时自动回退完整快照。
- [ ] 增加慢客户端背压、队列上限和断开策略。

### P4：真实 P2P 与本地多进程测试

- [ ] 实现 `WebRtcTransport`，谱面数据使用可靠有序 DataChannel。
- [ ] 实现最小 WebSocket 信令接口，只交换 SDP/ICE 与房间信息。
- [ ] 接入 STUN，并为无法直连的网络预留 TURN 配置。
- [ ] 增加 `--collab-profile-root` 等独立运行目录参数，避免本地多进程抢占配置文件。
- [ ] 提供一条命令启动 1 个房主和最多 7 个访客的本地测试环境。

### P5：产品化

- [ ] 权限、踢出与只读角色（协作内核已固定支持 2～8 个客户端）。
- [ ] Presence 限频和与权威谱面操作分离。
- [ ] 网络统计、操作延迟、重同步原因和日志脱敏。
- [ ] 协议兼容策略、模糊测试、畸形包和超大包防护。
- [ ] 评估房主迁移；在完成前房主退出即结束协作房间。

## 第一阶段验收标准

在不启动图形界面的单元测试中创建 1 个房主和 7 个访客。所有客户端并发提交增量编辑后，房主生成唯一连续版本，各客户端最终得到字节级一致的谱面模型；重复消息不重复应用，缺失增量可通过日志补齐。真实 P2P 传输接入后，同一套测试只替换 transport，不修改协作状态机和谱面适配器。
