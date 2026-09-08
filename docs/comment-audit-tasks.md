# 逐文件注释审查与分批任务

## 提交记录（2026-09-08）

按用户“提交一下”的要求，ICE 子仓已提交为 `658224d`（`docs(ice): 补充音频引擎注释与构建契约说明`），包含 74 个文件的注释及格式变更；主仓随本记录提交子仓引用、任务单与验收索引。提交前重新通过 C++ 去注释一致性、构建脚本词法一致性及 diff 检查，全量复扫仍为 ICE 92/92 数值达标。未推送；本次只提交已保存工作，不恢复审计，也不将提交视为全部验收通过。以下暂停交接记录中的“未提交”描述的是暂停当时状态。

## 本次暂停交接（2026-09-08）

用户要求“记录下进度，这次到此为止”，已停止继续审计，等待用户明确恢复；不把暂停标记为目标完成或环境阻塞。

- ICE 92 个有效自维护文件全部达到逐文件 30%，另 2 个空文件不适用。主仓仍有 685 个不足文件，未扩大修改范围。
- [验收索引](ice-comment-audit-index.md) 已登记全部 94 个文件。注释终核与 SHA-256 快照累计完成 13 个有效文件，剩余 79 个仍需依据当前内容确认；历史复核记录不能直接当作最终通过。
- 本轮新增确认九文件：顶层、src、依赖入口三个 CMakeLists；configure-project、macOS 交叉工具链、Buildfmt 三脚本；execptions 目录下三个错误类型头。均只读复核，cmake-format --check 与 clang-format --dry-run --Werror 通过，未新增源码改动。
- 最近一次独立 ICE Clang64 构建成功，CTest 1/1 通过（1.45 秒）。主项目本轮重新执行 `cmake --build build --parallel 12`，进程已以退出码 1 结束，仍因本机 libc++ 缺少原子 shared_ptr 特化失败，不是修复提交被回退。未修改工具链或主项目逻辑以绕过该门槛。
- 工作区改动均保留，未提交、未推送。无本轮构建进程仍在运行。统计及不足清单位于 `build/test_output/comment-audit/`，清理 build 会删除这些产物；关键统计与进度已保存至任务单及验收索引。
- 恢复后从验收索引中未登记终核的 79 个有效文件继续；先校验已登记摘要，只审查变化内容。全部质量证据、最终验证门槛尚未满足，不能宣布所有审计通过。

## 基线与口径

2026-09-07，主仓基线 `f2fc908b`；包含当前工作区与 ICE 已跟踪的自维护源码，未修改业务实现。使用 cloc 2.08，开启 `--by-file --skip-uniqueness`，相同内容的不同文件仍分别计数。

- 有效文件 **1009** 个，**758** 个注释率低于 30%，**251** 个仅在数值上达到要求，尚不能认定注释质量全部合格。
- 主项目 **918** 个文件中 **682** 个不足；ICE **91** 个文件中 **76** 个不足。
- `.h/.hpp`：412 个中 176 个不足；`.cpp/.cc/.cxx`：431 个中 425 个不足。此分类不含模板 `.in`、Objective-C++ 等其他后缀。
- 全部有效文件共有注释 31582 行、代码 219580 行；整体比例不是验收指标，不能抵扣单文件不足。
- 5 个空源文件单独标记不适用。包含构建脚本、测试代码、运行时 Lua/着色器、可执行的插件示例及自维护 `.in` 模板；排除外部第三方、预编译包、生成目录、代理头、纯翻译表、外来谱面测试资源。
- ICE 自维护的 `3rdpty/cmake` 和 `3rdpty/sources/cmake` 计入；其内部第三方库源码不读取、不修改。

完整逐文件明细：`build/test_output/comment-audit/files.csv` 与 `files.json`；范围清单：`files.txt`；排除原因：`excluded.json`；模块汇总：`summary.json`。`baseline-files.csv`、`baseline-files.json` 和 `baseline-summary.json` 保留首次补注释前的统计；无前缀文件为本批完成后重扫结果。这些是构建目录中的本次审查产物，清理构建目录会移除它们。

**证据边界**：逐文件行数进行了全量统计；函数体语义和注释质量进行了风险导向抽查，并非已经逐个审阅所有函数。不能把数值达标等同于函数内注释完整。

## 模块分布

| 范围 | 文件数 | 数值不足文件 |
| --- | ---: | ---: |
| Game/UI | 249 | 181 |
| Game/Logic | 146 | 97 |
| ICE | 91 | 76 |
| Game/Graphic | 67 | 41 |
| Game/Canvas | 57 | 41 |
| MMM 谱面模型/格式 | 48 | 42 |
| Network | 46 | 32 |
| Audio | 42 | 29 |
| Config | 52 | 35 |
| Event | 42 | 36 |
| 主仓自维护 3rdpty 构建脚本 | 56 | 52 |
| scripts | 30 | 30 |
| cmake | 17 | 15 |

其他范围见完整明细，不能因为未出现在此摘要表就免检。

## 抽查发现：需要补哪里、重写哪里

1. **头文件契约不能代替实现注释**：ICE `include/ice/core/MixBus.hpp` 为 46.97%，但 `src/ice/core/MixBus.cpp` 为 0.38%。`process` 中的分块、失败静音和快照释放路径，以及 `acquireSourceSnapshot` / `reclaimRetiredSourcesLocked` 的 hazard 保护与回收时序，几乎没有实现侧说明。需要解释不变量与时序，不能复制头文件描述填满开头。
2. **实时处理的状态协议缺说明**：ICE `RStretcher.hpp` 为 56.69%，对应 `.cpp` 为 0.94%。应在 `process`、`prewarm`、起始 padding、延迟丢弃和 final drain 附近解释帧数、结束状态和预分配约束。
3. **高比例也可能是假象**：ICE `src/sample.cpp` 为 88.33%，主体却是注释掉的旧 FFmpeg 示例代码。应先确认样例是否还使用，再清理/替换成有效示例说明；不能把这些废弃代码当合格文档。此任务不授权顺带删除文件或修改运行行为。
4. **已达标实现仍有注释重构空间**：`VKRenderPass.cpp` 为 31.18%。同步依赖段解释了约束，但不少附件设置注释仅重复 setter 名称；应保留同步原因，将浅层复述改成 load/clear、初末布局及调用方契约说明。
5. **协议与格式转换**：`BeatmapDocumentCodec.cpp` 为 3.71%，`SaveMMMMap.hpp` 为 4.76%。已有少量格式/字段标题，需要补外层封装、长度边界、压缩回退、格式版本、路径及事件映射的语义，避免逐字段重复赋值内容。
6. **刚完成的演练模块也未达标**：`WelcomeView.cpp` 0.96%、`WalkthroughModel.cpp` 1.73%、`WalkthroughService.cpp` 2.07%、`WalkthroughPage.cpp` 0.46%。重点为声明校验和依赖环、临时文件/备份回滚、事件信号与真实完成条件、停靠/焦点恢复、标签状态及 ImGui 栈配对。不能因为功能刚通过测试而豁免。
7. **大型实现与测试**：`ToolbarView.cpp` 1.61%、`VKContextImguiImpl.cpp` 2.90%、`CanvasCameraTest.cpp` 3.20%、`Basic2DCanvas_Interaction.cpp` 5.88%。先按函数/场景梳理职责，再在算法、状态切换和断言附近补理由。测试需要说明构造场景为何能覆盖回归和断言代表什么约束，而不是给每个断言机械加一句“检查结果”。

## 执行规则

- 一批以一个职责为边界，通常处理 1 至 3 个文件；头文件和实现文件独立验收，不能互相抵扣。
- 先通读实现和调用者，再写函数头契约与函数体关键说明；不改业务行为、原子序、异常策略、文件格式或第三方代码。
- 每个文件检查 `comment / (comment + code) >= 30%`，同时人工检查函数体注释位置及内容；不通过行数目标自动生成填充注释。
- 明细中的 `minimum_additional_comment_lines` 仅是保持代码行数不变时的数学差额，不是要求填充的注释配额。全量差额约 69439 行，说明这是长期治理任务，不能一次堆注释解决。
- 修改 C++ 后执行 clang-format 和限 60% CPU 的构建、相关测试；若格式化造成无关改动，应收敛差异。每批记录前后比例与验证结果。
- 主仓与 ICE 的改动按仓库和职责分开；只有用户要求时才提交，不自动推送。

## 待办批次

- [x] A0：建立主仓与 ICE 的逐文件基线，区分数值不足和质量问题，完成风险抽查。
- [x] A1：ICE `MixBus.cpp` 实现注释，复核 `MixBus.hpp` 的线程/生命周期约束；首批完成，验证边界见下文。
- [x] A2：ICE `RStretcher.cpp` 与对应头文件，解释预热、padding、delay、final 输入和尾部取出。
- [x] A3：ICE `TimeStretcher.cpp` 与头文件，解释重建、参数生效和实时路径边界；验证见 A3 完成记录。
- [x] A4：ICE `SourceNode.cpp`、`ALPlayer.cpp` 按各自职责分批处理，补来源切换、队列与播放状态；A4.2 的主项目构建限制见完成记录。
- [x] A5：ICE `FFmpegFileReceiver.cpp`、`FFmpegDecoderInstance.cpp` 注释及配套头契约复核完成，已说明状态、重采样、EOF 与回收；行为缺陷与测试边界仍见记录。
- [x] A6：ICE `TimeStretcherRealtimeTest.cpp` 注释与观测口径复核完成；`sample.cpp` 已独立复核并清理历史注释代码。运行覆盖限制见实时测试收尾记录，不代表全部行为规范通过。
- [ ] B1：演练 `WalkthroughModel.h/.cpp`，补章节顺序、占位主题、步骤 DAG 校验和进度归约。
- [ ] B2：演练 `WalkthroughService.h/.cpp`，补线程队列、只读恢复保护和写入失败回滚。
- [ ] B3：`WelcomeView.h/.cpp`、`WalkthroughPage.h/.cpp` 分两批，补页面生命周期与布局约束。
- [ ] B4：`WalkthroughTest.cpp`、`WelcomeViewTest.cpp`，补场景/预期/回归含义。
- [ ] C1：`EditorEngine.cpp`、`ProjectController.cpp` 及对应头文件逐组处理，补项目切换、会话恢复和副作用顺序。
- [ ] C2：`ActionController_Editing.cpp`、`BeatmapSession_Commands.cpp` 分批，补编辑指令、不变量和撤销语义。
- [ ] C3：画布交互、抓取工具、时间线弹窗分批，补坐标转换、吸附、焦点和拖放生命周期。
- [ ] C4：渲染系统和 `VKContextImguiImpl.cpp` 分批；复核已达标的 `VKRenderPass.cpp`，补同步与资源生命周期说明。
- [ ] D1：Network 协作房间和文档编解码分批，补协议边界、顺序、冲突处理和断线恢复。
- [ ] D2：MMM 的 Load/Save 头文件按格式分批，补版本兼容、时间单位、路径和有损映射。
- [ ] D3：Audio、Config、Common、Event、Main、Runtime、Updater、Log 等剩余代码，按完整明细逐文件推进。
- [ ] E1：工具栏、设置、元数据、音频管理等 UI 实现按职责拆批，补状态联动和布局约束。
- [ ] E2：剩余测试按被测模块归批；优先大型回归测试，说明边界条件和断言依据。
- [ ] F1：主仓 CMake、工具脚本、预编译布局脚本分批；ICE 构建脚本单独归批。
- [ ] F2：运行时 Lua、着色器和插件示例，补输入输出、坐标/颜色约定、皮肤覆盖行为。
- [ ] G1：复查所有数值达标文件的注释质量，清理无意义堆砌/过时说明，再执行最终全量逐文件验收。

## 首批完成记录：A1

仅修改 ICE `src/ice/core/MixBus.cpp`，补充控制线程/音频线程边界、分块混音、失败静音、快照保护与回收、声道路由及显示电平语义。头文件已复核，本批未修改。

| 文件 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| ICE `src/ice/core/MixBus.cpp` | 1 → 115 | 262 → 262 | 0.38% → 30.50% |
| ICE `include/ice/core/MixBus.hpp`（只读复核） | 93 → 93 | 105 → 105 | 46.97% → 46.97% |

验证：clang-format、主项目 `cmake --build build --parallel 12`、ICE 此翻译单元的 `clang++ -std=c++23 -fsyntax-only` 均通过；差异检查确认全部 114 行变更均为新增注释，无删除行或业务代码变更。

当前 `SOURCES_BUILD=OFF`，`MixBusRealtimeSafetyTest` 只在源码构建时注册，因此本次没有运行该测试，也没有重建 ICE 预编译库。主项目构建不能当成 ICE 运行时验证；本批为注释变更，已单独验证 ICE 源码编译。

首批重扫后仍有 **757/1009** 个文件不足，其中 ICE 为 **75/91**。这是 A1 完成时的历史记录，最新结果见下文。
## ICE 后续批次记录（2026-09-07）

本轮只推进 ICE 注释治理，不改变业务代码、同步操作、异常策略或预编译二进制。以下统计包含前一批 MixBus 的工作区修改。

- 当前 ICE 有效文件 91 个，**55 个数值达标，36 个仍不足**；相较首轮基线 76 个不足，累计减少 40 个，其中本轮减少 39 个。
- 工作区修改的 **44 个 ICE 文件均逐文件达到 30%**。新增说明分布在接口契约、函数体边界、状态转换、缓冲容量与生命周期处；没有用头文件抵扣实现文件。
- `sample.cpp` 移除了不可执行的注释代码，改为说明占位入口与正式实现归属；该文件没有加入构建目标，也没有补造运行行为。
- 尚未完成 ICE 全量验收；原本数值达标但未完整检查的文件仍保留质量复核任务。

### 已完成的职责批次

- [x] A2：RubberBand 包装的预热、起始填充、延迟丢弃、final 输入和尾部取出；修正倍率并发访问约束。
- [x] A4.1：SourceNode 块内起播、游标推进、循环/结束回调、参考时钟 hazard 获取与控制侧回收。
- [x] A5.1：FFmpeg 探测工厂及实例公开接口；解释时长估算、元信息复制、封面所有权及失败语义。实例实现仍待处理。
- [x] A6.1：清理 sample.cpp 的废弃注释代码，明确它不属于可运行验证。
- [x] A7：AudioTrack、Cachy/StreamingDecoder 及基础接口；明确首次等待、占位策略和追加借用视图语义。
- [x] A8：IEffectNode、Compressor、PitchAlter、Clipper 的已有实现和限制；未把历史分配或空实现描述为已实时化。
- [x] A9：AudioBuffer 头文件及两种平台存储实现；解释逻辑帧数、容量、固定存储跨度、移动及 SIMD 前置条件。
- [x] A10：配置、错误类型、输出接收基类、分配统计工具及线程池空翻译单元。
- [x] F1.1：ICE 顶层、src、3rdpty 三个构建入口；其余 Find、布局、源码依赖和跨编译脚本仍待审查。

### 逐文件结果

下表路径均相对于 ICE 根目录；代码行数因格式化可能变化，验证还检查了去注释、空白后的文本一致性。

| 文件 | 注释行：基线 → 当前 | 代码行：基线 → 当前 | 注释率：基线 → 当前 |
| --- | ---: | ---: | ---: |
| `3rdpty/CMakeLists.txt` | 3 → 8 | 17 → 17 | 15.00% → 32.00% |
| `CMakeLists.txt` | 11 → 30 | 70 → 70 | 13.58% → 30.00% |
| `include/ice/config/config.hpp` | 5 → 12 | 27 → 27 | 15.63% → 30.77% |
| `include/ice/core/effect/Clipper.hpp` | 3 → 7 | 15 → 15 | 16.67% → 31.82% |
| `include/ice/core/effect/Compresser.hpp` | 13 → 23 | 53 → 53 | 19.70% → 30.26% |
| `include/ice/core/effect/IEffectNode.hpp` | 31 → 32 | 40 → 40 | 43.66% → 44.44% |
| `include/ice/core/effect/PitchAlter.hpp` | 14 → 21 | 33 → 33 | 29.79% → 38.89% |
| `include/ice/core/effect/rubberband/RStretcher.hpp` | 89 → 90 | 68 → 68 | 56.69% → 56.96% |
| `include/ice/core/IAudioNode.hpp` | 4 → 6 | 14 → 14 | 22.22% → 30.00% |
| `include/ice/core/PlayCallBack.hpp` | 5 → 8 | 17 → 17 | 22.73% → 32.00% |
| `include/ice/execptions/buffer_error.hpp` | 0 → 5 | 11 → 11 | 0.00% → 31.25% |
| `include/ice/execptions/instance_build_error.hpp` | 0 → 5 | 11 → 11 | 0.00% → 31.25% |
| `include/ice/execptions/load_error.hpp` | 0 → 5 | 11 → 11 | 0.00% → 31.25% |
| `include/ice/manage/AudioBuffer.hpp` | 84 → 122 | 283 → 283 | 22.89% → 30.12% |
| `include/ice/manage/AudioFormat.hpp` | 0 → 6 | 13 → 12 | 0.00% → 33.33% |
| `include/ice/manage/AudioTrack.hpp` | 14 → 23 | 53 → 52 | 20.90% → 30.67% |
| `include/ice/manage/dec/AlbumArt.hpp` | 16 → 24 | 54 → 52 | 22.86% → 31.58% |
| `include/ice/manage/dec/CachyDecoder.hpp` | 10 → 18 | 42 → 41 | 19.23% → 30.51% |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp` | 2 → 8 | 18 → 18 | 10.00% → 30.77% |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp` | 4 → 10 | 23 → 23 | 14.81% → 30.30% |
| `include/ice/manage/dec/IDecoder.hpp` | 5 → 12 | 21 → 21 | 19.23% → 36.36% |
| `include/ice/manage/dec/IDecoderFactory.hpp` | 4 → 10 | 22 → 22 | 15.38% → 31.25% |
| `include/ice/manage/dec/IDecoderInstance.hpp` | 5 → 8 | 18 → 18 | 21.74% → 30.77% |
| `include/ice/manage/dec/MediaInfo.hpp` | 2 → 7 | 16 → 15 | 11.11% → 31.82% |
| `include/ice/manage/dec/StreamingDecoder.hpp` | 5 → 12 | 27 → 27 | 15.63% → 30.77% |
| `include/ice/out/IReceiver.hpp` | 19 → 21 | 30 → 30 | 38.78% → 41.18% |
| `include/ice/tool/AllocationTracker.hpp` | 4 → 18 | 40 → 40 | 9.09% → 31.03% |
| `src/CMakeLists.txt` | 6 → 22 | 50 → 50 | 10.71% → 30.56% |
| `src/ice/config/config.cpp` | 0 → 5 | 10 → 10 | 0.00% → 33.33% |
| `src/ice/core/effect/Clipper.cpp` | 2 → 3 | 6 → 6 | 25.00% → 33.33% |
| `src/ice/core/effect/Compresser.cpp` | 26 → 29 | 70 → 54 | 27.08% → 34.94% |
| `src/ice/core/effect/IEffectNode.cpp` | 0 → 18 | 42 → 42 | 0.00% → 30.00% |
| `src/ice/core/effect/PitchAlter.cpp` | 6 → 12 | 33 → 28 | 15.38% → 30.00% |
| `src/ice/core/effect/rubberband/RStretcher.cpp` | 3 → 135 | 315 → 315 | 0.94% → 30.00% |
| `src/ice/core/MixBus.cpp` | 1 → 115 | 262 → 262 | 0.38% → 30.50% |
| `src/ice/core/SourceNode.cpp` | 8 → 124 | 288 → 288 | 2.70% → 30.10% |
| `src/ice/manage/AudioBuffer.cpp` | 12 → 41 | 94 → 94 | 11.32% → 30.37% |
| `src/ice/manage/AudioTrack.cpp` | 2 → 20 | 46 → 41 | 4.17% → 32.79% |
| `src/ice/manage/dec/CachyDecoder.cpp` | 32 → 54 | 126 → 126 | 20.25% → 30.00% |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.cpp` | 31 → 67 | 156 → 156 | 16.58% → 30.04% |
| `src/ice/manage/dec/StreamingDecoder.cpp` | 4 → 12 | 28 → 28 | 12.50% → 30.00% |
| `src/ice/out/IReceiver.cpp` | 0 → 3 | 5 → 5 | 0.00% → 37.50% |
| `src/ice/thread/ThreadPool.cpp` | 0 → 2 | 4 → 4 | 0.00% → 33.33% |
| `src/sample.cpp` | 106 → 6 | 14 → 13 | 88.33% → 31.58% |

### 本轮验证与边界

- 修改的 C++/头文件执行 clang-format，三个 CMake 文件执行 cmake-format；两层仓库 `git diff --check` 通过。
- 44 个修改文件与 ICE HEAD 比较，去除注释和空白后的文本全部一致；这项检查覆盖 C++ 与 CMake，不替代运行测试。
- 在 `build/test_output/ice-comment-audit-build` 独立配置 ICE：`SOURCES_BUILD=OFF`、`ICE_BUILD_TESTS=ON`、`ICE_LINKAGE=static`、`RelWithDebInfo`。此模式编译当前 ICE 自有源码并链接已有依赖包，没有重建任何外部第三方源码。
- ICE 自身源码、示例程序、实时安全测试编译链接通过；`IonTimeStretcherRealtimeTest` 通过（1/1）。
- 主项目 `cmake --build build --parallel 12` 通过；该构建仍使用 ICE 预编译包，不能替代上面的 ICE 独立源码验证。
- AllocationTracker.hpp 另做单独语法检查通过；直接以主输入编译头文件产生 pragma-once 提示，不是引擎构建失败。
- 未验证 Windows/macOS 构建、实体音频设备或每个遗留效果器的实时行为；未同步或重建 ICE 发布二进制。
- 未提交、未推送。

### 发现但未擅自修改的行为问题

以下是注释审查发现的独立修复候选，并非已修复或通过运行回归验证的行为：

- StreamingDecoder 与 Clipper 仍是占位实现；非空对象不能证明可播放或已限幅。
- Compressor 的参数 setter 写入非原子脏标记，系数更新分支可能扩容；PitchAlter 仍有容量调整、共享指针复制及首次后端创建。
- FFmpegDecoderFactory 的历史 AVCALL_CHECK 宏将比较结果赋给 ret，不能保留原始负错误码；其 probe 主路径不使用该宏。
- AudioBuffer 的历史 SIMD 立体声拆分入口存在四帧步长与八浮点对齐写入冲突，旧的“已完成解交错”注释已收敛，需单独验证并修复。
- 既有异常类、原始分配及诊断输出只是保留原行为，新增注释不意味着授权新增这些用法。

### ICE 下一轮逐文件任务

下列清单保留上一批的 36 个待处理文件及其后续进度；A3 完成后仍有 35 个未达到数值门槛。继续处理输出后端和解码实现，再处理测试与构建脚本。除此之外，ThreadPool 等原有数值达标文件仍须人工质量复核。

- [ ] `tests/TimeStretcherRealtimeTest.cpp`：10.51%。
- [x] `src/ice/out/play/openal/ALPlayer.cpp`：6.79% → 30.22%，A4.2 完成注释审查，验证限制见下文。
- [x] `src/ice/core/effect/TimeStretcher.cpp`：5.88% → 30.04%，A3 完成。
- [x] `src/ice/out/io/FFmpegFileReceiver.cpp`：6.55% → 30.27%，实现注释与配套头契约一致性已复核。
- [x] `3rdpty/cmake/modules/PrebuiltLayout.cmake`：7.27% → 30.13%，公共接口与实现分支已复核，布局探针通过。
- [x] `3rdpty/sources/cmake/Buildrubberband.cmake`：7.07% → 30.06%，Meson 参数、元数据、平台分支与缓存说明已复核。
- [x] `3rdpty/sources/cmake/Buildffmpeg.cmake`：7.25% → 30.23%，参数构造、功能清单、缓存与安装边界已复核。
- [x] `src/ice/core/effect/GraphicEqualizer.cpp`：1.05% → 30.26%，实现和头文件已复核。
- [x] `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp`：20.89% → 30.24%，A5.2 已复核。
- [x] `3rdpty/sources/cmake/Buildsdl.cmake`：7.05% → 30.29%，平台裁剪、缓存及包装目标已复核。
- [x] `3rdpty/sources/cmake/Buildlame.cmake`：6.77% → 30.34%，交叉参数、归档补齐与缓存边界已复核。
- [x] `src/main.cpp`：13.48% → 30.32%，实际图连接、手工演示边界及线程生命周期说明已复核。
- [x] `3rdpty/sources/cmake/Buildzlib.cmake`：2.88% → 30.34%，已复核注释与配置文本。
- [x] `3rdpty/sources/cmake/Buildlibsamplerate.cmake`：3.81% → 30.41%，已复核注释与配置文本。
- [x] `3rdpty/cmake/modules/Findffmpeg.cmake`：1.16% → 30.51%，已复核。
- [x] `src/ice/out/play/sdl/SDLPlayer.cpp`：18.50% → 30.64%，已复核。
- [x] `3rdpty/sources/cmake/Buildfftw.cmake`：10.09% → 31.94%，外部构建、CRT、缓存与产物接口已复核。
- [x] `3rdpty/cmake/modules/FindSDL3Static.cmake`：3.39% → 30.49%，已复核。
- [x] `3rdpty/sources/cmake/Buildopenal.cmake`：13.98% → 30.43%，平台裁剪、链接与消费宏已复核。
- [x] `3rdpty/cmake/modules/Findrubberband.cmake`：1.89% → 30.67%，已复核。
- [x] `3rdpty/cmake/modules/FindOpenAL.cmake`：5.26% → 30.26%，已复核。
- [x] `3rdpty/cmake/modules/Findspdlog.cmake`：4.44% → 30.65%，已复核。
- [x] `cmake/cross/clang-cl-gcc-compatible.sh`：4.44% → 30.65%，已复核并检查 shell 语法。
- [x] `include/ice/manage/AudioPool.hpp`：24.06% → 30.05%，已复核。
- [x] `3rdpty/cmake/modules/Findzlib.cmake`：2.56% → 30.91%，已复核。
- [x] `cmake/cross/merge-msvc-archives.sh`：4.88% → 30.36%，已复核并检查 shell 语法。
- [x] `3rdpty/cmake/modules/Findfmt.cmake`：2.63% → 30.19%，已复核。
- [x] `3rdpty/cmake/modules/Findlibsamplerate.cmake`：2.63% → 30.19%，已复核。
- [x] `3rdpty/cmake/modules/Findfftw.cmake`：2.78% → 30.00%，已复核。
- [x] `src/ice/core/effect/filter/BiquadFilter.cpp`：14.04% → 30.00%，实现和头文件已复核。
- [x] `3rdpty/cmake/modules/Findlame.cmake`：3.13% → 31.11%，已复核。
- [x] `src/ice/manage/AudioPool.cpp`：9.09% → 32.00%，已复核。
- [x] `include/ice/out/play/sdl/SDLPlayer.hpp`：24.64% → 43.96%，已复核。
- [x] `cmake/cross/llvm-lib-ar-compatible.sh`：6.67% → 33.33%，已复核并检查 shell 语法。
- [x] `3rdpty/sources/cmake/Buildspdlog.cmake`：22.22% → 30.00%，已复核注释与配置文本。
- [x] `3rdpty/sources/cmake/arch-probe.cmake`：27.59% → 32.26%，已复核注释与配置文本。

该批复扫：主仓加 ICE 仍有 **718/1009** 个文件不足；主仓仍为 682/918，未在该批混入业务模块注释改动。

## A3 完成记录：TimeStretcher（2026-09-07）

本批只处理变速节点的头文件与实现，保留此前工作区修改，不改算法、参数、原子序或控制流程。

- 补充控制侧预热发布、音频侧激活、退役链回收的所有权与线程边界。
- 在函数体内说明暂停冻结请求、输入小数余量、连续段切分、定位重置、显式边界旧段排尾音、跨块输入计划保留，以及 final 提交与预算交付的区别。
- 明确 provider 配置仅做有限次读取，不阻塞等待配置稳定；配置序号不保护外部 context 生命周期，控制写入者必须串行。
- 修正公开查询语义：实际倍率是诊断值，激活状态时可先发布设定值；final drained 表示应用层预算已交付，不保证后端物理队列为空；拒绝计数也包含格式不匹配。

| ICE 相对路径 | 注释行：前 → 后 | 代码行：前 → 后 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `src/ice/core/effect/TimeStretcher.cpp` | 46 → 316 | 736 → 736 | 5.88% → 30.04% |
| `include/ice/core/effect/TimeStretcher.hpp` | 155 → 172 | 120 → 120 | 56.36% → 58.90% |

验证：clang-format、两层仓库 diff 检查、去除注释与空白后的代码一致性检查、ICE 独立源码构建、`IonTimeStretcherRealtimeTest`（1/1）及主项目构建均通过。ICE 独立构建沿用 `build/test_output/ice-comment-audit-build`，只编译引擎自有源码并链接现有第三方预编译依赖；未构建外部依赖源码、未更新预编译二进制。

全量复扫仍按逐文件口径，包括主项目与 ICE 自维护头文件、实现、测试及脚本，排除外部第三方和生成/纯数据文件。当前 ICE **56/91** 个文件数值达标，**35** 个仍不足；全项目 **717/1009** 个仍不足。数值已达标的其他文件仍需质量复核，不能据此宣称 ICE 全量审计完成。

下一批优先处理 `ALPlayer.cpp`、`FFmpegFileReceiver.cpp` 和 `FFmpegDecoderInstance.cpp`，随后推进实时测试与构建脚本。未提交、未推送。

## A4.2 完成记录：ALPlayer（2026-09-08）

本批处理 ICE `ALPlayer.cpp/.hpp`，补充默认设备回退、分阶段失败回收、图绑定与句柄生命周期、队列退回与欠载恢复、暂停与空间化切换、平面音频下混及上传格式约束。函数体内说明锁顺序、队列重建丢弃旧块但不回退图游标，以及日志截断和错误查询的边界。英文接口说明改为中文 Doxygen。

| ICE 相对路径 | 注释行：前 → 后 | 代码行：前 → 后 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `src/ice/out/play/openal/ALPlayer.cpp` | 56 → 333 | 769 → 769 | 6.79% → 30.22% |
| `include/ice/out/play/openal/ALPlayer.hpp` | 58 → 82 | 63 → 63 | 47.93% → 56.55% |

### 验证结果与边界

- 两个文件已执行 clang-format；两层仓库 `git diff --check` 通过；与 ICE HEAD 比较，识别字符串后去除注释和空白的业务文本一致。
- 使用本机实际存在的 `C:\msys64\msys2_shell.cmd -clang64 -shell fish -defterm -no-start -here` 进入 clang64；编译器为 Clang 22.1.8，并行度为 12（20 个逻辑处理器的 60%）。
- ICE 独立源码构建使用 `build/test_output/ice-comment-audit-build`，配置为 `SOURCES_BUILD=OFF`、`ICE_LINKAGE=static`、`ICE_BUILD_TESTS=ON`、`RelWithDebInfo`。引擎自有源码与示例、测试编译链接通过，`IonTimeStretcherRealtimeTest` 通过（1/1）。日志位于 `build/test_output/comment-audit/ice-build.log` 与 `ice-test.log`。
- 主项目 `cmake --build build --parallel 12` **未通过**。`BeatmapSession.h`、`RenderSyncRegistry.h`、`SessionRegistry.h` 中的 `std::atomic<std::shared_ptr<...>>` 在当前 clang64 libc++ 中触发普通 `atomic<T>` 的 trivially-copyable 静态断言，日志见 `build/test_output/comment-audit/main-build.log`。
- 已核对 `ac301275`（`fix(logic): 迁移原子共享指针接口`）属于当前 develop 历史，修复未丢失；该提交将旧式原子自由函数迁移为 `std::atomic<std::shared_ptr<...>>`。本机 libc++ 的 `version` 头中 `__cpp_lib_atomic_shared_ptr` 未启用，当前失败是迁移后接口与本地标准库的兼容问题。本批未修改这些业务文件或编译参数来绕过错误。
- ICE 回归测试不涉及实体 OpenAL 设备，因此不作为设备播放、暂停竞态或多实例上下文安全的验证。没有重建外部第三方源码，也没有更新发布预编译包。

### 当前全量复扫

本机没有上一轮构建目录中的基线产物，因此重新依据两层 Git 跟踪路径生成范围清单，并用 cloc 2.08 `--by-file --skip-uniqueness` 逐文件统计。范围包括主项目与 ICE 的业务头文件、实现、测试、自维护 CMake/工具/CI 脚本、运行时 Lua、着色器和可执行插件示例；排除外部第三方、预编译包、代理头、构建产物、纯配置数据模板、文档、纯翻译表及外来测试数据。`.cmake.in` 按 CMake 计数，头文件模板按 C++ 计数，5 个空文件单列为不适用。

当前有效文件 **1012** 个，**719** 个不足；主仓 **921** 个中 **685** 个不足，ICE **91** 个中 **34** 个不足。该重建清单比历史基线多 3 个有效文件，不把总数差异直接解释为本批业务代码变化；本批 ICE 数值不足减少 1 个，其余达标文件仍保留质量复核任务。

完整明细为 `build/test_output/comment-audit/files.csv`、`files.json`；其他未达标文件逐项列在 `under-30.csv`；范围、排除理由和汇总分别为 `files.txt`、`excluded.json`、`summary.json`。这些产物在构建目录中，清理构建目录会移除它们；本轮未重建或覆盖历史基线快照。

### 注释审查确认的现有约束

- `m_sourceMutex` 不参与基类 `set_source` 写入，图绑定仍必须在停止拉取后替换。
- 供数循环末尾的欠载恢复没有再次检查暂停，可能覆盖同轮控制侧暂停；空间开关与队列重建异步完成，重建会丢弃已预读的旧块。
- 供数线程现有互斥量、1ms 轮询休眠及排队失败时的格式化输出不满足无阻塞实时约束；新增说明没有把这些行为描述为已修复。

下一批继续 A5 的 `FFmpegFileReceiver.cpp` 或 `FFmpegDecoderInstance.cpp`，各按独立职责处理。未提交、未推送。

## ICE 全量审计持续推进记录（2026-09-08）

目标为全部 ICE 自维护文件完成比例与质量审计，当前仍在进行。A4.2 后依次处理四个职责批次：FFmpeg 解码实例、SDL 输出、音轨池、图形均衡器与双二阶滤波。各批次只补充或修正说明并格式化，不改变业务文本。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp` | 16 | 23 | 41.03% |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp` | 192 | 443 | 30.24% |
| `include/ice/out/play/sdl/SDLPlayer.hpp` | 40 | 51 | 43.96% |
| `src/ice/out/play/sdl/SDLPlayer.cpp` | 72 | 163 | 30.64% |
| `include/ice/manage/AudioPool.hpp` | 61 | 142 | 30.05% |
| `src/ice/manage/AudioPool.cpp` | 8 | 17 | 32.00% |
| `include/ice/core/effect/filter/BiquadFilter.hpp` | 22 | 26 | 45.83% |
| `src/ice/core/effect/filter/BiquadFilter.cpp` | 21 | 49 | 30.00% |
| `include/ice/core/effect/GraphicEqualizer.hpp` | 75 | 51 | 59.52% |
| `src/ice/core/effect/GraphicEqualizer.cpp` | 82 | 189 | 30.26% |

复扫沿用 A4.2 重建范围：主仓和 ICE 共 1012 个有效文件，5 个空文件单列；712 个数值不足，其中主仓 685/921、ICE **27/91**。ICE 当前数值达标 **64/91**，本轮减少 7 个不足文件。全量明细和剩余清单仍为 `build/test_output/comment-audit/files.csv` 与 `under-30.csv`。本轮的比例及函数体复核只覆盖上表职责，不能把其余原有达标文件视作已全量质量验收。

验证：修改文件全部 clang-format；当前 12 个工作区 C++/头文件（含此前 ALPlayer 两个文件）与 ICE HEAD 去除注释和空白后文本一致。clang64 下 ICE 独立源码构建及 `IonTimeStretcherRealtimeTest`（1/1）通过。主项目仍保留 A4.2 的原子共享指针标准库兼容阻塞，未声称全项目构建通过。

均衡器专项测试 `Modules/Audio/tests/GraphicEqualizerRealtimeTest.cpp` 只在主项目源码依赖模式注册。本轮尝试用 clang64 直接将原测试链接当前 ICE 独立库，但测试自身第 287 行的 `std::aligned_alloc` 在本机 libc++/MinGW 环境不可用，编译未通过，不能报告该专项测试运行通过。未修改测试内存分配策略或切换工具链掩盖这一限制。

实现侧审查新增约束与独立修复候选：

- FFmpeg 实例构造仍有历史异常和未完整回滚的裸句柄；寻道未裁剪到样本精确位置；EOF 排尾未保留超出本次请求的重采样帧；负重采样返回值未单独报告。本次注释明确短读与初始化失败语义，不将它们描述为已修复。
- SDL `stop` 先清 running，再调用要求 running 为真的 `resume`，该调用实际不恢复设备；供数退出依靠主动轮询。暂停设备后供数仍可填到队列阈值，类也没有自动 close 的析构实现。旧的“恢复设备是退出关键”说明已重写。
- AudioPool 的文件状态签名不是内容哈希或原子文件快照；缓存命中不更换加载策略，COREAUDIO 仍无工厂实现。
- 均衡器的控制侧预构造避免音频侧分配，但 hazard 获取在持续发布下可能重试，不保证有界等待；新状态历史清零，没有系数渐变。双二阶滤波状态按声道独占且跨块保留。

剩余重点：`FFmpegFileReceiver.cpp`、`TimeStretcherRealtimeTest.cpp`、`src/main.cpp`、预编译布局及 Find/源码构建/跨编译脚本；随后逐个复核其余数值达标文件的质量。全量目标保持未完成，未提交、未推送。

## ICE 构建脚本审计推进记录（2026-09-08）

本批完成 10 个独立 Find 模块、3 个跨编译 shell 脚本、3 个源码构建脚本、架构探测及源码模式入口的注释复核。补充配置映射、平台库名兼容、静态链接闭包、运行时 DLL 检查、工具参数转换、临时归档生命周期、增量构建缓存及配置头生成约束。未改动命令参数或执行逻辑，未读取或修改外部第三方源码。

### 范围校正与逐文件结果

本次发现 ICE 自维护的 `3rdpty/sources/CMakeLists.txt` 被原第三方路径过滤规则漏计，已补入统计并完成注释审查。有效范围由 1012 增至 **1013**（主仓 921、ICE **92**），另有 5 个空文件不适用；没有将新增计数解释为新增业务代码。其余统计与排除口径沿用 A4.2，仍逐文件使用 cloc 2.08 的 `comment / (comment + code)`。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `3rdpty/cmake/modules/Findffmpeg.cmake` | 36 | 82 | 30.51% |
| `3rdpty/cmake/modules/Findfftw.cmake` | 15 | 35 | 30.00% |
| `3rdpty/cmake/modules/Findfmt.cmake` | 16 | 37 | 30.19% |
| `3rdpty/cmake/modules/Findlame.cmake` | 14 | 31 | 31.11% |
| `3rdpty/cmake/modules/Findlibsamplerate.cmake` | 16 | 37 | 30.19% |
| `3rdpty/cmake/modules/FindOpenAL.cmake` | 23 | 53 | 30.26% |
| `3rdpty/cmake/modules/Findrubberband.cmake` | 23 | 52 | 30.67% |
| `3rdpty/cmake/modules/FindSDL3Static.cmake` | 25 | 57 | 30.49% |
| `3rdpty/cmake/modules/Findspdlog.cmake` | 19 | 43 | 30.65% |
| `3rdpty/cmake/modules/Findzlib.cmake` | 17 | 38 | 30.91% |
| `3rdpty/sources/CMakeLists.txt` | 7 | 12 | 36.84% |
| `3rdpty/sources/cmake/Buildlibsamplerate.cmake` | 45 | 103 | 30.41% |
| `3rdpty/sources/cmake/Buildspdlog.cmake` | 12 | 28 | 30.00% |
| `3rdpty/sources/cmake/Buildzlib.cmake` | 44 | 101 | 30.34% |
| `3rdpty/sources/cmake/arch-probe.cmake` | 10 | 21 | 32.26% |
| `cmake/cross/clang-cl-gcc-compatible.sh` | 19 | 43 | 30.65% |
| `cmake/cross/llvm-lib-ar-compatible.sh` | 7 | 14 | 33.33% |
| `cmake/cross/merge-msvc-archives.sh` | 17 | 39 | 30.36% |

### 验证及尚未满足的门槛

- 所有修改的 CMake 文件已 cmake-format 并通过 `--check`。词法检查现已覆盖 `CMakeLists.txt`，剔除注释与空白后的 CMake token 序列与 ICE HEAD 一致；shell 非注释命令行一致，3 个脚本均通过 clang64 环境下 `bash -n`。
- 已有 12 个 C++/头文件再次通过去注释与空白的文本一致性检查。两层仓库 `git diff --check` 通过。
- 指定 clang64/fish 环境中，ICE 独立构建 `cmake --build C:/MusicMapMaker-Next/build/test_output/ice-comment-audit-build --parallel 12` 通过，`IonTimeStretcherRealtimeTest` 1/1 通过（最终一轮 1.23 秒）。本批 Find 修改触发重新配置成功，之后 Ninja 无待编译项。
- 独立构建保持 `SOURCES_BUILD=OFF`；源码模式脚本和跨编译包装只完成静态审查及语法/词法核验，未通过实际构建外部第三方来验证。shell 脚本没有执行归档覆盖或删除操作。
- 主项目完整构建仍有 A4.2 记录的 libc++ 原子共享指针兼容阻塞，均衡器专项测试仍有 `std::aligned_alloc` 兼容阻塞，均未声称已通过。

### 实现边界记录

- zlib 源码入口固定构建静态库，不随 `ICE_LINKAGE` 切换；源码戳只比较时间，不检测安装库删除、源码删除或保留 mtime 的替换，也不直接识别编译参数变化。多配置使用及路径中的 POSIX 单引号仍需独立审查。
- libsamplerate 探针使用通用 `HAVE_*` 缓存名；舍入探针临时设置 `CMAKE_REQUIRED_LIBRARIES` 后直接 unset，而非恢复父值；版本元数据固定为 0.1.9。注释没有将这些局限描述为已修复。
- 跨编译归档脚本的输出覆盖失败不恢复原归档；调用者必须隔离输入输出，并注意临时目录切换后的相对输入路径。当前合并方式也不能据此保证同一输入归档中的重复成员名完整保留。

当前 ICE 数值达标 **82/92**，不足 **10**；全项目不足 **695/1013**，其中主仓不足 685/921。其余原有达标文件仍需完成质量复核，不得将数字达标等同于全量审计通过。

ICE 剩余不足清单：

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `3rdpty/cmake/modules/PrebuiltLayout.cmake` | 33 | 421 | 7.27% |
| `3rdpty/sources/cmake/Buildffmpeg.cmake` | 24 | 307 | 7.25% |
| `3rdpty/sources/cmake/Buildfftw.cmake` | 11 | 98 | 10.09% |
| `3rdpty/sources/cmake/Buildlame.cmake` | 9 | 124 | 6.77% |
| `3rdpty/sources/cmake/Buildopenal.cmake` | 13 | 80 | 13.98% |
| `3rdpty/sources/cmake/Buildrubberband.cmake` | 26 | 342 | 7.07% |
| `3rdpty/sources/cmake/Buildsdl.cmake` | 11 | 145 | 7.05% |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 52 | 742 | 6.55% |
| `src/main.cpp` | 24 | 154 | 13.48% |
| `tests/TimeStretcherRealtimeTest.cpp` | 123 | 1047 | 10.51% |

全项目明细及其他不足文件逐项清单已刷新到 `build/test_output/comment-audit/files.csv` 和 `under-30.csv`；该构建目录中的产物仍会随清理丢失。任务目标保持未完成，未提交、未推送。

## ICE 预编译布局 helper 审计记录（2026-09-08）

本批完整复核 `3rdpty/cmake/modules/PrebuiltLayout.cmake` 的 16 个 helper，补充中文接口契约与实现附近说明。覆盖缓存默认值、目标平台与架构推断、编译器标签候选、配置映射、路径选择与失败返回、可选 PDB、运行时登记及构建后部署。修正原有“当前配置 DLL 清单”和“保证直接运行”等超出实现保证的说明，不改变任何配置命令或参数。

| ICE 相对路径 | 注释行：前 → 后 | 代码行：前 → 后 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `3rdpty/cmake/modules/PrebuiltLayout.cmake` | 33 → 182 | 421 → 422 | 7.27% → 30.13% |

代码行增加来自格式化换行，CMake 词法 token 序列与 ICE HEAD 一致。已执行 cmake-format 并通过 `--check`；全体工作区构建脚本词法/命令文本检查以及既有 12 个 C++ 文件去注释文本检查再次通过。

### 已验证的边界与未修复事项

- 标签回退仅按编译器主版本递减，不验证标准库、ABI 或运行库兼容；显式标签恰好等于当前 gccN/clangN 时仍会产生旧版本候选。
- 目录选择先标签后配置，较新标签的 Release 可以优先于较旧标签的 RelWithDebInfo。首个目录存在但内部缺库时，文件查找直接失败，不继续尝试其余目录。
- 库目录与 DLL 目录独立解析，文件存在不能证明两者版本配对；版本化 DLL 回退按字典序首项选择，不是选择语义版本最大值。
- PDB 查找是可选行为，缺文件不报错；仅 MSVC 且请求 Debug/RelWithDebInfo 时返回符号，Release 请求即使使用含符号目录也不进入该分支。现有逻辑不能替代打包符号完整性验收。
- DLL 登记使用不分配置的全局路径集合。不同目录同名文件都可留下，部署时可能覆盖；未检查间接 DLL 依赖。构建后复制不会因仅替换源 DLL 而保证重新执行。
- 默认架构使用 Apple arm64 或指针宽度推断，非 Apple 的其他 64 位 ISA 需要显式覆盖；默认缓存也不能代替换工具链时的干净配置。

在 `build/test_output/comment-audit/prebuilt-layout-probe.cmake` 增加并运行临时诊断探针，11 项断言全部通过：标签递减、未知版本回退、Debug 隔离、发布含符号优先、自定义配置去重、多配置/无配置输出、标签优先的目录选择、动态缺目录返回空、运行时按完整路径去重及静态登记无操作。测试目录仅位于 `build/test_output/comment-audit/layout-probe`，未读取第三方源码或操作真实预编译包。该探针不覆盖跨编译实际链接、DLL 配对、PDB 标识或构建后复制行为，不能用作这些行为已验证的证据。

指定 clang64/fish 环境下，ICE 独立构建重新配置成功并通过，`IonTimeStretcherRealtimeTest` 1/1 通过（1.31 秒）。仍保持 `SOURCES_BUILD=OFF`，不构建外部第三方、不更新预编译包。主项目原子共享指针及均衡器专项测试的标准库兼容阻塞保持前述记录，未声称解决。

全量逐文件复扫范围仍为 1013 个有效自维护文件及 5 个空文件；ICE 当前 **83/92** 数值达标、**9** 个不足，全项目仍有 **694/1013** 不足（主仓 685/921）。剩余 ICE 清单为上一节表格去除 `PrebuiltLayout.cmake` 后的 9 项；完整主仓与 ICE 不足清单已刷新到 `build/test_output/comment-audit/under-30.csv`。其他原有达标文件仍待质量复核，全量审计未完成，未提交、未推送。

## ICE 输出后端构建脚本审计记录（2026-09-08）

本批复核 OpenAL 与 SDL 的自维护源码构建入口，只修正和补充中文注释，不读取或修改上游第三方源码。说明配置缓存、静态/动态选择、平台功能裁剪、目标包装、系统依赖传播及构建与运行时验证的区别。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `3rdpty/sources/cmake/Buildopenal.cmake` | 13 → 35 | 80 | 13.98% → 30.43% |
| `3rdpty/sources/cmake/Buildsdl.cmake` | 11 → 63 | 145 | 7.05% → 30.29% |

两文件均已 cmake-format 并通过 `--check`，CMake 词法 token 与 ICE HEAD 一致；所有工作区脚本和既有 C++ 的注释外文本核验通过，两层 `git diff --check` 通过。指定 clang64/fish 下 ICE 独立构建成功，Ninja 无待编译项，`IonTimeStretcherRealtimeTest` 1/1 通过（1.28 秒）。仍使用 `SOURCES_BUILD=OFF`，因此本批源码模式脚本只完成静态审查及语法/词法核验，不声称实际构建了 OpenAL/SDL 或验证了设备播放。

审查确认的现有边界：

- OpenAL 后端裁剪条件为 `CMAKE_CROSSCOMPILING OR WIN32`，交叉到 Linux/macOS 也会关闭所列后端；后文链接 CoreAudio 框架不重新启用该后端。
- OpenAL 的非 Apple/Windows 分支只显式补 `dl`，没有显式添加 `pthread`，已修正旧注释。静态内部日志符号宏只向静态消费者传播。
- SDL 的后端 SHARED 选项表示动态加载服务库，与 SDL 自身库类型不同；选项为 ON 不能证明配置探测成功或运行时服务存在。
- 两脚本存在仅在某个平台分支强制写入、没有反向恢复的缓存选项，跨工具链复用构建目录可能残留旧值；注释要求隔离构建树，未改变现有配置行为。
- SDL 共享目标兼容分支按名字查找，不另行验证聚合目标的库类型；静态分支要求上游提供 `SDL3-static`。构建成功不能替代实际音频设备测试。

全量复扫仍覆盖 1013 个有效自维护文件及 5 个空文件，排除第三方、预编译、生成代码和纯数据。ICE 当前 **85/92** 数值达标，仍有 **7** 个不足；全项目不足 **692/1013**，其中主仓不足 685/921。主项目构建与均衡器专项测试的标准库兼容阻塞仍未解除。

剩余 ICE 比例不足清单：`Buildffmpeg.cmake`、`Buildfftw.cmake`、`Buildlame.cmake`、`Buildrubberband.cmake`（均位于 `3rdpty/sources/cmake`），以及 `src/ice/out/io/FFmpegFileReceiver.cpp`、`src/main.cpp`、`tests/TimeStretcherRealtimeTest.cpp`。它们的逐文件数值保持此前表格所列；全项目完整未达标清单已刷新至 `build/test_output/comment-audit/under-30.csv`，明细为 `files.csv`。其余数值达标文件仍需质量复核，全量审计未完成，未提交、未推送。

## ICE FFTW 构建脚本审计记录（2026-09-08）

本批完整复核自维护的 FFTW 构建入口，补充外部构建生命周期、源码时间戳的失效边界、平台产物名称、MSVC CRT 传递、线程子库与实际消费库的区别、交叉编译工具传递及安装产物依赖说明。仅注释和格式变化，不读取或修改 FFTW 上游源码。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `3rdpty/sources/cmake/Buildfftw.cmake` | 11 → 46 | 98 | 10.09% → 31.94% |

验证：cmake-format 与 `--check` 通过，CMake token 序列与 ICE HEAD 一致；全体工作区脚本及既有 C++ 注释外文本检查通过，两层 `git diff --check` 通过。指定 clang64/fish 下 ICE 独立构建成功、Ninja 无待编译项，实时测试 1/1 通过（1.26 秒）。独立构建仍使用 `SOURCES_BUILD=OFF`，因此没有实际构建 FFTW，不能把 ICE 回归测试当成该源码入口的跨平台构建验证。

记录但未修复的边界：非 MSVC 动态分支用共享库前后缀直接拼 lib 下路径，没有单独描述 MinGW 导入库与 bin 下 DLL；线程子库开关为 ON 不意味着包装目标链接线程子库；源码戳不检测安装库删除或参数变化；未提供 MSVC CRT 偏好时后备为静态 CRT，不单独根据 ICE_LINKAGE 推导；非 MSVC 分支未显式传递父级各配置 C flags。后续实际打包验证应逐项覆盖，不能从注释达标推断这些行为已正确。

全量 cloc 复扫沿用 1013 个有效自维护文件及 5 个空文件的范围，排除第三方、预编译、生成代码及纯数据。ICE 当前 **86/92** 数值达标，**6** 个仍不足；全项目不足 **691/1013**，主仓仍不足 685/921。全项目明细与其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。

剩余 ICE 不足文件为 `3rdpty/sources/cmake/Buildffmpeg.cmake`、`3rdpty/sources/cmake/Buildlame.cmake`、`3rdpty/sources/cmake/Buildrubberband.cmake`、`src/ice/out/io/FFmpegFileReceiver.cpp`、`src/main.cpp`、`tests/TimeStretcherRealtimeTest.cpp`。原有数值达标文件仍需质量复核；主项目构建与均衡器专项测试的兼容阻塞保持前述记录。全量目标未完成，未提交、未推送。

## ICE LAME 构建脚本审计记录（2026-09-08）

本批完整复核 LAME 自维护构建入口，补充 Autotools 参数环境、交叉 MSVC 包装器、静态归档合并、安装戳、构建并行度及稳定消费接口说明。只修改中文注释与格式，未读取或修改上游 LAME 源码。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `3rdpty/sources/cmake/Buildlame.cmake` | 9 → 54 | 124 | 6.77% → 30.34% |

cmake-format、`--check`、CMake token 序列一致性及两层 `git diff --check` 通过；既有工作区脚本和 C++ 的注释外文本检查再次通过。指定 clang64/fish 环境下 ICE 独立构建成功，Ninja 无待编译项，实时测试 1/1 通过（1.32 秒）。保持 `SOURCES_BUILD=OFF`，没有实际执行 LAME 源码构建或归档合并，不能报告其跨平台构建行为已验证。

确认但未修复的边界：当前入口固定静态 LAME；交叉 MSVC 分支整体覆盖前面继承的 C flags，固定 /MT 或 /MTd 及 x86_64 host；子 make 按 ProcessorCount 自选并行度，不受外层 Ninja 上限直接约束；源码时间戳不识别安装库删除或编译参数变化；Windows 配置阶段查找的 shell 不复用于后续直接调用 sh 的构建命令。以上已在对应实现附近说明，不将注释完成当作行为修复。

全量逐文件复扫仍包含 1013 个有效自维护文件及 5 个空文件，排除范围保持不变。ICE **87/92** 数值达标，仍有 **5** 个不足；全项目不足 **690/1013**（主仓 685/921）。完整统计与其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。

剩余 ICE 比例不足文件：`3rdpty/sources/cmake/Buildffmpeg.cmake`、`3rdpty/sources/cmake/Buildrubberband.cmake`、`src/ice/out/io/FFmpegFileReceiver.cpp`、`src/main.cpp`、`tests/TimeStretcherRealtimeTest.cpp`。原有达标文件仍待质量复核，主项目及均衡器专项测试兼容阻塞仍在；全量目标未完成，未提交、未推送。

## ICE 手工示例入口审计记录（2026-09-08）

本批复核 `src/main.cpp`，新增入口和演示 lambda 的中文 Doxygen，并在函数体内解释缓存策略、真实图连接、未使用节点、固定等待及失败退出语义。移除注释掉的旧调用片段，改用对当前行为的说明；未改变任何可执行代码。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `src/main.cpp` | 24 → 67 | 154 | 13.48% → 30.32% |

审查确认的现有局限：第二音源没有加入总线，压缩器也未作为输出根，因此当前输出不包含二轨混合或压缩保护；两个扫参 lambda 均未调用；TimeStretcher 没有在入口显式 prepare；设备 open 状态未检查，加载失败仍返回 0。固定输入为原开发机器路径，非 Apple 分支也不提供 Windows 资源选择。分离诊断线程按引用捕获局部变量，500ms 延时不保证对象存活或首块处理完成，短音轨情况下存在生命周期风险。这些仅说明，未修复；示例不能替代自动化回归用例或线程安全模板。

验证：clang-format、13 个工作区 C++/头文件去注释与空白后的文本一致性、两层 `git diff --check` 通过。指定 clang64/fish 中 ICE 独立构建重新编译并链接 `testIonCachyEngin.exe` 成功，实时测试 1/1 通过（1.54 秒）。没有运行该手工示例，不声称验证实体设备播放或分离线程安全。构建仍保持 `SOURCES_BUILD=OFF`，未构建外部第三方。

全量复扫仍覆盖 1013 个有效自维护文件及 5 个空文件，范围与排除口径不变；ICE 当前 **88/92** 数值达标，仍有 **4** 个不足，全项目不足 **689/1013**（主仓 685/921）。逐文件明细和其他不足清单已刷新到 `build/test_output/comment-audit/files.csv` 与 `under-30.csv`。

ICE 剩余不足文件为 `3rdpty/sources/cmake/Buildffmpeg.cmake`、`3rdpty/sources/cmake/Buildrubberband.cmake`、`src/ice/out/io/FFmpegFileReceiver.cpp`、`tests/TimeStretcherRealtimeTest.cpp`。其余数值达标文件仍需质量复核；主项目及均衡器专项测试兼容门槛仍未满足。全量审计未完成，未提交、未推送。

## ICE FFmpeg 构建入口审计记录（2026-09-08）

完整阅读并补注释复核自维护 FFmpeg 构建入口，覆盖宿主/目标平台区别、交叉包装器、依赖探测、编解码器与容器/解析器/位流过滤器清单、调试符号策略、配置哈希、安装清理及静态链接闭包。修正把静态归档后缀称为动态库后缀的旧说明，不改变配置参数或命令。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `3rdpty/sources/cmake/Buildffmpeg.cmake` | 24 → 133 | 307 | 7.25% → 30.23% |

验证：cmake-format 与 `--check` 通过，所有工作区构建脚本的 CMake token/命令文本一致性检查通过，既有 13 个 C++/头文件注释外文本检查通过，两层 `git diff --check` 通过。指定 clang64/fish 中 ICE 独立构建成功，Ninja 无待编译项，实时测试 1/1 通过（1.46 秒）。保持 `SOURCES_BUILD=OFF`，未执行 FFmpeg 外部源码构建、安装或其清理命令，未读取上游第三方源码。

确认但未修复的实现边界：

- 入口固定静态 FFmpeg；MSVC 固定 /MT 或 /MTd，不按父级 CRT 偏好推导。非 MSVC 不直接继承父级 C flags，但继承通用 EXE 链接 flags，父级需避免混入业务 PGO。
- 配置哈希只包含参数清单，不覆盖工具二进制、PATH/pkg-config 环境或 zlib/LAME 库内容。源码检查仅比较 mtime，不识别删除和安装库缺失。
- 参数清单最终逐项套双引号变成 sh 字符串，未完整转义内嵌 shell 特殊字符；列表形式不等于任意路径均安全。
- 缓存失效会清理私有安装前缀，失败不回滚旧安装；安装成功后清理 share 并更新戳。本批没有执行这些操作。
- 编解码与封装清单表示构建请求，不证明全部功能探测或媒体运行验证通过；file 协议与系统套接字链接项不能混为网络协议支持。
- 子 make 使用调用环境中的 PROCESSOR_COUNT，本文件没有验证它或使之等于外层并行度。

全量逐文件范围仍为 1013 个有效自维护文件及 5 个空文件，排除口径不变；ICE **89/92** 数值达标，仍有 **3** 个不足，全项目不足 **688/1013**（主仓 685/921）。完整明细及其他不足文件清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。

剩余 ICE 比例不足文件：`3rdpty/sources/cmake/Buildrubberband.cmake`、`src/ice/out/io/FFmpegFileReceiver.cpp`、`tests/TimeStretcherRealtimeTest.cpp`。其他数值达标文件仍需质量复核，既有主项目构建与均衡器专项测试兼容阻塞仍在。全量目标未完成，未提交、未推送。

## ICE Rubber Band 构建入口审计记录（2026-09-08）

完整复核自维护 Rubber Band 构建脚本，补充 Meson 类型映射、编译/链接参数、生成 pkg-config 元数据、CRT、交叉机器描述、安装产物及增量缓存的中文说明。未修改执行内容，也未读取或修改上游 Rubber Band 源码。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `3rdpty/sources/cmake/Buildrubberband.cmake` | 26 → 147 | 342 | 7.07% → 30.06% |

验证：重复格式化至稳定后 cmake-format `--check` 通过，CMake token 与 ICE HEAD 一致；所有工作区脚本及既有 13 个 C++/头文件注释外文本检查通过，两层 `git diff --check` 通过。指定 clang64/fish 下 ICE 独立构建成功，Ninja 无待编译项，实时测试 1/1 通过（1.29 秒）。保持 `SOURCES_BUILD=OFF`，本轮未执行 Meson 或外部第三方源码构建；只能据此报告注释/语法复核和 ICE 回归结果，不能报告该入口跨平台构建已经验证。

确认但未修复的边界：

- LTO 追加编译 flags，却覆盖之前的链接 flags，可能丢掉 target、sysroot 或运行库参数；脚本仍引用外层命名的 LTO 开关，独立项目变量归属需要另行治理。
- 生成的 pkg-config 版本固定为 FFTW 3.3.10 与 libsamplerate 0.1.9，不验证源码或归档版本；路径为当前构建绝对位置，不是可迁移发布元数据。
- 非 MSVC 的所有交叉分支都写固定 Windows x86_64 little-endian 机器描述；MSVC cross-file 路径则依赖顶层源码目录，native-file=NUL 还包含宿主平台假设。
- 非 MSVC 已有 build.ninja 时跳过 setup，源码戳也不覆盖参数、依赖库和安装库缺失；MSVC 分支不使用这个时间戳缓存，两者不能合并描述。
- 非 MSVC shared 路径没有单列 MinGW 导入库和 bin 下 DLL；构建产物列表也未完整描述旁路符号，不能代替预编译包完整性验收。
- `MESON_ENV`、`RUBBERBAND_BUILD_TYPE` 等历史变量没有进入最终对应命令，不能把它们的设置当作实际行为；shell 字符串和生成 INI 的特殊字符引用也仍有约束。

全项目逐文件复扫范围保持 1013 个有效自维护文件及 5 个空文件，排除范围不变。ICE 当前 **90/92** 数值达标、**2** 个不足；全项目不足 **687/1013**（主仓 685/921）。完整统计及其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。

剩余 ICE 比例不足文件为 `src/ice/out/io/FFmpegFileReceiver.cpp`（52 行注释、742 行代码，6.55%）与 `tests/TimeStretcherRealtimeTest.cpp`（123 行注释、1047 行代码，10.51%）。原有数值达标文件仍需质量复核；既有主项目构建和均衡器专项测试兼容门槛仍未满足。全量审计未完成，未提交、未推送。

## ICE FFmpeg 文件输出分段审计记录一（2026-09-08）

已完整阅读文件输出实现和公开头，本批先补充格式协商 helper、采样数组释放、配置访问、open/close/start/stop 生命周期部分。中文 Doxygen 与函数内说明强调优选 codec 回退、能力表不可用的尝试语义、采样率首项回退、声道数匹配而非声道身份匹配、输入进度与输出持久化的区别、停止与资源释放的同步边界。英文 helper 文档已译为中文，不改业务文本。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 | 状态 |
| --- | ---: | ---: | ---: | --- |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 52 → 140 | 742 | 6.55% → 15.87% | 分段进行中，未达 30% |

该文件尚未完成，后续继续处理编码器初始化、重采样输出、FIFO 分帧、发送取包与排尾；不能把本批记作整文件通过，也不能仅堆积函数外说明补齐比例。

已确认的接口边界：frames_written 是送入编码链路的累计输入帧，不是磁盘持久化帧数；采样率无法精确匹配时选能力表首项而非最近值；声道布局按数量匹配，不保证声道身份相同；running 的 load/store 不提供 start 的互斥获取，close 也不能与正在执行的编码循环并发。stop 只请求块边界取消，不能中断当前编码、文件 IO 或回调；取消/失败路径不保证补 trailer，也不删除部分输出。头文件相关接口说明仍需与后续整文件复核一起检查一致性，本批没有声称修复这些行为。

验证：clang-format、14 个工作区 C++/头文件去注释与空白文本一致性、两层 `git diff --check` 均通过。指定 clang64/fish 中已重新编译 FFmpegFileReceiver、链接 ICE 静态库与示例/测试，实时测试 1/1 通过（1.74 秒）。该测试不覆盖文件编码、取消输出或容器有效性，不能用作这些行为的运行验证；主项目完整构建与均衡器专项测试兼容阻塞仍在。

全量统计范围仍为 1013 个有效自维护文件及 5 个空文件，排除口径保持不变。ICE 仍为 **90/92** 数值达标、**2** 个不足；全项目仍 **687/1013** 不足（主仓 685/921）。另一个 ICE 不足文件为 `tests/TimeStretcherRealtimeTest.cpp`，仍为 123 行注释、1047 行代码、10.51%。最新逐文件明细与其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。全量目标保持进行中，未提交、未推送。

## ICE FFmpeg 文件输出分段审计记录二（2026-09-08）

继续完成编码器初始化、重采样、FIFO 分帧、结束排尾、编码包写入和错误保存的 Doxygen 与实现附近注释。说明输入/输出时间基、固定帧长与短尾处理、临时布局及样本所有权、错误不回滚、包接收终止条件和实际文件写入边界。没有改变任何业务表达式或控制流程。

| ICE 相对路径 | 注释行：本批前 → 后 | 代码行：前 → 后 | 注释率：前 → 后 |
| --- | ---: | ---: | ---: |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 140 → 321 | 742 → 744 | 15.87% → 30.14% |

代码行变化来自 clang-format 的换行；14 个工作区 C++/头文件去注释与空白后的文本均与 ICE HEAD 一致。clang-format、两层 `git diff --check` 通过。指定 clang64/fish 下重新编译输出实现并链接引擎、示例、测试成功，实时测试 1/1 通过（1.49 秒）。该测试不覆盖音频文件编码、取消文件有效性或高声道数；不能把它作为这些行为已验证的证据。

新增记录的现有约束与修复候选：初始 FIFO 块大小窄化及容量累加未完整检查整数上界；输出容量查询为零时直接返回且不调用转换器；固定长度编码器的尾帧不补零，由编码器接受或拒绝；FIFO 使用 frame->data 而非 extended_data，较多平面声道需要另行验证；样本出队与 PTS 推进后发送失败不回滚；send 的 EAGAIN 被当成失败，没有先 drain 再重试分支；排尾期间不检查 stop，不能保证及时取消。上述均未实施行为修复。

本实现的数值与实现说明复核已完成，但公开头 `include/ice/out/io/FFmpegFileReceiver.hpp` 仍需统一输入进度、持久化含义与跨线程调用契约，A5 批次暂不整体勾选。全量逐文件统计范围保持 1013 个有效自维护文件及 5 个空文件，排除口径不变；ICE **91/92** 数值达标，唯一比例不足为 `tests/TimeStretcherRealtimeTest.cpp`（123 行注释、1047 行代码，10.51%）。全项目不足 **686/1013**，其中主仓 685/921；完整明细及其他不足清单已刷新到 `build/test_output/comment-audit/files.csv`、`under-30.csv`。

其余原有数值达标文件仍需质量复核，主项目构建及均衡器专项测试兼容门槛仍未满足。全量审计目标保持进行中，未提交、未推送。

## ICE 文件输出公开契约一致性复核（2026-09-08）

本批对照完整实现更新 `FFmpegFileReceiver.hpp`，统一输入预算/进度与编码输出的单位区别、同步回调线程、取消请求、错误文本借用及资源回收约束。原先“close 总会写 trailer”及“运行状态 false 表示完成”的潜在误读已消除；同时修正实现与头中 start 的前置失败路径说明：前置检查失败直接返回，只有进入编码循环后才统一收尾关闭。没有修改接口签名、原子操作或执行逻辑。

| ICE 相对路径 | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/out/io/FFmpegFileReceiver.hpp` | 110 | 68 | 61.80% |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 323 | 744 | 30.27% |

验证：两文件均 clang-format；当前 15 个工作区 C++/头文件去注释与空白文本一致性通过；两层 `git diff --check` 通过。指定 clang64/fish 下重新编译接收端并链接 ICE、示例和测试成功，实时测试 1/1 通过（最终一轮 1.67 秒）。仍不将该测试解释为文件编码有效性、取消输出或高声道数覆盖；未执行外部第三方源码构建。

全量逐文件复扫范围保持 1013 个有效自维护文件及 5 个空文件，排除口径不变。ICE 仍 **91/92** 数值达标，唯一比例不足为 `tests/TimeStretcherRealtimeTest.cpp`（123 行注释、1047 行代码，10.51%）；全项目仍 **686/1013** 不足，主仓 685/921。最新明细与其他不足文件清单位于 `build/test_output/comment-audit/files.csv`、`under-30.csv`。下一步处理实时测试注释及其余原有达标文件质量复核，既有构建/专项测试兼容门槛仍未满足，全量审计未完成，未提交、未推送。

## ICE 实时测试分段审计记录一（2026-09-08）

补充 `tests/TimeStretcherRealtimeTest.cpp` 前半段的夹具契约、函数内实现说明和测试覆盖边界：固定容量边界脚本、单线程消费、借用 epoch 生命周期、release/acquire 通知、relaxed 观测计数、块内代际切换、分配统计窗口，以及状态切换、排尾、容量保护、暂停恢复和小数帧余量用例。只修改注释与格式，没有修改测试条件或业务行为。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 | 状态 |
| --- | ---: | ---: | ---: | --- |
| `tests/TimeStretcherRealtimeTest.cpp` | 123 → 204 | 1047 | 10.51% → 16.31% | 分段审计进行中，未达 30% |

本批明确了证据限制：线程局部分配统计不能证明其他线程或未接入钩子的入口没有堆操作；部分早期用例仅检查 C++ 计数；静音比较不拒绝 NaN；首声道极值与可闻帧统计不证明多声道一致或算法有效帧数；暂停用例为串行状态观察，不能代替设备及并发测试。上述是现有测试边界，不是本批修复完成的缺陷。后半段极端参数、边界用例、并发测试和分配钩子实现仍待继续复核。

验证：clang-format 完成；16 个工作区 C++/头文件去注释及空白后的文本与 ICE HEAD 一致；两层 `git diff --check` 通过。指定 clang64/fish 环境下重新构建 ICE 独立测试成功，`IonTimeStretcherRealtimeTest` 1/1 通过（2.22 秒）。主项目完整构建的 libc++ 原子共享指针阻塞和均衡器专项测试的 aligned_alloc 阻塞仍按前文记录，不作通过声明。

全量逐文件复扫保持 1013 个有效自维护文件、5 个空文件及既有排除口径；ICE 91/92 数值达标，唯一比例不足仍为上述测试文件。全项目不足 686/1013，其中主仓 685/921。完整逐文件数据及其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。全量审计目标仍进行中，未提交、未推送。

## ICE 实时测试分段审计记录二（2026-09-08）

继续补充极端状态、待发布状态与 final 排尾、起始延迟补偿、循环边界、provider Final、旁路和并发发布用例；为分配器包装、全局分配/释放入口和测试入口添加中文 Doxygen，并在关键实现附近说明计数及所有权边界。测试行为和断言未改变。

| ICE 相对路径 | 注释行：前 → 后 | 代码行 | 注释率：前 → 后 | 状态 |
| --- | ---: | ---: | ---: | --- |
| `tests/TimeStretcherRealtimeTest.cpp` | 204 → 321 | 1047 | 16.31% → 23.46% | 仍不足 30%，继续复核 |

进一步明确：极端参数不是完整组合覆盖；循环边界极值只能证明两侧信号出现，不能证明逐采样连续；Final 可闻帧上界不证明低幅度尾音未丢失；并发测试没有强制每次发布与音频读取重叠；C 包装依赖链接选项，不保证捕获共享库内部调用；realloc 计数为保守的调用记录，不等于实际搬迁/释放次数；普通 new 钩子未在此实现过对齐入口。部分 reset 用例及并发汇总仍未检查 C free，未通过修改测试掩盖这些覆盖边界。

验证：clang-format、16 个工作区 C++/头文件去注释与空白文本一致性、ICE `git diff --check` 通过。指定 clang64/fish 下 ICE 独立构建成功，实时测试 1/1 通过（2.32 秒）。全量复扫仍为 1013 个有效自维护文件和 5 个空文件；ICE 91/92 数值达标，唯一不足仍为此测试文件；全项目 686/1013 不足（主仓 685/921），最新明细及不足清单仍在 `build/test_output/comment-audit/files.csv`、`under-30.csv`。尚需测试文件整体质量和比例复核、其余原有达标文件质量复核；主项目构建及均衡器专项测试的既有兼容门槛未消除。全量目标保持进行中，未提交、未推送。

## ICE 测试说明校正与基类质量复核（2026-09-08）

对照 `TimeStretcher.cpp` 的 `roundedOutputFrames` 与终止段预算计算，修正测试中误写的 floor 说明：实现对累计输出预算四舍五入，37 / 0.75 在两种取整方式下恰好都得到 49，因此当前用例无法区分这两种规则。对照 `src/CMakeLists.txt` 及当前 Ninja 构建规则，确认 **Windows Clang64 未启用 ICE_TEST_WRAP_MALLOC 或 --wrap**；该配置中 C 堆计数为零没有提供 C 堆观测证据。此前实时测试通过记录只能按实际启用的入口解释，不能扩展为 C 堆也已覆盖。

同时完整阅读并复核 `IAudioNode.hpp`、`IEffectNode.hpp`、`IEffectNode.cpp`：补充接口析构、同步借用、单实例串行处理、无环上游链、无效准备、效果输出完整性和缓冲引用约束。实现侧的保护、预清零和失败静音说明已有覆盖，本批未修改实现。只读复核 `src/sample.cpp`，确认其已不再保留注释掉的旧示例，仍为未接入构建且不应调用的无返回占位；不能作为运行验证。上述四个文件的本次语义阅读为质量复核证据，不代表剩余 ICE 文件均已复核。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/core/IAudioNode.hpp` | 15 | 14 | 51.72% |
| `include/ice/core/effect/IEffectNode.hpp` | 44 | 40 | 52.38% |
| `tests/TimeStretcherRealtimeTest.cpp` | 325 | 1047 | 23.69%（未达标） |

三个文件均执行 clang-format，18 个工作区 C++/头文件去注释及空白文本与 ICE HEAD 一致。指定 clang64/fish 下重新构建受影响 ICE 源码、示例和测试成功，实时测试 1/1 通过（2.21 秒）。全项目逐文件复扫范围仍为 1013 个有效自维护文件与 5 个空文件，排除口径不变；ICE 91/92 数值达标，唯一不足仍为实时测试；全项目 686/1013 不足（主仓 685/921）。最新逐文件表及不足清单位于 `build/test_output/comment-audit/files.csv`、`under-30.csv`。主项目构建及专项测试兼容门槛仍未满足，全量审计继续进行，未提交、未推送。

## ICE 实时测试夹具观察口径复核（2026-09-08）

继续补充零预算区间消费、零帧拉取对首段序号的影响、统计帧与样本单位、标签借用，以及静音/极值/可闻帧观察的有效范围。未改变夹具或断言行为。

`tests/TimeStretcherRealtimeTest.cpp`：409 行注释、1047 行代码，**28.09%**，仍未达 30%。clang-format、41 个工作区 C++/头文件去注释与空白文本一致性通过；指定 clang64/fish 下正式测试构建成功，CTest 1/1 通过（2.20 秒）。全量复扫保持 1013 个有效自维护文件、5 个空文件；ICE 91/92 比例达标，全项目不足 686/1013，主仓 685/921。完整统计与其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。全量审计未完成，未提交、未推送。

## ICE 实时测试排尾与并发收尾复核（2026-09-08）

继续补充待发布状态接管与终态保持的组合断言、起始预填与延迟预算的单位区别、双声道偏移哨兵、并发线程的 TLS 汇总和停止/析构边界。明确最后一个发布状态不保证被消费、无启动握手可能使回调次数断言失败，以及部分末段只验证行为而未开启分配统计。未改变测试行为。

`tests/TimeStretcherRealtimeTest.cpp` 当前 391 行注释、1047 行代码，**27.19%**，仍未达 30%。clang-format、41 个工作区 C++/头文件去注释及空白文本一致性通过；指定 clang64/fish 下正式测试构建成功，CTest 1/1 通过（2.30 秒）。全量统计继续覆盖 1013 个有效自维护文件、5 个空文件；ICE 91/92 比例达标，全项目不足 686/1013，主仓 685/921，最新明细和其他不足清单已更新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。全量审计仍未完成，未提交、未推送。

## ICE 实时测试边界断言复核（2026-09-08）

补充容量保护、暂停恢复、小数预算、循环两侧、provider Final 和旁路终态的构造目的与断言边界。明确容量测试未设非零输出哨兵、暂停只检查最后块内容、最终累计预算不证明逐块分布，以及旁路“立即排空”标签实际在第二次 process 后观察状态。未修改断言或增加不真实的覆盖承诺。

本批修改 `tests/TimeStretcherRealtimeTest.cpp`：367 行注释、1047 行代码，**25.95%**，仍未达 30%。clang-format、41 个工作区 C++/头文件去注释与空白文本一致性通过；指定 clang64/fish 下正式测试构建成功，CTest 1/1 通过（2.56 秒）。全量复扫口径保持 1013 个有效自维护文件、5 个空文件；ICE 91/92 比例达标，全项目不足 686/1013（主仓 685/921），完整表及其他不足清单已刷新到 `build/test_output/comment-audit/files.csv`、`under-30.csv`。全量审计仍进行中，未提交、未推送。

## ICE 实时测试阶段契约补充（2026-09-08）

继续补充正式实时测试中参数连续发布、旁路排除、时间线借用、代际递增与终态断言的依据。极端用例说明三个 setter 并非整组原子提交，准备中间状态可混合新倍率和上一组参数；明确第三组倍率变更仍沿用 +24 半音与 Balanced。未修改断言、处理调用或统计窗口。

本批修改 `tests/TimeStretcherRealtimeTest.cpp`：注释由 325 增至 348 行，代码仍 1047 行，比例由 23.69% 增至 **24.95%**，尚未达到 30%。clang-format、41 个工作区 C++/头文件去注释及空白文本一致性、ICE diff 检查通过。指定 clang64/fish 下正式测试重新构建并运行，CTest 1/1 通过（2.48 秒）。原有 C 堆包装平台限制保持不变。

全量复扫仍为 1013 个有效自维护文件、5 个空文件，排除口径不变；ICE 91/92 数值达标，唯一不足仍为实时测试；全项目不足 686/1013，主仓 685/921。最新逐文件表及其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。全量质量复核、测试比例与既有主项目构建门槛尚未完成，未提交、未推送。

## ICE 混音与来源节点注释终核（2026-09-08）

重新完整对照 MixBus、SourceNode 的声明与实现及 IAudioNode 契约。混音头补充无环图与单来源串行处理前置条件、控制侧增删/清空的分配回收边界、每块路由读取的原子警告，以及左右静音模式互斥和读后写非事务。来源节点补充禁止复制/移动声明、暂停不等待在途处理、增益定义域、空回调/空音轨约束、时间转换前置条件和调度/定位多原子写的覆盖风险；移除未经证明的 lock-free 表述。实现中修正 hazard 对裸指针/引用捕获的生命周期保证，补充 provider 查询与重试的热路径说明。不改变算法、内存序或失败处理。

| ICE 相对路径 | 本批处理 | 注释行 | 代码行 | 注释率 |
| --- | --- | ---: | ---: | ---: |
| `include/ice/core/MixBus.hpp` | 修改注释 | 102 | 105 | 49.28% |
| `include/ice/core/SourceNode.hpp` | 修改注释 | 168 | 176 | 48.84% |
| `src/ice/core/SourceNode.cpp` | 修改注释 | 127 | 288 | 30.60% |
| `src/ice/core/MixBus.cpp` | 只读终核 | 115 | 262 | 30.50% |

三个修改文件 clang-format；44 个工作区 C++/头文件去注释与空白文本一致，ICE diff 检查通过。Clang64 以 12 并行重建混音、来源节点、引擎库和示例成功，CTest 1/1 通过（1.45 秒）。该实时变速测试不能代替 MixBus/SourceNode 专项运行覆盖，不据此声称并发定位等既有风险已修复。

验收索引为四个完整复核文件登记“注释终核完成”和 SHA-256 内容快照，后续据差异决定是否重审，不再仅以历史批次名称表示状态。其他文件仍按清单继续终核，整体目标未完成。全项目复扫范围保持 1013 个有效文件、5 个空文件；ICE 92/92 数值达标，2 空文件不适用；主仓仍有 685 个不足，明细见 `build/test_output/comment-audit/files.csv`、`under-30.csv`。未提交、未推送。

## ICE 顶层与目标构建说明终审（2026-09-08）

完整对照顶层、`src/CMakeLists.txt` 和依赖入口；核对共享导出宏的实际使用与安装规则。顶层修正“标准选择保证 ABI 可链接”、模块路径隔离、静态依赖可独立分发等过度承诺；说明链接参数按子串替换而非命令行分词，仅覆盖通用缓存项，不包含配置/目标级选项，且本入口未剥离继承的 PGO 参数。目标入口明确安装接口不等于安装规则、Windows 不启用 C 堆包装以及 CTest 未设置超时。依赖入口只读复核，不读取外部第三方实现。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `CMakeLists.txt` | 30 | 70 | 30.00% |
| `src/CMakeLists.txt` | 22 | 50 | 30.56% |

两个修改文件已 cmake-format；30 个修改的构建/工具脚本非注释语义文本与 HEAD 一致，41 个 C++/头文件去注释与空白文本一致。Clang64 独立 ICE 目录重新配置与构建成功，CTest 1/1 通过（1.33 秒）；ICE 和文档 diff 检查通过。未改变构建命令、开关默认值或运行时逻辑，未构建外部依赖源码。

新增 [ICE 注释审计验收索引](ice-comment-audit-index.md)，包含全部 94 个范围内文件的逐文件统计和职责复核记录定位，并分列数值、质量、格式、构建、测试和交付门槛。表中记录定位不等同于最终质量通过，仍须核对其证据覆盖当前文件，不能把索引存在当成终审完成。

全项目复扫保持 1013 个有效自维护文件及 5 个空文件；ICE 92/92 数值达标，2 个空文件不适用，主仓仍有 685 个不足。完整与不足清单仍为 `build/test_output/comment-audit/files.csv`、`under-30.csv`。主项目 Clang64 构建门槛和其余证据终核未完成，目标保持进行中，未提交、未推送。

## ICE 交叉工具链注释质量复核（2026-09-08）

完整复核 `cmake/toolchain-macos-to-windows.cmake`，保留原有工具链及路径设置，仅重写注释。移除英文分隔标题和注释掉的窗口链接参数，解释宿主工具/目标库分离、固定 x86_64 与开发机绝对系统根、普通变量覆盖及 ABI 不校验。根据 [CMake 查找文档](https://cmake.org/cmake/help/latest/command/find_package.html) 修正“所有 Find 模块只能在系统根中找到”和“保证没有宿主依赖泄漏”的过度表述：模块加载与配置模式包查找不是同一过程，显式路径及调用参数仍可绕开默认根路径模式。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `cmake/toolchain-macos-to-windows.cmake` | 11 | 15 | 42.31% |

文件已 cmake-format；28 个修改的构建/工具脚本非注释语义文本与 HEAD 一致，41 个 C++/头文件去注释及空白文本一致，ICE diff 检查通过。`cmake -P` 解析执行成功，只验证设置语句和输出，不探测 macOS 编译器或进行交叉链接。Clang64 独立 ICE 构建成功（无待编译项），CTest 1/1 通过（1.27 秒），不能将本机构建当作 macOS 交叉编译验证。

全量复扫仍包含主仓与 ICE 自维护代码、构建及工具脚本，共 1013 个有效文件及 5 个空文件，排除口径不变；ICE 92/92 数值达标，另外 2 个空文件不适用。主仓 685 个不足文件保留在 `build/test_output/comment-audit/under-30.csv`，完整数据为 `files.csv`。剩余需将全部文件的质量证据逐一与清单对齐，并复查顶层/目标构建说明及验证门槛，不将已知未修复行为或未运行的平台分支标为通过。目标保持进行中，未提交、未推送。

## ICE 小型构建入口与历史错误类型质量复核（2026-09-08）

通读三个 `include/ice/execptions/` 头并核对 AudioBuffer、CachyDecoder、FFmpeg 的实际使用点，确认现有说明对应历史失败通道，类型没有额外数据成员；只读复核，不调整异常策略。另通读顶层、src、依赖入口和小型构建脚本，发现数值达标仍未解决的浅层或错误说明，按职责先完成下列两文件。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `cmake/configure-project.cmake` | 4 | 7 | 36.36% |
| `3rdpty/sources/cmake/Buildfmt.cmake` | 8 | 17 | 32.00% |

指令集入口移除注释掉的 AVX/消毒器配置，解释目录作用域、空平台分支及无条件 AVX2 的部署约束。fmt 入口替换 IDE 显示控制、强制相同标准及固定 `-fPIC` 等不准确表述，说明缓存覆盖、依赖触发构建、标准版本继承和平台相关 PIC。未读取 fmt 第三方源码、未修改命令。

两文件 cmake-format；27 个修改的构建/工具脚本非注释语义文本与 HEAD 一致，41 个 C++/头文件去注释与空白文本一致，ICE diff 检查通过。Clang64 独立目录重新配置及构建成功，CTest 1/1 通过（1.26 秒）。当前 `SOURCES_BUILD=OFF`，未执行 fmt 源码构建，不将此构建结果当作该依赖分支运行验证。

全量统计仍为 1013 个有效文件和 5 个空文件；ICE 92/92 数值达标、2 个空文件不适用；其他不足均在主仓，共 685 个，清单见 `build/test_output/comment-audit/under-30.csv`。继续保留 macOS 交叉工具链脚本的绝对化查找保证等说明复核，以及全量证据归档和主项目验证门槛；目标未完成，未提交、未推送。

## ICE 实时测试注释收尾与数值门槛清零（2026-09-08）

完成测试入口、普通及带大小分配/释放替换函数、TLS 统计、脚本输入单位及观察助手的契约说明。修正数组释放说明：元素析构次数不计入最终存储释放次数，但析构函数内部若调用已接入的堆入口，仍可能被全局钩子观察。补充空声道指针、极值输出别名与 NaN/无穷大判定的前置条件和覆盖边界。只修改中文注释，不改变用例、同步或断言。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `tests/TimeStretcherRealtimeTest.cpp` | 449 | 1047 | 30.01% |

再次只读确认 `src/sample.cpp` 是未接入构建的空历史入口，不含废弃注释代码，不能用作文件解码验证。A6 的注释工作据此完成。

验证：测试文件 clang-format；41 个工作区 C++/头文件去注释与空白文本一致；ICE `git diff --check` 通过。指定 clang64/fish 环境以 12 并行构建独立 ICE 审计目录成功，CTest `IonTimeStretcherRealtimeTest` 1/1 通过（2.49 秒）。Windows 配置未启用 C 分配链接包装，不能宣称 C 堆零分配已被证明；测试也不覆盖所有设备后端、文件解码和效果器。

全项目复扫包含 1013 个有效自维护文件及 5 个空文件，排除外部第三方、预编译/生成目录和纯数据，保留自维护构建脚本及资源目录中的程序脚本。ICE 92/92 有效文件数值达标，另 2 个空文件不适用；主仓仍有 685/921 个不足，因此全项目仍有 685/1013 个不足。完整逐文件数据及其他不足清单为 `build/test_output/comment-audit/files.csv`、`under-30.csv`。

剩余验收：逐一归档全量质量复核证据、核对热路径说明及已有验证门槛。主项目 Clang64 原子共享指针兼容问题仍未修复，既有行为缺陷也未按注释任务扩大修改。数值清零不等于所有审计通过，目标保持进行中，未提交、未推送。

## ICE 极端参数扩容警告定位（2026-09-08）

本轮针对上次日志中的 RubberBand 扩容警告完成只读源码分析和隔离运行诊断，不修改任何已跟踪测试或业务源码，也不读取第三方源码。构建目录新增临时诊断入口 `build/test_output/comment-audit/realtime-case-probe.cpp`，沿用 Ninja 中原测试的 Clang64 编译选项及库依赖，逐进程调用原有 12 个测试函数：全部返回零，只有 `testExtremePreparedStates` 输出该警告。

为细分阶段，在 `realtime-phase-snapshot.cpp` 保存当前原测试的诊断副本，仅在极端用例的倍率、音高、质量、首次处理、重置请求、重置处理前加入可选终止检查点，未改原测试。检查点 0 至 12 的执行前缀无警告，13 首次出现同一扩容警告。检查点 12 是第三组参数的 `set_playback_ratio(0.05)` 之前，13 是其之后、`set_pitch_semitones` 之前；此时沿用前组的 +24 半音和 Balanced 质量。将终止改为 `_Exit` 跳过析构，再次运行 12/13 得到相同结果，排除是局部对象收尾产生警告。

因此已确认本次警告发生于第三组倍率 setter 内，**在堆统计窗口和该组 process 之前**，并非本次回调内扩容证据。自有代码中该 setter 会调用 `publish_prepared_state`，后端包装构造包含预热，与控制侧准备行为相符。仍不能据此证明所有回调无 C 堆分配：当前 Windows 测试没有启用 C 堆包装，既有覆盖限制保持不变。

临时诊断副本的提前退出仅用于阶段定位，不是完整用例通过证据；12 个原函数的独立完整执行结果与阶段前缀结果已明确区分。临时文件位于构建输出目录，排除于自维护逐文件统计，清理构建目录会移除；没有改 CMake、正式测试断言或第三方库。上一轮官方 CTest 仍为 1/1 通过（2.49 秒）。本轮仅更新诊断证据与文档，原逐文件统计不变：ICE 91/92 比例达标，实时测试 325/1047 行、23.69%，全项目不足清单仍见 `build/test_output/comment-audit/under-30.csv`。全量审计仍未完成，未提交、未推送。

## ICE 音频缓冲质量复核（2026-09-08）

完整复核 AudioBuffer 两个平台分支与移动实现，补充活动范围扩大不清零、Linux clear_from 不清对齐尾部、尺寸算术未完整防溢出、扩容失败不整体回滚，以及复制禁用和移动所有权契约。移除 Linux 历史 SIMD 转换段未经完整验证的中间结果推断，保留明确的四帧步长与八浮点写入冲突警告。未修改样本运算或内存管理行为。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioBuffer.hpp` | 125 | 283 | 30.64% |
| `src/ice/manage/AudioBuffer.cpp` | 41 | 94 | 30.37% |

两文件 clang-format，41 个工作区 C++/头文件去注释和空白文本一致性通过，ICE diff 检查通过。指定 clang64/fish 下重建当前引擎、示例及测试成功，实时测试 1/1 通过（2.49 秒）。Windows 构建只覆盖非 Linux 缓冲分支，不能证明 Linux SIMD 路径可用。

本轮 LastTest.log 同时输出 RubberBand 输出缓冲被迫从 131072 增至 262144 的警告。日志没有标明发生于哪个测试统计窗口，不能推断是准备阶段还是回调阶段；结合 Windows 未启用 C 堆包装，此次测试通过不能作为整个后端零扩容的证明。需在后续测试质量复核中定位，未擅自改测试或后端来掩盖警告。

全量复扫仍为 1013 个有效自维护文件、5 个空文件，排除口径不变；ICE 91/92 数值达标，实时测试仍为唯一不足（325/1047 行，23.69%）；全项目不足 686/1013，主仓 685/921。完整表及其他不足清单已刷新到 `build/test_output/comment-audit/files.csv`、`under-30.csv`。质量复核、测试比例和既有构建门槛尚未全部完成，未提交、未推送。

## ICE 接收端与播放通知契约复核（2026-09-08）

完整复核 IReceiver 头与构造实现、PlayCallBack 头，核对 SourceNode 注册与实际通知调用。补充基类不保存格式、默认析构不调用 close、后端 start/stop 同步性不同、get_source 引用不额外保活等边界。播放完成通知表示输入末尾，不是设备尾音播放完成；帧进度是下一次读取游标，循环时可归零；纳秒仅为时间表示单位。两份空头 ALSource.hpp 和 IEncoder.hpp 保持不适用，不计为达标文件。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/core/PlayCallBack.hpp` | 18 | 17 | 51.43% |
| `include/ice/out/IReceiver.hpp` | 28 | 30 | 48.28% |

两文件 clang-format，39 个工作区 C++/头文件去注释和空白文本一致性通过，ICE diff 检查通过。指定 clang64/fish 下受影响引擎、示例和测试构建成功，实时测试 1/1 通过（1.29 秒）；不扩展为实体设备停机或通知线程专项测试。未修改执行逻辑。

全量复扫仍为 1013 个有效自维护文件、5 个空文件，排除口径不变；ICE 91/92 数值达标，实时测试仍为唯一不足（325/1047 行，23.69%）；全项目不足 686/1013，主仓 685/921。完整逐文件数据及其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。其余质量审计、测试比例及既有构建门槛未完成，未提交、未推送。

## ICE 配置与音频格式质量复核（2026-09-08）

完整复核配置头、配置定义和 AudioFormat，并检索配置消费位置。补充构造时快照与 SourceNode 处理时读取全局值并存的约束，明确运行期间必须保持配置稳定；默认块长的零值保护因消费端而异。格式类型补充公开字段无校验、声道数量不等于布局身份等边界，不改变默认值或运行行为。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/config/config.hpp` | 18 | 27 | 40.00% |
| `include/ice/manage/AudioFormat.hpp` | 9 | 12 | 42.86% |

两文件 clang-format，37 个工作区 C++/头文件去注释及空白文本一致性通过。指定 clang64/fish 下重建受影响引擎源码、示例和测试成功，实时测试 1/1 通过（2.37 秒）。未将测试解释为运行中更改全局配置安全，此行为仍不受支持。

全量复扫保持 1013 个有效自维护文件、5 个空文件及既有排除口径；ICE 91/92 比例达标，实时测试仍为唯一不足（325/1047 行，23.69%），全项目不足 686/1013，主仓 685/921。完整逐文件数据及其他不足清单已刷新到 `build/test_output/comment-audit/files.csv`、`under-30.csv`。剩余质量复核、测试文件比例和既有主项目构建门槛未完成，未提交、未推送。

## ICE 封面所有权质量复核（2026-09-08）

完整核对 `AlbumArt.hpp` 与 FFmpeg 封面复制调用，补充公开裸指针的所有权不变量、复制失败状态、移动前后借用地址与目标旧视图失效，以及 isValid 只检查非空的边界。替换旧的“窃取资源”等浅层说明，不修改数组分配、释放或异常行为。

本批修改文件 `include/ice/manage/dec/AlbumArt.hpp`：36 行注释、52 行代码，40.91%。clang-format、35 个工作区 C++/头文件去注释及空白文本一致性、ICE diff 检查通过。指定 clang64/fish 下受影响引擎、示例及实时测试构建成功，CTest 1/1 通过（1.36 秒）；不代表封面分配失败、错误别名或图像格式有效性已作专项运行验证。

全量复扫范围不变：1013 个有效自维护文件和 5 个空文件；ICE 91/92 数值达标，实时测试仍为唯一不足（325/1047 行，23.69%）；全项目不足 686/1013，主仓 685/921。最新完整明细及其他不足清单已更新到 `build/test_output/comment-audit/files.csv`、`under-30.csv`。其余质量审计、测试比例及既有构建门槛继续保留，未提交、未推送。

## ICE 工厂、实例与媒体信息契约复核（2026-09-08）

复核 `IDecoderFactory.hpp`、`IDecoderInstance.hpp`、`MediaInfo.hpp`，并对照 FFmpeg 工厂头、创建实现及格式/长度查询。修正工厂基类“失败仅返回空”的错误承诺，现明确历史实现还可抛出异常；补充实例定位精度、源/输出格式区别、帧单位与读取阻塞边界，以及元信息标量默认初始化不赋初值的约束。未改变代码行为。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/dec/IDecoderFactory.hpp` | 15 | 22 | 40.54% |
| `include/ice/manage/dec/IDecoderInstance.hpp` | 19 | 18 | 51.35% |
| `include/ice/manage/dec/MediaInfo.hpp` | 12 | 15 | 44.44% |

三个头文件 clang-format，34 个工作区 C++/头文件去注释与空白文本一致性通过，ICE diff 检查通过。指定 clang64/fish 下受影响引擎、示例及测试构建成功，实时测试 1/1 通过（1.44 秒）；不将其解释为工厂失败或文件解码专项覆盖。

全量复扫仍覆盖 1013 个有效自维护文件及 5 个空文件，ICE 91/92 比例达标，唯一不足为实时测试（325/1047 行，23.69%）；全项目不足 686/1013，主仓 685/921。最新逐文件统计及其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。其余质量复核、测试比例及既有主项目构建门槛未完成，未提交、未推送。

## ICE 独立变调与限幅占位质量复核（2026-09-08）

完整阅读 `PitchAlter.hpp/.cpp` 与 `Clipper.hpp/.cpp`。变调接口补充倍率定义域、半音换算、默认顺序一致原子读写、重复上游共享句柄读取和覆盖基类保护的边界；说明近单位倍率旁路使用累加、保留旧后端历史，首次后端创建未按当前块长重新准备。只读确认 Clipper 的头和实现已明确空处理不裁剪也不直通，本批不修改该占位类型，不把类型存在当作限幅功能通过。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/core/effect/PitchAlter.hpp` | 37 | 33 | 52.86% |
| `src/ice/core/effect/PitchAlter.cpp` | 21 | 28 | 42.86% |

两文件 clang-format，31 个工作区 C++/头文件去注释及空白文本一致性通过，ICE diff 检查通过。指定 clang64/fish 下重建当前引擎、示例和测试成功，实时测试 1/1 通过（1.45 秒），不覆盖 PitchAlter 的旁路切换或 Clipper 功能。原有热路径扩容、共享复制、状态延续和参数校验问题只记录未修复。

全量复扫范围不变：1013 个有效自维护文件、5 个空文件；ICE 91/92 数值达标，实时测试仍为唯一不足（325/1047 行，23.69%）。全项目 686/1013 不足，主仓 685/921；明细与其他不足清单已更新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。剩余质量复核、测试比例及主项目构建门槛未完成，未提交、未推送。

## ICE 压缩效果器质量复核（2026-09-08）

完整复核 `Compresser.hpp/.cpp`，修正“上升一定更快”等不成立的参数假设，明确 dB 域平滑、各声道独立包络、零 dB 初值、resize 保留已有状态、补偿缓存未参与实际处理和最终输出不限幅。补充原子参数写入者/读取者、默认顺序一致及非原子脏标记约束，不改变同步操作或算法。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/core/effect/Compresser.hpp` | 39 | 53 | 42.39% |
| `src/ice/core/effect/Compresser.cpp` | 29 | 54 | 34.94% |

现有风险未修复：更新分支在音频调用栈中可能扩容；单独改变声道数不触发包络 resize；参数不校验有限性与范围；补偿逐样本原子读取；格式不匹配时直接保留输出；不存在立体声联动或最终限幅。注释复核不代表这些行为已满足实时规范。

两文件 clang-format，29 个工作区 C++/头文件去注释及空白文本一致性通过，ICE diff 检查通过。指定 clang64/fish 下重建引擎、示例及测试成功，实时测试 1/1 通过（1.37 秒）；此测试不覆盖 Compressor 音频结果或参数切换。

全量复扫口径仍为 1013 个有效自维护文件及 5 个空文件；ICE 91/92 数值达标，实时测试仍为唯一不足（325/1047 行，23.69%）。全项目 686/1013 不足，主仓 685/921，完整明细及其他不足清单已更新到 `build/test_output/comment-audit/files.csv`、`under-30.csv`。剩余质量复核、测试比例与既有构建门槛未完成，目标保持进行中，未提交、未推送。

## ICE 音轨封装质量复核（2026-09-08）

完整对照 `AudioTrack.hpp/.cpp`，修正“绝对路径”的旧说明，实际为原样保存输入字符串；补充策略枚举、构造前置条件、源元信息与输出格式区别、read 缓冲容量、origin 浮点转整数及借用生命周期。非法策略会留下空解码器、origin 丢弃读取帧数且不检查转换范围等现有边界均已明确，未修改行为或接口。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/AudioTrack.hpp` | 42 | 52 | 44.68% |
| `src/ice/manage/AudioTrack.cpp` | 23 | 41 | 35.94% |

两文件 clang-format，27 个工作区 C++/头文件去注释及空白文本一致性通过，ICE diff 检查通过。指定 clang64/fish 下重建受影响引擎、示例及实时测试成功，CTest 1/1 通过（1.38 秒），不将此视为文件解码或 origin 边界专项验证。

全量统计范围不变：1013 个有效自维护文件、5 个空文件；ICE 91/92 数值达标，唯一不足仍为实时测试（325/1047 行，23.69%）；全项目不足 686/1013，主仓 685/921。完整统计及不足文件清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。其余质量复核、测试比例和既有主项目构建门槛仍未完成；未提交、未推送。

## ICE 解码接口与流式占位质量复核（2026-09-08）

完整核对 `IDecoder.hpp`、`StreamingDecoder.hpp/.cpp`，对照缓存实现统一返回值与未覆盖声道的表述，补充准备等待、借用视图与容量契约。流式策略当前确为占位：没有文件探测、定位、预读或状态保存，所有读取返回零；创建对象仍分配并复制共享句柄。注释明确这些现状，不实现新的流式功能，也不把非空对象或正常返回当成可播放证明。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/dec/IDecoder.hpp` | 26 | 21 | 55.32% |
| `include/ice/manage/dec/StreamingDecoder.hpp` | 21 | 27 | 43.75% |
| `src/ice/manage/dec/StreamingDecoder.cpp` | 16 | 28 | 36.36% |

三个文件 clang-format；25 个工作区 C++/头文件去注释及空白文本一致性通过；ICE diff 检查通过。指定 clang64/fish 下重建受影响引擎、示例及实时测试成功，CTest 1/1 通过（1.31 秒）。测试不覆盖流式播放，也不能证明尚未实现的流式功能完成。

全量逐文件复扫继续覆盖 1013 个有效自维护文件与 5 个空文件，排除口径不变；ICE 91/92 比例达标，唯一不足为实时测试（325/1047 行，23.69%）；全项目不足 686/1013，其中主仓 685/921。完整表及其他不足清单已刷新至 `build/test_output/comment-audit/files.csv`、`under-30.csv`。其余质量复核、测试文件比例和既有主项目构建门槛仍未完成；未提交、未推送。

## ICE 缓存解码质量复核（2026-09-08）

完整对照 `CachyDecoder.hpp/.cpp`，替换逐句复述的旧注释，补充后台任务捕获与取消边界、实例格式、容量预估、read 返回约束、call_once 的唯一结果提取、std::exception 失败缓存、声道映射及 span 借用契约。修正 decode 返回值说明：零目标声道仍可返回非零切片长度，不能将返回值当成所有目标声道已写入的证明。未改变业务表达式、异常策略或控制流程。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/manage/dec/CachyDecoder.hpp` | 43 | 41 | 51.19% |
| `src/ice/manage/dec/CachyDecoder.cpp` | 66 | 126 | 34.38% |

既有行为限制仍保留：非空工厂为前置条件；整文件缓存没有应用层内存上限；创建强制 seek(0)；read 返回零不区分 EOF 与失败；多声道裁剪不是混音；首次 get_data 可阻塞；origin 可能扩容且不保活解码器。历史异常和原始分配仍未按注释任务擅自替换，不能据此声称全部行为规范通过。

两文件 clang-format，22 个工作区 C++/头文件去注释和空白文本一致性通过，ICE diff 检查通过。指定 clang64/fish 下重建当前 ICE 源码、示例及测试成功，实时测试 1/1 通过（1.34 秒）；该测试不覆盖缓存文件解码及失败资源，本批不作专项运行通过声明。

全量复扫保持 1013 个有效自维护文件、5 个空文件及既有排除口径；ICE 91/92 数值达标，唯一不足仍为实时测试（325 注释行、1047 代码行、23.69%）。全项目不足 686/1013，主仓 685/921；最新逐文件统计及其他不足清单为 `build/test_output/comment-audit/files.csv`、`under-30.csv`。全量质量审计和既有构建门槛仍未完成，未提交、未推送。

## ICE 线程池与分配跟踪质量复核（2026-09-08）

完整阅读 `ThreadPool.hpp`、空实现编译入口 `ThreadPool.cpp`、`AllocationTracker.hpp`，并核对缓存解码的任务提交与示例包含位置。线程池原有高比例注释大量逐句复述语法且引用错误的 stop_/tasks_ 成员名，现重写为中文 Doxygen 和实现附近的队列协议、所有权、停止排空及并发约束。分配跟踪工具补充统计扰动、全进程入口范围、原子读取/复位以及历史异常和输出接口边界。未改变运行逻辑。

| ICE 相对路径（本批修改） | 注释行 | 代码行 | 注释率 |
| --- | ---: | ---: | ---: |
| `include/ice/thread/ThreadPool.hpp` | 54 | 97 | 35.76% |
| `include/ice/tool/AllocationTracker.hpp` | 35 | 40 | 46.67% |

现有行为限制：线程数下限实际为四；队列无界且不支持取消；析构先排空任务再 join，没有截止时间；不能从本池任务销毁池，也不能依赖 stop 检查使析构与提交并发安全；同池嵌套 future 等待可能耗尽工作线程；普通任务异常没有 packaged_task 结果通道。分配工具仍包含历史 throw 和 cout，原子计数自身也会影响被测时延。这些未按注释任务擅自改为另一错误策略或调度实现，不能宣称通过所有行为规范检查。

验证：两文件 clang-format；20 个工作区 C++/头文件去注释和空白文本一致性通过；ICE `git diff --check` 通过。指定 clang64/fish 环境重新编译受影响引擎与示例成功，实时测试 1/1 通过（1.30 秒）；该测试不覆盖线程池任务失败/停机或该全局分配跟踪器，构建成功不代替这些运行验证。

全量逐文件复扫口径不变：1013 个有效自维护文件、5 个空文件，ICE 91/92 数值达标；唯一不足为实时测试的 325 行注释、1047 行代码、23.69%。全项目不足 686/1013，其中主仓 685/921，其他不足清单及完整明细已刷新至 `build/test_output/comment-audit/under-30.csv`、`files.csv`。仍需继续测试文件与其余质量复核，既有主项目构建/专项测试兼容门槛不变，目标未完成，未提交、未推送。
