# 逐文件注释审查与分批任务

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
- [ ] A4：ICE `SourceNode.cpp`、`ALPlayer.cpp` 按各自职责分批处理，补来源切换、队列与播放状态。
- [ ] A5：ICE `FFmpegFileReceiver.cpp`、`FFmpegDecoderInstance.cpp` 分批处理，补解码/编码状态、重采样、EOF 和回收。
- [ ] A6：ICE `TimeStretcherRealtimeTest.cpp`；独立复核 `sample.cpp` 的废弃代码，不以高比例直接验收。
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
- [ ] `src/ice/out/play/openal/ALPlayer.cpp`：6.79%。
- [x] `src/ice/core/effect/TimeStretcher.cpp`：5.88% → 30.04%，A3 完成。
- [ ] `src/ice/out/io/FFmpegFileReceiver.cpp`：6.55%。
- [ ] `3rdpty/cmake/modules/PrebuiltLayout.cmake`：7.27%。
- [ ] `3rdpty/sources/cmake/Buildrubberband.cmake`：7.07%。
- [ ] `3rdpty/sources/cmake/Buildffmpeg.cmake`：7.25%。
- [ ] `src/ice/core/effect/GraphicEqualizer.cpp`：1.05%。
- [ ] `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp`：20.89%。
- [ ] `3rdpty/sources/cmake/Buildsdl.cmake`：7.05%。
- [ ] `3rdpty/sources/cmake/Buildlame.cmake`：6.77%。
- [ ] `src/main.cpp`：13.48%。
- [ ] `3rdpty/sources/cmake/Buildzlib.cmake`：2.88%。
- [ ] `3rdpty/sources/cmake/Buildlibsamplerate.cmake`：3.81%。
- [ ] `3rdpty/cmake/modules/Findffmpeg.cmake`：1.16%。
- [ ] `src/ice/out/play/sdl/SDLPlayer.cpp`：18.50%。
- [ ] `3rdpty/sources/cmake/Buildfftw.cmake`：10.09%。
- [ ] `3rdpty/cmake/modules/FindSDL3Static.cmake`：3.39%。
- [ ] `3rdpty/sources/cmake/Buildopenal.cmake`：13.98%。
- [ ] `3rdpty/cmake/modules/Findrubberband.cmake`：1.89%。
- [ ] `3rdpty/cmake/modules/FindOpenAL.cmake`：5.26%。
- [ ] `3rdpty/cmake/modules/Findspdlog.cmake`：4.44%。
- [ ] `cmake/cross/clang-cl-gcc-compatible.sh`：4.44%。
- [ ] `include/ice/manage/AudioPool.hpp`：24.06%。
- [ ] `3rdpty/cmake/modules/Findzlib.cmake`：2.56%。
- [ ] `cmake/cross/merge-msvc-archives.sh`：4.88%。
- [ ] `3rdpty/cmake/modules/Findfmt.cmake`：2.63%。
- [ ] `3rdpty/cmake/modules/Findlibsamplerate.cmake`：2.63%。
- [ ] `3rdpty/cmake/modules/Findfftw.cmake`：2.78%。
- [ ] `src/ice/core/effect/filter/BiquadFilter.cpp`：14.04%。
- [ ] `3rdpty/cmake/modules/Findlame.cmake`：3.13%。
- [ ] `src/ice/manage/AudioPool.cpp`：9.09%。
- [ ] `include/ice/out/play/sdl/SDLPlayer.hpp`：24.64%。
- [ ] `cmake/cross/llvm-lib-ar-compatible.sh`：6.67%。
- [ ] `3rdpty/sources/cmake/Buildspdlog.cmake`：22.22%。
- [ ] `3rdpty/sources/cmake/arch-probe.cmake`：27.59%。

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
