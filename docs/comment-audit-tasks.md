# 注释补充任务

## 当前状态

- ICE 注释补充阶段收尾，不再以完成全面安全、异常或实时性审计为前提。
- 最近一次已保存统计（2026-09-09）：ICE 95/95 文件达到 30%；全项目 1018 个有效文件，其他 685 个不足 30%。这是历史基线，本次未复扫。
- 范围包含主仓与 ICE 自维护代码、头文件、测试和脚本，排除外部第三方、生成文件、文档及纯资源。未达标明细在本机 `build/test_output/comment-audit/under-30.csv`，缺失时在阶段收尾重建。
- [历史记录](archive/comment-audit-history-2026-09-09.md)只归档保存，日常不读取、不追加。

## 执行方式

- 每批一个职责，通常 5～10 个文件；大型实现可单独一批。按下面的模块优先级逐批推进。
- 补接口契约和函数体关键实现说明；每个文件独立达到 30%，不堆砌注释、不用头文件抵扣实现文件。
- 阅读以解释当前实现所需为限，不递归审计整个调用链；发现行为问题简短登记，另行授权修复。
- 日常只统计本批改动文件；全项目统计留到阶段收尾或明确请求时执行。
- 纯注释批次完成后统一格式化、检查差异并构建一次；测试按风险执行，修改测试或测试逻辑时运行相关测试。不为纯注释新增故障注入、压力或跨平台探针。
- 每批只更新完成项，简短记录逐文件注释行、代码行、比例及验证结论；不追加逐轮流水账，不重复全量清单。提交、推送仍须用户明确要求。

## 模块顺序

旧的未完成批次已清空。目录来自实际 `ls Modules` 与 `ls Modules/Game`；以下顺序按编辑正确性、数据安全、运行核心、界面与辅助功能排序，不采用字母顺序。各模块的头文件、实现、测试及构建脚本一并归入对应模块，已完成的 ICE 和演练模型不重复补充。

1. `Game/Logic`：会话、编辑指令、项目管理与保存；按用户最新要求停在 130/146，剩余文件等待后续指示，不自动继续。
2. `MMM`：谱面模型与格式读写。
3. `Audio`：播放、时间同步与导出。
4. `Game/Canvas`：编辑交互、坐标与吸附。
5. `Game/Graphic`：渲染和图形资源生命周期。
6. `Runtime`、`Main`：线程、启动与退出。
7. `Config`：配置持久化及资源加载。
8. `Event`、`Game/Common`：跨模块通信与公共基础。
9. `Network`：协作与网络数据传输。
10. `Game/UI`：界面及演练服务等剩余内容。
11. `Game/include`、`Game/src`：Game 聚合入口。
12. `Updater`、`Log`：更新及诊断辅助。

模块外的自维护构建、工具和资源脚本留到模块阶段之后处理；届时按实际目录选批，不预先展开文件级长清单。

## Game/Logic 当前进度

范围为该目录下全部 146 个自维护文件（头文件、实现、测试和 CMake）。2026-09-09 收尾时仅复查本模块，逐文件比例达标 130 个，剩余 16 个；比例达标不代替关键实现说明。按用户要求到此停止，不代表模块全部完成。模块明细在本机 `build/test_output/comment-audit/logic-files.csv`，未扫描其他模块。

已补充：SessionRegistry、RenderSyncRegistry、BeatmapSyncBuffer 类型入口、NoteIdentity、RegistrySnapshotLifetimeTest、ProjectStorage 及其测试、ProjectDirectoryScanner、ProjectDirectoryWatcher 及其路径过滤测试、BeatmapBackupService 及其测试。继续处理剩余职责，不提前宣布模块完成。

## 最近批次：播放视觉时钟与重播控制测试

已完成 PlaybackVisualClockTest 与 PlaybackRestartTest 的场景意图、快照状态转换、预期公式及全局状态恢复说明。仅修改注释与格式，两个文件分别达到 30%。

| 文件（相对主仓） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| Modules/Game/Logic/tests/PlaybackVisualClockTest.cpp | 236 | 549 | 30.06% |
| Modules/Game/Logic/tests/PlaybackRestartTest.cpp | 204 | 476 | 30.00% |

验证：格式化、差异检查、两个测试目标及主项目构建通过；两个测试直接运行均返回 0。缺失资源警告来自测试既有夹具。未验收真实设备发声或 UI 行为，未提交。

此前验证记录：ImdPackageExportServiceTest 直接运行因无法解码 source.wav 返回 1，未在注释任务中扩展排查；产物保留于 build/test_output/imd-comment-check-V7zLTZ。
