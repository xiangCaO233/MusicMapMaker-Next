# 注释补充任务

## 执行规则

- 每个自维护代码文件独立按 `comment / (comment + code)` 计算，最低注释率为 30%；头文件与实现文件分别达标。
- 覆盖主项目与 IonCachyEngine 的源码、测试和构建脚本；排除外部第三方、生成文件、文档及纯数据资源。
- 每批处理一个职责，通常 5～10 个文件，大型文件可单独成批；补充接口契约和函数体关键实现说明，不扩展为全面行为审计。
- 日常只统计本批修改文件；模块完成时复扫该模块，全项目清单只在阶段收尾或明确要求时刷新。
- 纯注释批次统一格式化、检查差异并构建；测试按修改范围执行。独立行为问题不记入本文档，另行授权处理。
- 提交和推送仍需用户明确要求。

## 当前进度

更新时间：2026-09-12。

| 范围 | 进度 | 最后复扫 | 状态 |
| --- | ---: | --- | --- |
| `3rdpty/sources/IonCachyEngine` | 95/95 | 2026-09-09 | 已完成 |
| `Modules/Game/Logic` | 146/146 | 2026-09-10 | 已完成 |
| `Modules/MMM` | 48/48 | 2026-09-10 | 已完成 |
| `Modules/Audio` | 44/44 | 2026-09-10 | 已完成 |
| `Modules/Game/Canvas` | 57/57 | 2026-09-11 | 已完成 |
| `Modules/Game/Graphic` | 67/67 | 2026-09-12 | 已完成 |
| `Modules/Runtime` | 6/6 | 2026-09-12 | 已完成 |
| `Modules/Main` | 7/7 | 2026-09-12 | 已完成 |
| `Modules/Config` | 52/52 | 2026-09-12 | 已完成 |
| `Modules/Event` | 42/42 | 2026-09-12 | 已完成 |
| `Modules/Game/Common` | 27/27 | 2026-09-12 | 已完成 |

全项目 2026-09-09 的旧基线已因 Logic 与 MMM 后续补充而过期，不再作为当前未达标数量。阶段收尾时重新生成逐文件清单。

## 后续模块顺序

1. `Network`：协作与网络数据传输。
2. `Game/UI`：界面与演练服务。
3. `Game/include`、`Game/src`：Game 聚合入口。
4. `Updater`、`Log`：更新与诊断辅助。
5. 模块外自维护的构建、工具和资源脚本。

## 最近验证

- `Game/Logic`：模块复扫 146/146 达标；相关批次已完成格式化、差异检查、测试和主项目构建。
- `MMM`：模块复扫 48/48 达标；14 个格式与模型测试场景以及 `cmake --build build --parallel 12` 通过。
- `Audio`：模块复扫 44/44 达标；可用音频测试目标已完成构建和直接运行，完整构建通过。
- `Game/Canvas`：模块复扫 57/57 达标；本批 7 个大型实现均独立达到 30%，完整构建通过。当前构建树未注册 CTest（`Total Tests: 0`）。
- `Game/Graphic`：模块复扫 67/67 达标；字体子模块 9/9、平台外观与光标批次 6/6，以及 Vulkan 上下文、诊断、固定渲染资源、渲染器基础资源、主帧热路径、离屏命令录制、离屏资源生命周期、纹理生命周期、主题注册表、原生窗口状态机、三平台窗口适配器、ImGui Vulkan 初始化与内置主题实现和主题插件测试 28/28 达标。最后一项 `VKContextImguiImpl.cpp` 为 2233 行注释、4249 行代码，注释率 34.45%；完整构建及隔离配置下的 `ThemePluginLoaderTest` 1/1 通过。既有 `AsciiFontRasterizerTest` 通过记录继续有效。Win32/macOS 原生分支未在本机实际编译；`MacOSWindowUtils.mm` 与 `MacOSWindowAdapter.mm` 已完成静态差异检查。
- `Runtime`：模块复扫 6/6 达标；补充共享线程池、退出看门狗、回归测试和构建入口的线程生命周期与同步约束。完整构建及隔离配置下的 `ShutdownWatchdogTest` 1/1 通过。
- `Main`：模块复扫 7/7 达标；补充启动资源同步、PGO 写出与上传、主程序生命周期、平台构建和 Windows 图标资源说明。纯资源 `logo.svg` 按规则排除，`icon.rc` 作为自维护资源脚本计入；完整构建通过，当前没有 Main 专属 CTest。
- `Config`：模块复扫 52/52 达标；补充配置值序列化与旧字段迁移、应用路径、翻译缓存、皮肤包事务、皮肤 Lua 解析及对应回归测试的接口和实现约束。完整构建通过；隔离配置下 Config 注册的 19/19 项 CTest 全部通过。
- `Event`：模块复扫 42/42 达标，合计 639 行注释、1353 行代码，注释率 32.08%；补充事件分发层级、订阅生命周期、同步回调与锁约束、项目和 UI 事件载荷，以及 GLFW/ImGui 键码翻译边界说明。完整构建通过；隔离配置下 `BeatmapLoadDiagnosticPublisherTest`、`ProjectOpenOriginTest`、`ProjectOpenProgressStateTest`、`SaveResultFeedbackTest` 4/4 通过。
- `Game/Common`：模块复扫 27/27 达标，合计 1643 行注释、3242 行代码，注释率 33.63%；补充公共编辑类型、音频与视频探测、时间线兼容性、画布组件布局、渲染快照及无锁快照池的接口、算法和热路径约束。完整构建通过；隔离配置下 `VideoFrameDecoderTest`、`AudioResourceDragPayloadTest`、`BeatmapAudioTimelineCompatibilityTest` 3/3 通过。
- 完整构建仍会输出已知的 GCC 16 `stl_algobase.h` LLVM gold plugin 循环未展开提示；该构建问题不属于注释补充任务。

历史流水记录仅保存在 [归档](archive/comment-audit-history-2026-09-09.md)，日常不读取、不追加。
