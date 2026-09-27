# 音频解码方式

软件设置中的“音频解码方式”提供完整缓存（默认）和流式解码。改变设置后，重新加载项目或试听音频生效；正在播放的音轨保持创建时的方式。

| 方式 | 优点 | 代价与适用范围 |
| --- | --- | --- |
| 完整缓存 | 准备完成后直接读取 PCM，反复跳转、多位置混音稳定，支持长期借用完整声道视图 | 首次需要完成整文件解码，内存随时长、采样率及声道数增长；适合常规写谱、短音效和离线分析 |
| 流式解码 | 同步准备首块后后台按需预读，PCM 页缓存有上限；长音频通常更快开始播放 | 播放期间持续消耗解码 CPU 和文件 IO；缺页或缓存竞争时输出静音，远距离定位可能等待后台准备；适合长音频顺序播放 |

主项目接口：

```cpp
#include "audio/AudioManager.h"
#include "config/AudioPlaybackConfig.h"

MMM::Audio::AudioManager::instance().setDecodingMode(
    MMM::Config::AudioDecodingMode::Streaming);
```

`decodingMode()` 查询当前加载偏好。接口只修改运行时偏好；需要持久化时，通过现有 AppConfig 保存流程写入 `EditorSettings::audioDecodingMode`。JSON 值为 `cached` 或 `streaming`，缺少字段和未知文本回退到 `cached`。

ICE 接口：

```cpp
#include <ice/manage/AudioPool.hpp>
#include <ice/thread/ThreadPool.hpp>

ice::ThreadPool threads(2);
ice::AudioPool pool;
auto track = pool.get_or_load(
    threads, audioPath, ice::CachingStrategy::STREAMING).lock();
```

也可以给 `AudioTrack::create` 显式传入 `CachingStrategy`。`AudioTrack::cachingStrategy()` 查询音轨实际策略。`ICEConfig::default_caching_strategy` 是省略参数时的默认值，只应在处理线程启动前设置。音频池分别保存两种策略的资源，同一路径的播放与分析不会互相逐出缓存。

流式音轨通过 `AudioTrack::read` 获取 PCM，输出格式是创建时的 ICE 内部格式。调用方负责提供足够容量，并处理超出 EOF 的尾部。有效范围内缺页时会交付等长静音并排队预读，不通过零返回值误报播放结束；这意味着返回正帧数并不代表该区间所有样本已就绪。`origin` 不提供借用视图，避免暴露后台会替换的页地址。

流式实现最多保留 16 页，每页 16384 个目标帧，另有一个后台工作页。立体声 float PCM 上限约 2.125 MiB，不含解码后端、线程栈及容器元信息。短于一页的音频在创建时读完，不启动常驻预读线程。多播放位置共享该音轨的有界缓存，超出缓存工作集时可能反复缺页；需要密集随机跳转时应选择完整缓存。

流式定位保持目标样本序列一致：向前跳过时按块解码丢弃，倒退且目标页已淘汰时先回到文件头再顺序推进。当前不使用后端不保证逐样本精度的任意位置 seek，因此远距离跳转的后台成本与跳过长度相关。长度最初来自目标采样率下的元信息估算，遇到 EOF 时修正；未知总长度的输入目前不支持流式创建。

主项目的时间线和独立试听遵循该设置。音效、波形分析与导出固定使用完整缓存；需要资源级变速、变调或 EQ 的时间线音频也使用完整缓存。全局播放效果仍在实时混音链路处理。分析和离线 DSP 可能另行保留整段 PCM，因此流式设置并不保证整个应用只有页缓存大小的音频内存。

创建、加载、资源回收都属于非实时操作。每个长流式音轨有自己的预读线程；音轨销毁会停止并等待该线程，不能在音频回调销毁最后一个所有者。回调读取只尝试缓存锁，不等待文件 IO，也不分配 PCM。

验证必须链接本次 ICE 实现及匹配的公共头文件。已同步 MSVC x64 / clang-cl 22 的静态预编译库（`msvc/2026/Debug`、`msvc/2026/RelWithDebInfo`）和全部 ICE 公共头文件，库内保留 CodeView 调试信息。其他工具链或动态库包仍需按相同源码版本重新生成，不能混用新头文件和旧 ICE 二进制。

在项目根目录执行 MSVC 全源码交叉构建：

```bash
bash scripts/ci/cross/msvc-clang-build.sh --build-dir build/msvc-source-verification --build-type RelWithDebInfo --sources-build
```

首次配置会缓存 Windows Vulkan SDK 路径；后续可直接运行 `cmake --build build/msvc-source-verification`。LAME、FFmpeg、Rubber Band 的外部构建步骤从 CMake 工具链恢复 SDK 头文件和库搜索环境，不要求重新设置原终端的 `INCLUDE`、`LIB`。
