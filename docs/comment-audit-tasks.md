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

1. `Game/Logic`：会话、编辑指令、项目管理与保存；146/146 个文件已完成。
2. `MMM`：谱面模型与格式读写；作为下一模块按职责分批处理。
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

范围为该目录下全部 146 个自维护文件（头文件、实现、测试和 CMake）。2026-09-10 因本机旧明细缺失重建本模块基线，129 个达标、17 个不足 30%；本轮完成后按同一文件清单复扫为 146/146，剩余 0 个，未扫描其他模块。比例达标不代替关键实现说明。

已补充的历史职责见前次记录。本轮完成音频描述符、打击特效、自动采样、拖动虚影和画布组件渲染测试、NoteRenderSystem 快照编排和 BeatmapSession 会话渲染实现，继续处理会话、编辑、交互及其余渲染实现与测试，不代表模块全部完成。

已完成：`ProjectAudioReferenceTest.cpp` 补充夹具、引用保护、歌曲提示、模板迁移、资源重命名和文件移动回滚的关键流程，687 注释行 / 1603 代码行（30.00%）。已格式化并直接运行相关测试；批量用例验证功能完整性，不以此证明复杂度，文本哨兵仅证明额外文本保留，字节一致性由失败回滚用例验证。

已完成：`EditorEngine.cpp` 补充逻辑线程调度、会话创建与切换、项目工作区恢复、命令权限路由、跨画布同步、剪贴板生命周期和音频资源事务的接口契约及关键实现说明，1498 注释行 / 3425 代码行（30.43%）。保留热路径与低频文件操作边界，说明 SessionRegistry 快照保活、全局 transport 所有权、资源重命名回滚和临时项目另存后的路径身份重建。

已完成：`BeatmapSession_Commands.cpp` 补充命令文件职责、项目路径与资源解析、玩家轨道迁移、Main BGM 重定向、外部修改哈希、Malody 兼容写出、临时转换文件、MCZ 音频原点对齐预检、谱面包逐项写入、命令消费、协作权威替换及各类保存与导出处理，1029 注释行 / 2396 代码行（30.04%）。

已完成：`ActionController_Editing.cpp` 补充可撤销编辑协议、剪贴板秒/拍换算、Polyline 父子结构、Note/Sample 混合动作、Timeline 与批注事务、协作对象差量和多数据域权威替换的关键约束，1145 注释行 / 2657 代码行（30.12%）。

已完成：`CanvasCameraTest.cpp` 按场景补充主画布投影、轨道域隔离、画笔资源类型、跨区手势、协作草稿、自动采样和撤销往返说明，并在测试体内标明准备、命令与往返断言的关键边界，2068 注释行 / 4758 代码行（30.30%）。这是本模块最后一个原未达标文件；未将简单断言扩展为行为审计。

## 最近完成批次：音频测试与会话渲染快照

补充资源解析、事件身份、完整指纹与 Main 同步指纹的边界，说明同 ID 和跨匹配方式冲突、静音草稿及批量事件断言；补充同图集混合模式切批的几何索引约束。

NoteRenderSystem 单独成批，补充缓存借用、补间条件、静态与动态顶点边界、特效命令层次、辅助区裁剪、Timing 像素行占用与几何区间、预览范围和调试命中框说明。

| 文件（相对主仓） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| Modules/Game/Logic/tests/AudioTimelineDescriptorTest.cpp | 280 | 653 | 30.01% |
| Modules/Game/Logic/tests/HitEffectStereoTest.cpp | 145 | 338 | 30.02% |
| Modules/Game/Logic/src/logic/ecs/system/render/NoteRenderSystem.cpp | 527 | 1219 | 30.18% |
| Modules/Game/Logic/src/logic/session/BeatmapSession_Rendering.cpp | 620 | 1436 | 30.16% |
| Modules/Game/Logic/tests/SampleRenderSystemTest.cpp | 306 | 712 | 30.06% |
| Modules/Game/Logic/tests/NoteDragGhostRenderTest.cpp | 283 | 656 | 30.14% |
| Modules/Game/Logic/tests/CanvasComponentRenderSystemTest.cpp | 315 | 731 | 30.11% |
| Modules/Game/Logic/tests/BeatmapMutationObserverBindingTest.cpp | 446 | 1035 | 30.11% |
| Modules/Game/Logic/src/logic/session/InteractionController.cpp | 688 | 1602 | 30.04% |
| Modules/Game/Logic/src/logic/session/tool/GrabTool.cpp | 650 | 1514 | 30.04% |
| Modules/Game/Logic/src/logic/session/tool/DrawTool.cpp | 626 | 1460 | 30.01% |
| Modules/Game/Logic/src/logic/ecs/system/render/NoteRenderSystem_Notes.cpp | 857 | 1999 | 30.01% |

BeatmapSession 渲染实现单独成批，补充批注目标解析与时间分组、ECS 脏索引及统计口径、辅助视图非阻塞限频、快照缓冲借用、视口映射、部件检视与拍位拟合、工具预览和最终发布约束。

SampleRenderSystemTest 补充轨道底色与标题顺序、标签滚动裁剪、独立 BGM 布局、临时画笔及交互发光、UTF-8 字形复用与缺字请求说明。验证发现尺寸用例以助手固定 1.2 倍生成物件，却按默认配置计算期望值；改为显式传入默认宽高缩放，覆盖此前 0.95 倍默认值修改。

NoteDragGhostRenderTest 补充按绘制命令提取纹理范围、局部 Registry 和排序索引生命周期、Draft/Player/BGM 独立轨宽、Flick 端点与 Polyline 子项投影、画笔几何和命中框的验证边界。

CanvasComponentRenderSystemTest 补充固定字体与 DPI 档位、组件显示条件、拍号和分拍时间的布局扩展及 scissor、KPS 实例覆盖与紧凑文本、滚动计数窗口和格式化边界。

BeatmapMutationObserverBindingTest 补充本地与权威回调类别、批注目标与权限、活动画笔保留、本地回执和权威包含序号、准备差量的实体身份、离线和细分权限门禁，以及文件门闩夹具的 latch/join 顺序。

InteractionController 补充选择状态、屏幕投影、分区物件包围框、时间区间索引与去重、悬浮与工具路由、采样音量批次、轨道重映射、鼠标归属和框选重算；通过非注释 token 对比和主项目构建。

GrabTool 补充折线退化与来源身份、统一目标投影、多选及局部位移、跨域转换校验、整组回退和撤销事务；说明既有子实体扫描点。非注释 token 与 HEAD 一致，主项目构建通过。

DrawTool 补充起笔与续接、分区坐标、形状增长与退化、尾部合并和擦除分裂的实现约束，已格式化、通过非注释 token 对比与主项目构建。

NoteRenderSystem_Notes 补充可见性包络与分桶、播放补间余量、分层几何与命中、不同根的重叠检测、遮罩拆分及画笔预览。纠正候选必然时间有序的旧说明，记录既有局部分配与排序成本；非注释 token 对比及主项目构建通过。

已登记而未扩展修复：GrabTool 局部节点及子实体同步的全表扫描；DrawTool 续接删除后日志读取父组件，以及尾部合并追加向量元素后仍使用末段引用的生命周期风险。

验证：clang-format、差异检查和非注释 token 对比通过；AudioTimelineDescriptorTest、HitEffectStereoTest、SampleRenderSystemTest、NoteDragGhostRenderTest、CanvasComponentRenderSystemTest、BeatmapMutationObserverBindingTest、ProjectAudioReferenceTest 七个相关 CTest 通过；各批分别完成主项目构建。除自动采样尺寸测试显式使用默认配置外，仅修改注释与格式，本批注释按用户要求提交并暂停，模块尚未全部达标；构建开关已恢复原来的 BUILD_TESTING=OFF。

此前验证记录：ImdPackageExportServiceTest 直接运行因无法解码 source.wav 返回 1，未在注释任务中扩展排查；产物保留于 build/test_output/imd-comment-check-V7zLTZ。

## MMM 当前进度

2026-09-10 按模块实际文件清单统计 48 个自维护文件，本轮完成后逐文件复扫为 48/48 独立达到 30%，剩余 0 个。已覆盖领域模型、项目元数据、BPM 与变速、osu!、RM/IMD、原生 MMM、Malody 读写、共享测试辅助和 CMake 注册说明；大型兼容测试按场景补充格式契约、边界判定与关键实现说明，不扩展为格式行为审计。

验证：逐文件 cloc 为 48/48，clang-format、cmake-format 和差异检查通过；直接运行 BPM、IMD、三个 osu!、两个 Malody、Malody 边界、绑定音效、两个原生 MMM、元数据兼容、变速覆盖及打包扩展名共 14 个场景均通过；`cmake --build build --parallel 12` 完整构建通过。链接仍输出已知的 GCC 16 `stl_algobase.h` LLVM gold plugin 未展开循环提示，本轮未修改构建优化策略。
