# ICE 注释审计验收索引

## 范围与证据边界

本索引于 2026-09-08 从全项目 cloc 逐文件明细提取，只列 ICE 自维护范围：92 个有效文件和 2 个空文件。包含本引擎维护的构建与工具脚本，排除内部第三方源码、预编译包、生成目录及纯资源。不合并头文件与实现文件比例。

“复核记录定位”对应 [任务单](comment-audit-tasks.md) 的职责批次或复核记录，便于逐项回查，不表示每个文件均通过所有行为规范或所有平台运行测试。数值表不能单独证明注释质量。最终验收仍须逐项确认记录覆盖当前文件，未完成前不勾选全部通过。

## 验收门槛

| 门槛 | 当前证据 | 状态 |
| --- | --- | --- |
| 范围完整与逐文件比例 | 全项目 files.csv，ICE 92/92 ≥30%，2 个空文件不适用 | 数值通过 |
| 接口契约、函数体说明、中文及热路径警告 | 下表对应职责记录及内容快照 | 13/92 已登记终核，79 个有效文件待登记确认 |
| 不改变业务、同步与构建命令 | C++ 去注释比较与构建脚本词法比较 | 当前工作区检查通过 |
| 格式化 | 每批 clang-format / cmake-format 记录 | 已执行，最终复验待完成 |
| 当前 ICE 源码构建与相关 CTest | 独立 Clang64、预编译外部依赖、12 并行 | 已通过 |
| 主项目要求的构建验证 | main-build.log：libc++ 原子 shared_ptr 特化不可用 | 未通过，不改写为 ICE 独立构建替代 |
| 未覆盖分支的证据边界 | Windows C 堆钩子未启用；设备/文件/其他平台未完整运行 | 不作运行通过声明 |
| 全项目复扫与其他不足清单 | 1013 有效、5 空；主仓 685 个不足见 under-30.csv | 已提供，非本轮扩改范围 |
| 修改文件逐项交付统计 | 下表和各批任务记录 | 已提供 |
| 全部审计通过 | 上述门槛尚有未完成项 | 未证明 |

统计产物位于 `build/test_output/comment-audit/`；本索引是本轮快照，后续源文件修改后必须重新核对，不能沿用旧比例充当新证据。

## 已完成注释终核的内容快照

2026-09-08：以下 13 个文件已对照当前内容与对应职责记录完成注释终核：混音/来源节点四文件，以及构建入口和历史错误类型九文件。新增九文件本轮只读确认，格式检查通过，未再次修改源码。此状态仅指注释内容，不豁免已记录的运行行为限制或主项目构建门槛。其余文件继续按已有记录核对，不将“有历史记录”自动升级为终核完成。

| ICE 相对路径 | SHA-256（当前文件字节） |
| --- | --- |
| `include/ice/core/MixBus.hpp` | `bf67d58bbf161457b1dcded67a0bd782f48109086f49e1846967788a54a4b96b` |
| `include/ice/core/SourceNode.hpp` | `b4cc23621b2b86cbab6cbbc9444014d136948514a615fb4a743a9932163cf4da` |
| `src/ice/core/MixBus.cpp` | `0eb30c03cc134de229da186a17fffcdf783423481db7529ce76a1e447b2f57c8` |
| `src/ice/core/SourceNode.cpp` | `dff492233ce2175e4f57266e35c07cfdc702db25d1281b0a880b68a748de8738` |

| `3rdpty/CMakeLists.txt` | `9d9ee2c5b0ad249edb2b54808bb9d78cb01976ed0ec2765d6458f42bd2bc78a1` |
| `3rdpty/sources/cmake/Buildfmt.cmake` | `24a7db89b5b1975670868ab66129ce5451a940a2af5979b1c23d0a9e122767e6` |
| `cmake/configure-project.cmake` | `cdfc911092051b35fff3108027a1041810964b5f45c977331f45238fcff30a01` |
| `cmake/toolchain-macos-to-windows.cmake` | `7054d3db263776b6e55800c750807dd641c5fb5783128fb4fb561ffa1688584c` |
| `CMakeLists.txt` | `893a72f083d6b3c5ae5bed9083dd13e346f1f386c712427c9aa376489716ac6e` |
| `include/ice/execptions/buffer_error.hpp` | `1caca0218b939ad70b8d8df3e7c48442cf41d5036abb386fc458126c998f0672` |
| `include/ice/execptions/instance_build_error.hpp` | `95421544f666de4f7fd6e0bffd205ed5c6d443a144baa1eed9ae805bf1b6492c` |
| `include/ice/execptions/load_error.hpp` | `36497513eebce80ef03fc8f012dacb487d7228c76556f232ff31c9d8504a2976` |
| `src/CMakeLists.txt` | `5c976e804afd03e99f72767f23b4d743f48892ea4f7c8675d9557c6d9f8e2eea` |

后续任何内容或换行变更导致摘要变化时，先检查差异再更新本表；不应重复无差异的整文件补注释。

## 逐文件统计与复核记录定位

| ICE 相对路径 | 注释行 | 代码行 | 注释率 | 复核记录定位 |
| --- | ---: | ---: | ---: | --- |
| `3rdpty/cmake/modules/Findffmpeg.cmake` | 36 | 82 | 30.51% | 构建脚本审计推进记录 |
| `3rdpty/cmake/modules/Findfftw.cmake` | 15 | 35 | 30.00% | 构建脚本审计推进记录 |
| `3rdpty/cmake/modules/Findfmt.cmake` | 16 | 37 | 30.19% | 构建脚本审计推进记录 |
| `3rdpty/cmake/modules/Findlame.cmake` | 14 | 31 | 31.11% | 构建脚本审计推进记录 |
| `3rdpty/cmake/modules/Findlibsamplerate.cmake` | 16 | 37 | 30.19% | 构建脚本审计推进记录 |
| `3rdpty/cmake/modules/FindOpenAL.cmake` | 23 | 53 | 30.26% | 构建脚本审计推进记录 |
| `3rdpty/cmake/modules/Findrubberband.cmake` | 23 | 52 | 30.67% | 构建脚本审计推进记录 |
| `3rdpty/cmake/modules/FindSDL3Static.cmake` | 25 | 57 | 30.49% | 构建脚本审计推进记录 |
| `3rdpty/cmake/modules/Findspdlog.cmake` | 19 | 43 | 30.65% | 构建脚本审计推进记录 |
| `3rdpty/cmake/modules/Findzlib.cmake` | 17 | 38 | 30.91% | 构建脚本审计推进记录 |
| `3rdpty/cmake/modules/PrebuiltLayout.cmake` | 182 | 422 | 30.13% | 预编译布局 helper 审计记录 |
| `3rdpty/CMakeLists.txt` | 8 | 17 | 32.00% | 注释终核完成；构建入口与错误类型收尾记录 |
| `3rdpty/sources/cmake/arch-probe.cmake` | 10 | 21 | 32.26% | 构建脚本审计推进记录 |
| `3rdpty/sources/cmake/Buildffmpeg.cmake` | 133 | 307 | 30.23% | FFmpeg 构建入口审计记录 |
| `3rdpty/sources/cmake/Buildfftw.cmake` | 46 | 98 | 31.94% | FFTW 构建脚本审计记录 |
| `3rdpty/sources/cmake/Buildfmt.cmake` | 8 | 17 | 32.00% | 注释终核完成；构建入口与错误类型收尾记录 |
| `3rdpty/sources/cmake/Buildlame.cmake` | 54 | 124 | 30.34% | LAME 构建脚本审计记录 |
| `3rdpty/sources/cmake/Buildlibsamplerate.cmake` | 45 | 103 | 30.41% | 构建脚本审计推进记录 |
| `3rdpty/sources/cmake/Buildopenal.cmake` | 35 | 80 | 30.43% | 输出后端构建脚本审计记录 |
| `3rdpty/sources/cmake/Buildrubberband.cmake` | 147 | 342 | 30.06% | Rubber Band 构建入口审计记录 |
| `3rdpty/sources/cmake/Buildsdl.cmake` | 63 | 145 | 30.29% | 输出后端构建脚本审计记录 |
| `3rdpty/sources/cmake/Buildspdlog.cmake` | 12 | 28 | 30.00% | 构建脚本审计推进记录 |
| `3rdpty/sources/cmake/Buildzlib.cmake` | 44 | 101 | 30.34% | 构建脚本审计推进记录 |
| `3rdpty/sources/CMakeLists.txt` | 7 | 12 | 36.84% | 全量审计持续推进记录（范围补齐） |
| `cmake/configure-project.cmake` | 4 | 7 | 36.36% | 注释终核完成；构建入口与错误类型收尾记录 |
| `cmake/cross/clang-cl-gcc-compatible.sh` | 19 | 43 | 30.65% | 全量审计持续推进记录（工具脚本） |
| `cmake/cross/llvm-lib-ar-compatible.sh` | 7 | 14 | 33.33% | 全量审计持续推进记录（工具脚本） |
| `cmake/cross/merge-msvc-archives.sh` | 17 | 39 | 30.36% | 全量审计持续推进记录（工具脚本） |
| `cmake/toolchain-macos-to-windows.cmake` | 11 | 15 | 42.31% | 注释终核完成；构建入口与错误类型收尾记录 |
| `CMakeLists.txt` | 30 | 70 | 30.00% | 注释终核完成；构建入口与错误类型收尾记录 |
| `include/ice/config/config.hpp` | 18 | 27 | 40.00% | 配置与音频格式质量复核 |
| `include/ice/core/effect/Clipper.hpp` | 7 | 15 | 31.82% | 独立变调与限幅占位质量复核 |
| `include/ice/core/effect/Compresser.hpp` | 39 | 53 | 42.39% | 压缩效果器质量复核 |
| `include/ice/core/effect/filter/BiquadFilter.hpp` | 22 | 26 | 45.83% | 全量审计持续推进记录（均衡器与滤波器） |
| `include/ice/core/effect/GraphicEqualizer.hpp` | 75 | 51 | 59.52% | 全量审计持续推进记录（均衡器与滤波器） |
| `include/ice/core/effect/IEffectNode.hpp` | 44 | 40 | 52.38% | 测试说明校正与基类质量复核 |
| `include/ice/core/effect/PitchAlter.hpp` | 37 | 33 | 52.86% | 独立变调与限幅占位质量复核 |
| `include/ice/core/effect/rubberband/RStretcher.hpp` | 90 | 68 | 56.96% | A2：预热、补偿与排尾 |
| `include/ice/core/effect/TimeStretcher.hpp` | 172 | 120 | 58.90% | A3：参数发布与实时状态协议 |
| `include/ice/core/IAudioNode.hpp` | 15 | 14 | 51.72% | 测试说明校正与基类质量复核 |
| `include/ice/core/MixBus.hpp` | 102 | 105 | 49.28% | 注释终核完成；混音与来源节点终核记录 |
| `include/ice/core/PlayCallBack.hpp` | 18 | 17 | 51.43% | 接收端与播放通知契约复核 |
| `include/ice/core/SourceNode.hpp` | 168 | 176 | 48.84% | 注释终核完成；混音与来源节点终核记录 |
| `include/ice/execptions/buffer_error.hpp` | 5 | 11 | 31.25% | 注释终核完成；构建入口与错误类型收尾记录 |
| `include/ice/execptions/instance_build_error.hpp` | 5 | 11 | 31.25% | 注释终核完成；构建入口与错误类型收尾记录 |
| `include/ice/execptions/load_error.hpp` | 5 | 11 | 31.25% | 注释终核完成；构建入口与错误类型收尾记录 |
| `include/ice/manage/AudioBuffer.hpp` | 125 | 283 | 30.64% | 音频缓冲质量复核 |
| `include/ice/manage/AudioFormat.hpp` | 9 | 12 | 42.86% | 配置与音频格式质量复核 |
| `include/ice/manage/AudioPool.hpp` | 61 | 142 | 30.05% | 全量审计持续推进记录（音频池） |
| `include/ice/manage/AudioTrack.hpp` | 42 | 52 | 44.68% | 音轨封装质量复核 |
| `include/ice/manage/dec/AlbumArt.hpp` | 36 | 52 | 40.91% | 封面所有权质量复核 |
| `include/ice/manage/dec/CachyDecoder.hpp` | 43 | 41 | 51.19% | 缓存解码质量复核 |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp` | 8 | 18 | 30.77% | A5.1／工厂接口复核 |
| `include/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp` | 16 | 23 | 41.03% | A5／实例实现与契约复核 |
| `include/ice/manage/dec/IDecoder.hpp` | 26 | 21 | 55.32% | 解码接口与流式占位质量复核 |
| `include/ice/manage/dec/IDecoderFactory.hpp` | 15 | 22 | 40.54% | 工厂、实例与媒体信息契约复核 |
| `include/ice/manage/dec/IDecoderInstance.hpp` | 19 | 18 | 51.35% | 工厂、实例与媒体信息契约复核 |
| `include/ice/manage/dec/MediaInfo.hpp` | 12 | 15 | 44.44% | 工厂、实例与媒体信息契约复核 |
| `include/ice/manage/dec/StreamingDecoder.hpp` | 21 | 27 | 43.75% | 解码接口与流式占位质量复核 |
| `include/ice/out/io/cod/IEncoder.hpp` | 0 | 0 | 不适用 | 空文件；无代码可审 |
| `include/ice/out/io/FFmpegFileReceiver.hpp` | 110 | 68 | 61.80% | 文件输出公开契约一致性复核 |
| `include/ice/out/IReceiver.hpp` | 28 | 30 | 48.28% | 接收端与播放通知契约复核 |
| `include/ice/out/play/openal/ALPlayer.hpp` | 82 | 63 | 56.55% | A4.2：OpenAL 播放生命周期 |
| `include/ice/out/play/openal/ALSource.hpp` | 0 | 0 | 不适用 | 空文件；无代码可审 |
| `include/ice/out/play/sdl/SDLPlayer.hpp` | 40 | 51 | 43.96% | 全量审计持续推进记录（SDL 播放器） |
| `include/ice/thread/ThreadPool.hpp` | 54 | 97 | 35.76% | 线程池与分配跟踪质量复核 |
| `include/ice/tool/AllocationTracker.hpp` | 35 | 40 | 46.67% | 线程池与分配跟踪质量复核 |
| `src/CMakeLists.txt` | 22 | 50 | 30.56% | 注释终核完成；构建入口与错误类型收尾记录 |
| `src/ice/config/config.cpp` | 5 | 10 | 33.33% | 配置与音频格式质量复核 |
| `src/ice/core/effect/Clipper.cpp` | 3 | 6 | 33.33% | 独立变调与限幅占位质量复核 |
| `src/ice/core/effect/Compresser.cpp` | 29 | 54 | 34.94% | 压缩效果器质量复核 |
| `src/ice/core/effect/filter/BiquadFilter.cpp` | 21 | 49 | 30.00% | 全量审计持续推进记录（均衡器与滤波器） |
| `src/ice/core/effect/GraphicEqualizer.cpp` | 82 | 189 | 30.26% | 全量审计持续推进记录（均衡器与滤波器） |
| `src/ice/core/effect/IEffectNode.cpp` | 18 | 42 | 30.00% | 测试说明校正与基类质量复核 |
| `src/ice/core/effect/PitchAlter.cpp` | 21 | 28 | 42.86% | 独立变调与限幅占位质量复核 |
| `src/ice/core/effect/rubberband/RStretcher.cpp` | 135 | 315 | 30.00% | A2：预热、补偿与排尾 |
| `src/ice/core/effect/TimeStretcher.cpp` | 316 | 736 | 30.04% | A3：参数发布与实时状态协议 |
| `src/ice/core/MixBus.cpp` | 115 | 262 | 30.50% | 注释终核完成；混音与来源节点终核记录 |
| `src/ice/core/SourceNode.cpp` | 127 | 288 | 30.60% | 注释终核完成；混音与来源节点终核记录 |
| `src/ice/manage/AudioBuffer.cpp` | 41 | 94 | 30.37% | 音频缓冲质量复核 |
| `src/ice/manage/AudioPool.cpp` | 8 | 17 | 32.00% | 全量审计持续推进记录（音频池） |
| `src/ice/manage/AudioTrack.cpp` | 23 | 41 | 35.94% | 音轨封装质量复核 |
| `src/ice/manage/dec/CachyDecoder.cpp` | 66 | 126 | 34.38% | 缓存解码质量复核 |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.cpp` | 67 | 156 | 30.04% | A5.1／工厂接口复核 |
| `src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp` | 192 | 443 | 30.24% | A5／实例实现与契约复核 |
| `src/ice/manage/dec/StreamingDecoder.cpp` | 16 | 28 | 36.36% | 解码接口与流式占位质量复核 |
| `src/ice/out/io/FFmpegFileReceiver.cpp` | 323 | 744 | 30.27% | 文件输出公开契约一致性复核 |
| `src/ice/out/IReceiver.cpp` | 3 | 5 | 37.50% | 接收端与播放通知契约复核 |
| `src/ice/out/play/openal/ALPlayer.cpp` | 333 | 769 | 30.22% | A4.2：OpenAL 播放生命周期 |
| `src/ice/out/play/sdl/SDLPlayer.cpp` | 72 | 163 | 30.64% | 全量审计持续推进记录（SDL 播放器） |
| `src/ice/thread/ThreadPool.cpp` | 2 | 4 | 33.33% | 线程池与分配跟踪质量复核 |
| `src/main.cpp` | 67 | 154 | 30.32% | 手工示例入口审计记录 |
| `src/sample.cpp` | 6 | 13 | 31.58% | A6／实时测试注释收尾 |
| `tests/TimeStretcherRealtimeTest.cpp` | 449 | 1047 | 30.01% | 实时测试注释收尾与数值门槛清零 |
