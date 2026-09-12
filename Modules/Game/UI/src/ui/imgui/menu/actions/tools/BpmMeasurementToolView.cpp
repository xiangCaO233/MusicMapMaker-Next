#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "graphic/imguivk/VKContext.h"
#include "imgui.h"
#include "implot.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectResourceService.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "mmm/timing/BpmNormalization.h"
#include "runtime/AppThreadPool.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/menu/actions/tools/BpmAutomaticMeasurementPolicy.h"
#include "ui/utils/TimeFormatUtils.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fftw3.h>
#include <fmt/format.h>
#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <limits>
#include <numeric>
#include <utility>

/// @file BpmMeasurementToolView.cpp
/// @brief BPM 测量工具的 UI、试听时间线、节拍网格和后台频谱分析实现。
/// @details 本文件同时连接多个执行域，维护时必须保持以下边界：
///
/// UI 线程职责：
/// - 读取项目音频资源列表并维护当前选择；
/// - 绘制波形、频谱、Timing 段和播放控件；
/// - 将鼠标像素坐标转换为统一的画布秒坐标；
/// - 消费后台发布的完整分析结果；
/// - 在 Vulkan 资源回调中分帧上传频谱纹理；
/// - 通过命令队列修改编辑器共享播放状态和谱面 Timing。
///
/// 后台线程职责：
/// - 从分析专用 AudioTrack 分块解码音频；
/// - 生成波形包络和频谱热力图；
/// - 在显式自动测量请求下运行 BPM 检测；
/// - 只向互斥保护的单槽结果写入完成快照；
/// - 通过原子完成标志通知 UI 线程，不直接访问 ImGui 或 Vulkan。
///
/// 时间坐标约定：
/// - 音频时间描述解码器和播放后端中的真实秒数；
/// - 画布时间描述叠加视觉偏移后的 UI 秒数；
/// - 波形和频谱可以分别使用专用视觉偏移；
/// - Timing 段、首拍和节拍标记始终位于画布时间域；
/// - 节拍器调度始终位于音频时间域，不能重复叠加视觉偏移。
///
/// 生命周期约定：
/// - 同一时刻至多有一个后台分析任务；
/// - 新任务启动前协作取消并等待旧任务退出；
/// - CPU 分析容器以移动方式从后台快照交给 UI；
/// - GPU 纹理只在设备安全点释放或重建；
/// - 关闭窗口时保存偏好并卸载独立试听轨；
/// - 析构前等待所有可能引用本对象的后台工作结束。
///
/// 交互优先级约定：
/// - 播放游标手柄拖动优先于拍线拖动；
/// - 拍线拖动优先于背景时间线平移；
/// - 波形和频谱通过 ownerId 共享且互斥地捕获拖动；
/// - 双击背景设置首拍，单击拖动背景执行平移；
/// - 游标拖动只在松手时提交最终 seek；
/// - 边缘滚动即时改变本地视野，不阻塞播放和渲染。
///
/// 性能约定：
/// - 每帧绘制仅访问内存缓存，不解码音频；
/// - 波形使用有序时间数组二分裁剪可见区；
/// - 分拍线按屏幕像素列去重；
/// - 拍点命中扫描设有固定上限；
/// - 频谱上传每帧最多处理一个纹理分块；
/// - 配置保存采用非阻塞消抖，仅关闭时强制落盘。
///
/// 数据所有权约定：
/// - m_waveTimes、m_waveMin 和 m_waveMax 始终属于同一分析批次；
/// - m_waveCanvasTimes 是根据当前波形偏移生成的 UI 线程缓存；
/// - m_pendingSpectrumChunks 保存尚未上传的 CPU RGBA 数据；
/// - m_spectrumTextures 保存可被 ImGui 绘制命令引用的 GPU 纹理；
/// - AnalysisResult 在线程局部构造，发布后由 UI 线程整体移动；
/// - m_pendingResultMutex 只保护单个结果槽，不包围解码和 FFT；
/// - AudioTrack 通过 shared_ptr 跨越任务生命周期，UI 不逐帧复制其所有权。
///
/// 分析状态约定：
/// - m_analysisRunning 表示后台任务仍可能访问本对象；
/// - m_analysisFinished 表示结果槽已经完成发布，可被 UI 消费；
/// - m_analysisProgress 只是近似显示值，不承担内存同步；
/// - release 写入 finished 与 acquire 读取构成结果发布顺序；
/// - 协作取消不生成失败结果，以免覆盖紧随其后的新任务状态；
/// - 真正失败生成带 failed 标志的完整结果，让 UI 统一显示消息；
/// - 自动测量结果缺失与音频分析失败使用不同的状态路径。
///
/// Timing 段约定：
/// - 列表至少保留一个基准段；
/// - 每段起点表示该 BPM 网格的 index 0 拍；
/// - 非首段只在自身起点之后生效；
/// - 相邻段之间保持严格递增时间；
/// - 第一段可以位于音频零点前最多一个拍长；
/// - m_bpm、m_firstBeatTime 和 m_beatLengthSeconds 镜像第一段；
/// - 导出前统一转换为毫秒单位的 BPM Timing；
/// - 写入谱面必须经过可撤销命令，不直接修改会话模型。
///
/// 节拍显示约定：
/// - 整拍使用白色中心线和黄色宽度框；
/// - 每段 index 0 额外使用红色框标识 Timing 起点；
/// - 正式时间轴上的整拍显示跨段连续编号；
/// - 零点前的负拍可绘制但不显示负编号；
/// - 分拍颜色按约分后的分母查询皮肤和用户覆盖调色板；
/// - 标记宽度用毫秒定义，因此缩放后像素宽度自然变化；
/// - 命中半径用像素定义，因此不同缩放下仍保持可操作性。
///
/// 播放路由约定：
/// - Unavailable 表示没有可控制的有效音轨；
/// - Audition 表示 BPM 工具独占的试听 SourceNode；
/// - SynchronizedWithEditor 表示复用活动编辑器的主时间线；
/// - 路由切换必须使节拍器计划失效；
/// - 独立试听同步键由项目根和资源相对路径规范化得到；
/// - 同步路由的播放修改通过 EditorEngine 命令提交；
/// - 独立路由只调用 audition 专用接口，不改变主画布播放状态。
///
/// 偏好与项目状态边界：
/// - 标记宽度、分拍数、视图中心和缩放属于用户偏好；
/// - 音轨选择、BPM、首拍与 Timing 段属于当前项目工作状态；
/// - 播放位置属于音频 transport 状态；
/// - 后台分析结果属于当前选择派生缓存；
/// - 连续拖动只更新内存偏好并重置非阻塞保存倒计时；
/// - 鼠标释放且倒计时结束后才尝试写配置文件；
/// - 保存失败保留脏位并延迟重试；
/// - 窗口关闭和析构是强制保存点。
///
/// 失败处理约定：
/// - 文件系统查询全部使用 error_code，不依赖异常控制流；
/// - 选择失效会清除同步键并卸载独立试听；
/// - 分析轨创建失败不会启动线程；
/// - FFTW 分配或计划失败由守卫发布统一失败结果；
/// - 解码短读保留已获得的有效数据前缀；
/// - 无效 GPU 分块被跳过且不会阻塞上传状态机；
/// - 项目切换期间只绘制占位内容，不读取迁移中的会话资源。
///
/// 坐标转换公式：
/// - visualTime = audioTime + effectiveVisualOffset；
/// - audioTime = canvasTime - effectiveVisualOffset；
/// - waveCanvasTime = waveSampleTime + waveformVisualOffset；
/// - spectrumPixel = (canvasTime - spectrumVisualOffset) * segmentsPerSecond；
/// - screenX = rectMinX + (time - viewStart) / viewRange * rectWidth；
/// - time = viewStart + normalizedMouseX * viewRange；
/// - beatTime = segmentStart + integerBeatIndex * beatLength；
/// - subdivisionTime = segmentStart + integerStepIndex * stepDuration。
/// 所有换算入口都必须处理退化范围和首尾钳制，不能依赖浮点值恰好有效。
///
/// 绘制层次顺序：
/// - 波形包络或频谱纹理作为底层内容；
/// - 分拍线作为低强调网格；
/// - 整拍框和段起点作为测量标记；
/// - 播放游标作为 transport 反馈；
/// - 悬停光晕和拖动预览作为最高交互反馈；
/// - 工具提示在命中处理结束后生成。
/// 顺序不可随意交换，否则底层图像可能遮挡用户正在操纵的对象。
///
/// 鼠标捕获状态：
/// - m_isPlaybackCursorDragging 表示播放手柄已捕获左键；
/// - m_playbackCursorDragOwner 标识捕获来自波形还是频谱；
/// - m_hasPendingPlaybackSeek 表示存在尚未提交的松手位置；
/// - m_isBeatMarkerDragging 表示某根段起点或整拍已捕获左键；
/// - m_beatMarkerDragOwner 防止另一个分析视图重复处理；
/// - m_beatMarkerDragMode 区分移动段起点和反推拍长；
/// - m_isTimelinePanning 只在两个具体对象均未捕获时成立；
/// - m_isOverviewTimelineDragging 与分析区背景平移互斥。
/// 捕获状态必须在鼠标释放、区域失效和路由失效时明确清理。
///
/// 频谱内存布局：
/// - 横轴为按 segmentsPerSecond 采样的时间段；
/// - 纵轴为经指数偏置映射的频率区间；
/// - heatmap 索引布局为 bin * segmentCount + segment；
/// - dB 值被截断在 -100 到 0 范围；
/// - 转换纹理时纵轴翻转，使高频显示在图像顶部；
/// - RGBA 像素按行连续存储；
/// - 超过 MAX_TEXTURE_W 的横向数据切成多个相邻纹理；
/// - 绘制时只对当前像素范围相交的分块计算 UV。
///
/// 波形内存布局：
/// - 每个固定时间桶保存起始时间、最小幅值和最大幅值；
/// - 多声道 PCM 先按帧平均为单声道值；
/// - 最小值和最大值都从零开始，保证包络包含静音基线；
/// - 三个源数组必须以共同最小长度提交给 ImPlot；
/// - 派生画布时间数组与源时间一一对应；
/// - 可见区二分后向左右各扩一个点，避免边缘断口；
/// - 数据点数在提交前限制到 ImPlot 的 int 计数上限。
///
/// 自动测量结果处理：
/// - 自动检测只在显式请求且音轨至少十秒时运行；
/// - 检测器使用完整单声道 PCM 和引擎内部采样率；
/// - BPM 先通过公共规范化规则；
/// - offset 从毫秒转换为首拍秒数；
/// - 首拍按一拍负范围和画布末尾钳制；
/// - 自动结果替换工具内 Timing 段为单个基准段；
/// - 对齐误差同时格式化为拍分数用于状态展示；
/// - 向导回调存在时直接导出，不存在时要求用户确认应用目标。
///
/// 维护限制：
/// - 不在任何每帧绘制函数中增加文件存在性检查；
/// - 不在绘制函数中创建 FFTW 计划或解码缓冲；
/// - 不在后台线程中调用 ImGui、ImPlot 或 Vulkan；
/// - 不让后台线程直接移动 UI 当前使用的容器；
/// - 不让独立试听操作影响编辑器主播放状态；
/// - 不让视觉偏移进入节拍器的真实音频调度；
/// - 不用固定 sleep 等待分析、播放或用户交互状态；
/// - 不在拖动期间连续写配置文件或连续提交昂贵 seek。
///
/// ImGui 状态约定：
/// - 窗口标题使用 ###BpmMeasurementTool 作为稳定内部 ID；
/// - 波形、频谱、全局条和段落列表使用互不冲突的隐藏 ID；
/// - 段落行在循环中压入索引 ID，并在任何提前退出前恢复；
/// - BeginChild 和 EndChild 在返回值为 false 时仍必须成对；
/// - BeginPopupModal 成功后始终调用 EndPopup；
/// - BeginDisabled 与 EndDisabled 按同一条件严格成对；
/// - 紧凑按钮样式一次压入两项并一次弹出两项；
/// - 绘图裁剪矩形在每条控制路径中保持栈平衡。
///
/// 本地化与状态文本约定：
/// - 固定控件文本通过 TR 获取当前语言；
/// - 含数值状态通过 TR_FMT 格式化；
/// - 资源 ID、类型和路径保持项目原始文本；
/// - 文件不存在、加载失败和自动检测失败使用不同消息；
/// - 普通分析完成不暗示 BPM 已自动测得；
/// - 自动结果展示规范化 BPM、offset、误差、原始候选及节奏特征；
/// - 导出和应用成功只在操作真正提交后更新状态。
///
/// 节拍器资源约定：
/// - 普通拍使用 metronome.beat_low；
/// - 四拍周期的 index 0 使用 metronome.downbeat_high；
/// - 皮肤可为两个 key 分别提供路径和 lead-in；
/// - 缺失皮肤路径时回退 resources/audio/metronome；
/// - 已预加载资源通过时长查询复用；
/// - 两个资源必须同时可用才把 ready 设为 true；
/// - 前台打开和真正开始播放都可幂等确保资源就绪；
/// - 每帧调度更新本身不进行文件加载。
///
/// 线程同步约定：
/// - m_analysisProgress 使用 relaxed，因为它不保护其他状态；
/// - m_analysisRunning 使用 relaxed，只表达用户界面的运行提示；
/// - m_analysisFinished 的 release/acquire 保护结果发布顺序；
/// - m_pendingResult 的读取和写入都持有同一互斥锁；
/// - FFTW plan 的创建和销毁持有进程级 planner 互斥锁；
/// - fftw_execute 使用任务独占 plan 和缓冲，不持有 planner 锁；
/// - 停止令牌在主要长循环的有界间隔内检查；
/// - future wait 只出现在选择切换、重分析和析构等低频路径。
///
/// 可撤销编辑约定：
/// - BPM 工具内参数拖动只修改工具草稿状态；
/// - 双击设置首拍同样只修改草稿首段；
/// - 段落新增、删除、排序均不直接触碰 BeatMap；
/// - 应用弹窗明确选择目标已打开会话；
/// - 应用前切换活动会话并请求相应焦点；
/// - CmdReplaceBeatmapTimings 承担实际谱面写入；
/// - keepNonBpmTimingsOnApply 决定是否保留其他 TimingEffect；
/// - 向导回调接收值语义 Timing 列表，不共享工具内部容器。
///
/// 单位约定：
/// - 本文件内部时刻和时长默认使用 double 秒；
/// - 谱面 Timing 的 timestamp 和 beat_length 使用 double 毫秒；
/// - UI markerWidth 偏好使用毫秒并在绘制时转换为秒；
/// - 鼠标命中半径、线宽、边距和窗口尺寸使用 ImGui 像素；
/// - 波形幅值使用归一化浮点 PCM 范围；
/// - 频谱能量使用 dB，并在生成颜色前钳制；
/// - 频谱横轴内部使用整数段或纹理像素列；
/// - 自动检测 offset 返回毫秒，消费时转换为秒。
///
/// 数值稳定约定：
/// - BPM 始终经 normalizeBpmValue 进入公共合法范围；
/// - 拍长由 60 / BPM 计算，并在除法前验证正值；
/// - 可视时间跨度至少保留 0.001 秒；
/// - 像素宽度至少保留 1.0，避免归一化除零；
/// - 段起点比较使用微秒级容差抑制浮点抖动；
/// - 拍索引和分拍索引使用整数解析式生成时间；
/// - 负索引取模后校正到非负节奏周期；
/// - FFT dB 转换仅对高于噪声阈值的幅度开方取对数。
///
/// 资源切换约定：
/// - 音轨 ID 是项目内稳定选择身份；
/// - 项目根与资源路径共同形成播放同步身份；
/// - 标签文本变化不影响选择或缓存命中；
/// - 路径身份变化立即卸载旧独立试听；
/// - 新身份把分析设置为待刷新，但不会在身份函数中创建线程；
/// - 显式分析请求消费待刷新位并验证实际文件；
/// - 新结果到达前不把旧 GPU 纹理与新 CPU 元数据混合显示；
/// - 纹理重载完成后才释放待上传像素并清除脏位。

#ifndef M_PI
#    define M_PI 3.14159265358979323846
#endif

namespace MMM::UI
{
namespace
{
/// @brief 播放指针三角手柄半宽，单位为像素。
/// @note 与矩形指针线共同构成可见且可拾取的顶部手柄。
constexpr float PLAYBACK_CURSOR_HANDLE_HALF_WIDTH = 7.0f;

/// @brief 播放指针三角手柄高度，单位为像素。
/// @note 高度也参与分析区域顶部的鼠标拾取范围。
constexpr float PLAYBACK_CURSOR_HANDLE_HEIGHT = 11.0f;

/// @brief BPM 测量线横向拾取半径，单位为像素。
/// @note 拾取范围大于可见线宽，降低精确点击难度。
constexpr float BEAT_MARKER_LINE_PICK_RADIUS = 6.0f;

/// @brief BPM 测量线悬浮发光线宽，单位为像素。
/// @note 只用于反馈覆盖层，不改变实际节拍线几何位置。
constexpr float BEAT_MARKER_LINE_GLOW_WIDTH = 8.0f;

/// @brief 非首段首拍红线吸附到前一段分拍网格的像素半径。
/// @note 像素阈值随后根据当前时间缩放转换为秒。
constexpr float BEAT_MARKER_SEGMENT_SNAP_RADIUS = 10.0f;

/// @brief BPM 整拍索引文本距绘制区域底部的距离，单位为像素。
/// @note 保持标签位于图像内部且不覆盖底边。
constexpr float BEAT_MARKER_INDEX_LABEL_BOTTOM_PADDING = 3.0f;

/// @brief BPM 工具普通拍节拍器音效 key。
/// @note 由内置皮肤音效注册表解析为低重音拍声。
constexpr const char* BPM_METRONOME_LOW_KEY = "metronome.beat_low";

/// @brief BPM 工具四拍重音节拍器音效 key。
/// @note 每四个全局拍索引使用高重音声。
constexpr const char* BPM_METRONOME_HIGH_KEY = "metronome.downbeat_high";

/// @brief BPM 工具节拍器播放时的额外音量倍率。
/// @note 最终音量仍会与皮肤资源及用户音效配置共同作用。
constexpr float BPM_METRONOME_VOLUME_FACTOR = 2.0f;

/// @brief BPM 测量工具默认浮动窗口宽度，单位为 ImGui 像素。
/// @note 当前视口过窄时由尺寸计算入口进一步限制。
constexpr float BPM_MEASURE_DEFAULT_WINDOW_WIDTH = 1120.0f;

/// @brief BPM 测量工具默认浮动窗口高度，单位为 ImGui 像素。
/// @note 当前视口过矮时保留最小可操作高度。
constexpr float BPM_MEASURE_DEFAULT_WINDOW_HEIGHT = 720.0f;

/// @brief BPM 测量工具浮动窗口距主视口边缘的安全留白。
/// @note 仅影响首次浮动位置，不覆盖用户后续停靠布局。
constexpr float BPM_MEASURE_WINDOW_MARGIN = 24.0f;

/// @brief BPM 测量工具浮动窗口避开主窗口标题栏的顶部安全距离。
/// @note 顶部安全区大于普通边距，防止首次窗口遮挡标题栏控件。
constexpr float BPM_MEASURE_WINDOW_TOP_SAFE_MARGIN = 56.0f;

/// @brief 节拍器音效提前调度窗口，单位为秒。
/// @note 调度窗口补偿 UI 帧与音频图处理间的提交延迟。
constexpr double BPM_METRONOME_SCHEDULE_LOOKAHEAD_SECONDS = 0.2;

/// @brief 单帧最多触发的节拍器音效数量，避免跳转后爆发播放。
/// @note 超出上限时重置调度游标，不补播大量历史拍点。
constexpr int BPM_METRONOME_MAX_TRIGGERED_PER_FRAME = 8;

/// @brief BPM 工具全局时间滚动条最小高度，单位为像素。
/// @note 低于该高度时仍保证缩略轨道和窗口手柄可拾取。
constexpr float BPM_OVERVIEW_SCROLLBAR_MIN_HEIGHT = 24.0f;

/// @brief BPM 工具紧凑文字按钮的最大横向内边距，单位为逻辑像素。
/// @note 实际内边距不会大于当前主题 FramePadding。
constexpr float BPM_COMPACT_BUTTON_PADDING_X = 4.0f;

/// @brief 播放指针拖到分析视图边缘时触发自动滚动的范围，单位为像素。
/// @note 边缘滚动只在手柄拖拽期间生效。
constexpr float PLAYBACK_CURSOR_EDGE_SCROLL_MARGIN = 20.0f;

/// @brief 连续调整 BPM 工具偏好停止后等待落盘的时间，单位为秒。
/// @note 非阻塞延迟只合并配置写入，不延迟本地交互反馈。
constexpr double BPM_USER_PREFERENCE_SAVE_DELAY_SECONDS = 0.35;

/// @brief BPM 工具偏好保存失败后的重试等待时间，单位为秒。
/// @note 失败保留脏标志，后续帧到期再尝试保存。
constexpr double BPM_USER_PREFERENCE_SAVE_RETRY_DELAY_SECONDS = 1.0;

/// @brief 压入 BPM 工具窄文字按钮的紧凑内边距和居中样式。
/// @warning UI 热路径：每帧只读取当前样式和 DPI 并压入两个样式变量。
/// @note 必须与 popBpmCompactTextButtonStyleVars 在同一绘制路径成对调用。
/// @details 横向内边距随 DPI 取整且不超过主题值，纵向内边距保持不变；
/// 文本居中只作用于紧凑按钮，调用者在成组绘制结束后统一恢复样式栈。
void pushBpmCompactTextButtonStyleVars()
{
    // 横向内边距随 DPI 放大，但不超过主题原始值。
    const ImGuiStyle& style = ImGui::GetStyle();
    const float       dpiScale =
        std::max(1.0f, Config::AppConfig::instance().getWindowContentScale());
    // floor 使紧凑按钮边缘落在设备像素上。
    const float paddingX =
        std::min(style.FramePadding.x,
                 std::floor(BPM_COMPACT_BUTTON_PADDING_X * dpiScale));
    // 保留主题纵向内边距，避免改变按钮行高。
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(paddingX, style.FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
}

/// @brief 恢复 BPM 工具窄文字按钮的局部样式。
/// @warning UI 热路径：与 pushBpmCompactTextButtonStyleVars 成对调用。
/// @post 恢复 FramePadding 与 ButtonTextAlign 两项样式变量。
void popBpmCompactTextButtonStyleVars()
{
    ImGui::PopStyleVar(2);
}

/// @brief 获取 FFTW 计划互斥锁，保护全局 planner 状态。
/// @return 进程内唯一 FFTW planner 互斥锁。
/// @note 计划创建和销毁需要串行化，执行已创建计划不使用该锁。
/// @details FFTW planner 的全局状态可能被不同功能的后台任务共享；
/// 函数静态互斥避免额外生命周期管理，也不扩大到耗时 FFT 执行阶段。
std::mutex& fftwPlanMutex()
{
    // 函数静态对象避免跨翻译单元初始化顺序依赖。
    static std::mutex mutex;
    return mutex;
}

/// @brief 计算节拍器允许补响的时间窗口。
/// @param beatLengthSeconds 当前拍长，单位为秒。
/// @return 已越过拍点但仍允许立即补响的时间窗口，单位为秒。
/// @note 使用四分之一拍并钳制到 35–120ms，兼顾快慢速度。
/// @details 窗口用于容忍 UI 帧跨过拍点，并非调度前瞻长度；非法拍长
/// 使用固定 50ms 兜底，防止 NaN 进入拍索引计算。
double metronomePastTriggerWindow(double beatLengthSeconds)
{
    // 非法拍长回退 50ms，避免调度器传播非有限值。
    if ( !std::isfinite(beatLengthSeconds) || beatLengthSeconds <= 0.0 ) {
        return 0.05;
    }
    // 上下限防止极端 BPM 产生不可感知或过宽补响窗口。
    return std::clamp(beatLengthSeconds * 0.25, 0.035, 0.12);
}

/// @brief 生成音频资源在下拉框中的显示文本。
/// @param resource 音频资源。
/// @return 适合 UI 展示的标签。
/// @note 标签同时包含稳定 ID、资源类型和项目路径。
/// @details ID 用于区分同名资源，类型提示其播放语义，路径帮助用户确认
/// 实际文件；该标签只用于展示，资源查找始终使用原始 ID。
std::string makeAudioResourceLabel(const AudioResource& resource)
{
    // BPM 工具候选只含 Main 或 Effect，非 Main 统一显示 Effect。
    const char* typeText =
        resource.m_type == AudioTrackType::Main ? "Main" : "Effect";
    return resource.m_id + " [" + typeText + "] - " + resource.m_path;
}

/// @brief ImGui 分拍线样式。
/// @details 以已经转换的 ImU32 颜色和设备像素线宽传给绘制列表。
struct BeatLineStyle {
    /// @brief ImGui RGBA 颜色。
    /// @note 默认白色用于皮肤键缺失时保持线条可见。
    ImU32 color{ IM_COL32(255, 255, 255, 255) };

    /// @brief 线宽，单位为像素。
    /// @note 最终值来自皮肤 beat_lines_width 配置。
    float width{ 2.0f };
};

/// @brief 音频控制器同款同步播放时间快照。
/// @details 同时保留音频域和视觉域时间，避免调用点重复应用视觉偏移。
struct PlaybackTimelineState {
    /// @brief 音频时间，单位为秒。
    /// @note 用于音频 seek、节拍器调度和播放进度。
    double audioTime{ 0.0 };

    /// @brief 叠加视觉偏移后的视觉时间，单位为秒。
    /// @note 用于画布指针和跟随视图。
    double visualTime{ 0.0 };

    /// @brief 音频总时长，单位为秒。
    /// @note 独立试听与编辑器路由分别从对应 SourceNode 读取。
    double totalTime{ 0.0 };

    /// @brief 当前视觉偏移，单位为秒。
    /// @note 来自 VisualConfig 的有效偏移。
    double visualOffset{ 0.0 };

    /// @brief 当前逻辑播放状态。
    /// @note 同步路由有快照时以逻辑快照状态为准。
    bool isPlaying{ false };
};

/// @brief 将 BPM 测量工具默认尺寸限制在当前主视口可用区域内。
/// @param viewport 当前 ImGui 主视口。
/// @param dpiScale 当前窗口内容 DPI 缩放。
/// @return BPM 测量工具首次打开时使用的窗口尺寸。
/// @warning UI 低频路径：仅窗口首次显示时计算，不修改视口。
/// @note 默认尺寸按可用工作区收缩，并保留最小可操作宽高。
/// @details 水平使用两侧普通边距，垂直额外保留标题栏安全区；
/// 视口小于最低尺寸时仍优先保证控件可布局，由窗口系统处理最终可见范围。
ImVec2 calculateDefaultBpmMeasureWindowSize(const ImGuiViewport& viewport,
                                            float                dpiScale)
{
    // 普通边距和顶部安全距分别转换到设备像素。
    const float margin = std::floor(BPM_MEASURE_WINDOW_MARGIN * dpiScale);
    const float topSafeMargin =
        std::floor(BPM_MEASURE_WINDOW_TOP_SAFE_MARGIN * dpiScale);
    // 工作区极小时仍返回最低 360x320，避免控件完全折叠。
    const float maxWidth =
        std::max(360.0f, viewport.WorkSize.x - margin * 2.0f);
    const float maxHeight =
        std::max(320.0f, viewport.WorkSize.y - topSafeMargin - margin);

    // 正常视口使用默认逻辑尺寸，小视口则限制到可用上限。
    return { std::min(BPM_MEASURE_DEFAULT_WINDOW_WIDTH, maxWidth),
             std::min(BPM_MEASURE_DEFAULT_WINDOW_HEIGHT, maxHeight) };
}

/// @brief 计算 BPM 测量工具打开时避开主窗口标题栏的浮动位置。
/// @param viewport 当前 ImGui 主视口。
/// @param windowSize 将要使用的窗口尺寸。
/// @param dpiScale 当前窗口内容 DPI 缩放。
/// @return BPM 测量工具打开时使用的窗口左上角坐标。
/// @warning UI 低频路径：只计算坐标，不修改 ImGui 状态。
/// @note 优先在安全工作区居中，空间不足时贴靠允许的最小位置。
/// @details X 轴相对完整工作区居中，Y 轴相对标题栏下方区域居中；
/// 当可用上界小于下界时避免反向 clamp，直接使用安全区起点。
ImVec2 calculateDefaultBpmMeasureWindowPos(const ImGuiViewport& viewport,
                                           const ImVec2&        windowSize,
                                           float                dpiScale)
{
    const float margin = std::floor(BPM_MEASURE_WINDOW_MARGIN * dpiScale);
    const float topSafeMargin =
        std::floor(BPM_MEASURE_WINDOW_TOP_SAFE_MARGIN * dpiScale);
    // 最小坐标从工作区左上角加安全边距得到。
    const float minX = viewport.WorkPos.x + margin;
    const float minY = viewport.WorkPos.y + topSafeMargin;
    // 最大坐标保证窗口右侧和底部仍留出普通边距。
    const float maxX =
        viewport.WorkPos.x + viewport.WorkSize.x - windowSize.x - margin;
    const float maxY =
        viewport.WorkPos.y + viewport.WorkSize.y - windowSize.y - margin;

    // 水平方向相对完整工作区居中。
    const float centeredX =
        viewport.WorkPos.x + (viewport.WorkSize.x - windowSize.x) * 0.5f;
    // 垂直方向只在标题栏以下的安全区域居中。
    const float visibleTop    = viewport.WorkPos.y + topSafeMargin;
    const float visibleHeight = viewport.WorkSize.y - topSafeMargin;
    const float centeredY = visibleTop + (visibleHeight - windowSize.y) * 0.5f;

    // 可用范围退化时不调用反向边界 clamp，直接采用最小位置。
    const float posX = maxX >= minX ? std::clamp(centeredX, minX, maxX) : minX;
    const float posY = maxY >= minY ? std::clamp(centeredY, minY, maxY) : minY;
    return { posX, posY };
}

/// @brief 将皮肤颜色转换为 ImGui 颜色。
/// @param color 皮肤颜色。
/// @param alphaScale 额外透明度倍率。
/// @return ImGui RGBA 颜色。
/// @note 每个通道先钳制到 0–1，再缩放到八位整数。
/// @details alphaScale 只与皮肤 alpha 相乘，RGB 保持原色；越界和负值
/// 在转换前钳制，避免整型转换环绕。
ImU32 toImColor(const Config::Color& color, float alphaScale)
{
    // RGB 不受额外透明度影响，alpha 单独与 alphaScale 相乘。
    return IM_COL32(static_cast<int>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f),
                    static_cast<int>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f),
                    static_cast<int>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f),
                    static_cast<int>(
                        std::clamp(color.a * alphaScale, 0.0f, 1.0f) * 255.0f));
}

/// @brief 获取 BPM 工具允许的最早首拍时间。
/// @param beatLengthSeconds 当前拍长，单位为秒。
/// @return 允许首拍略早于音频起点，最多早一拍。
/// @note 非正拍长回退零，不开放负时间范围。
double firstBeatMinSeconds(double beatLengthSeconds)
{
    // 一拍负范围允许把距零点最近的首拍放在音频起点之前。
    return -std::max(0.0, beatLengthSeconds);
}

/// @brief 将首拍时间限制到 BPM 工具可编辑范围。
/// @param firstBeatTime 首拍时间，单位为秒。
/// @param beatLengthSeconds 当前拍长，单位为秒。
/// @param canvasDuration 当前音频画布时长，单位为秒。
/// @return 限制后的首拍时间。
/// @note 下界为负一拍，上界为非负画布末尾。
double clampFirstBeatTime(double firstBeatTime, double beatLengthSeconds,
                          double canvasDuration)
{
    // 双精度 clamp 保留拖拽和自动测量的亚毫秒精度。
    return std::clamp<double>(firstBeatTime,
                              firstBeatMinSeconds(beatLengthSeconds),
                              std::max(0.0, canvasDuration));
}

/// @brief 将毫秒残差格式化为相对拍长的近似分数。
/// @param inaccuracyMs 残差 RMS，单位为毫秒。
/// @param beatLengthSeconds 当前拍长，单位为秒。
/// @return 形如 1/64 的拍内分数。
/// @warning UI 展示路径：遍历固定 15 个候选分母。
/// @note 选择绝对误差最小的有理近似并约分。
/// @details 候选分母覆盖常见二分、三分及复合节奏；相同误差时保留
/// 更早且通常更简单的分母。小于最高分辨率的非零值显示为上界形式。
std::string formatBeatFractionInaccuracy(double inaccuracyMs,
                                         double beatLengthSeconds)
{
    // 先统一到毫秒单位，残差与拍长可直接求比例。
    const double beatMs = beatLengthSeconds * 1000.0;
    if ( !(beatMs > 0.0) || !std::isfinite(beatMs) ||
         !std::isfinite(inaccuracyMs) ) {
        // 非法输入不向状态栏传播 NaN 或无穷文本。
        return "0";
    }

    // 负残差防御性按零处理。
    const double fraction = std::max(0.0, inaccuracyMs / beatMs);
    // 候选兼顾二分、三分及其复合细分，最高到 192 分拍。
    constexpr std::array<int, 15> denominators{ 1,  2,  3,  4,  6,  8,   12, 16,
                                                24, 32, 48, 64, 96, 128, 192 };

    // 初始化为最高分母，确保极小非零残差可显示上界提示。
    int    bestNumerator   = 0;
    int    bestDenominator = denominators.back();
    double bestError       = std::numeric_limits<double>::infinity();
    // 每个分母取最近整数分子并比较原始比例误差。
    for ( int denominator : denominators ) {
        const int numerator =
            static_cast<int>(std::max(0.0, std::round(fraction * denominator)));
        const double approx =
            static_cast<double>(numerator) / static_cast<double>(denominator);
        const double error = std::abs(approx - fraction);
        if ( error < bestError ) {
            // 严格更小才替换，平局保留列表中更简单的较早分母。
            bestError       = error;
            bestNumerator   = numerator;
            bestDenominator = denominator;
        }
    }

    if ( bestNumerator <= 0 ) {
        // 非零但小于最高分辨率时显示小于号，避免误称精确为零。
        return fraction > 0.0 ? fmt::format("<1/{}", denominators.back()) : "0";
    }

    // 最终用最大公约数化为最简分数。
    const int divisor = std::gcd(bestNumerator, bestDenominator);
    return fmt::format(
        "{}/{}", bestNumerator / divisor, bestDenominator / divisor);
}

/// @brief 按主画布规则查询指定分母的分拍线样式。
/// @param denominator 分拍分母。
/// @return 分拍线颜色和线宽。
/// @warning UI 热路径：查询固定皮肤键和内存配置，不访问文件系统。
/// @note 缺失专用颜色时回退 default，并允许视觉配置覆盖调色板。
/// @details 颜色键和线宽键都遵循 beat_lines 的分母命名；皮肤管理器
/// 以洋红色作为缺失颜色哨兵。用户覆盖只替换颜色，透明度与皮肤线宽
/// 仍通过统一规则应用，使 BPM 工具和主画布保持一致。
BeatLineStyle getBeatLineStyle(int denominator)
{
    // 非正分母按整拍处理，避免构造无效皮肤键。
    if ( denominator <= 0 ) {
        denominator = 1;
    }

    // 颜色键与分母直接对应主画布 beat_lines 命名约定。
    auto&         skin  = Config::SkinManager::instance();
    std::string   key   = "beat_lines.beat_" + std::to_string(denominator);
    Config::Color color = skin.getColor(key);
    if ( color.r == 1.0f && color.g == 0.0f && color.b == 1.0f &&
         color.a == 1.0f ) {
        // 洋红哨兵表示皮肤键不存在，颜色和线宽均回退 default。
        color = skin.getColor("beat_lines.default");
        key   = "beat_lines_width.default";
    } else {
        key = "beat_lines_width.beat_" + std::to_string(denominator);
    }

    // 用户调色板覆盖优先于皮肤颜色，但继续应用统一透明度。
    const auto& visual = Config::AppConfig::instance().getVisualConfig();
    if ( visual.overrideBeatLineColors ) {
        // 分母通过统一槽位映射到有限调色板。
        const auto& overrideColor =
            visual
                .beatLineColors[Config::beatLineColorPaletteSlot(denominator)];
        color = { overrideColor[0],
                  overrideColor[1],
                  overrideColor[2],
                  overrideColor[3] };
    }
    // 线宽仍来自皮肤，缺失专用值时使用默认宽度或 2px。
    return { toImColor(color, visual.beatLineAlpha),
             skin.getValue(key,
                           skin.getValue("beat_lines_width.default", 2.0f)) };
}

/// @brief 读取指定 BPM 播放路由的播放状态。
/// @param route 当前 BPM 播放路由。
/// @param audioManager 音频管理器。
/// @return 对应编辑器主音轨或独立试听通道的播放状态。
/// @warning UI 热路径：每帧读取固定数量的内存状态。
Audio::PlaybackStatus getPlaybackStatus(BpmPlaybackRoute           route,
                                        const Audio::AudioManager& audioManager)
{
    if ( route == BpmPlaybackRoute::Audition ) {
        // 独立试听读取 audition SourceNode，不影响主编辑器音轨。
        return audioManager.getAuditionStatus();
    }
    if ( route == BpmPlaybackRoute::SynchronizedWithEditor ) {
        // 同轨模式复用主音频图播放状态。
        return audioManager.getStatus();
    }
    // 无可用路由统一表现为停止状态。
    return Audio::PlaybackStatus::Stopped;
}

/// @brief 读取指定 BPM 播放路由的当前音频时间。
/// @param route 当前 BPM 播放路由。
/// @param audioManager 音频管理器。
/// @return 当前音频时间，单位为秒。
/// @warning UI 热路径：每帧读取一次 SourceNode 播放位置。
/// @pre route 不应为 Unavailable；调用方在读取前负责短路。
double getPlaybackCurrentTime(BpmPlaybackRoute           route,
                              const Audio::AudioManager& audioManager)
{
    // 两个有效路由分别读取独立试听或主播放图时间。
    return route == BpmPlaybackRoute::Audition
               ? audioManager.getAuditionCurrentTime()
               : audioManager.getCurrentTime();
}

/// @brief 读取指定 BPM 播放路由的音频总时长。
/// @param route 当前 BPM 播放路由。
/// @param audioManager 音频管理器。
/// @return 音频总时长，单位为秒。
/// @warning UI 热路径：每帧读取一次 SourceNode 时长。
/// @pre route 不应为 Unavailable；调用方在读取前负责短路。
double getPlaybackTotalTime(BpmPlaybackRoute           route,
                            const Audio::AudioManager& audioManager)
{
    // 时长必须与当前路由时间来自同一个 SourceNode。
    return route == BpmPlaybackRoute::Audition
               ? audioManager.getAuditionTotalTime()
               : audioManager.getTotalTime();
}

/// @brief 按当前路由读取 BPM 工具播放时间。
/// @param route 当前 BPM 播放路由。
/// @return 已应用视觉偏移和亚帧补偿的播放时间。
/// @warning UI 热路径约束如下。
/// 同轨时读取同步缓冲区快照；独立试听时只读取 audition SourceNode。
/// @note 快照预览拖拽期间使用其显式时间，正常播放时再做稳态时钟外推。
/// @details 返回值统一封装两类播放源：
/// - Audition 直接读取独立 SourceNode；
/// - SynchronizedWithEditor 优先读取活动画布同步快照；
/// - 同步快照缺失时退回主 SourceNode；
/// - 预览拖拽期间不执行时钟外推；
/// - 正常播放时以 steady_clock 将快照推进到当前帧；
/// - 调用方不持有同步缓冲或快照生命周期。
PlaybackTimelineState readPlaybackTimelineState(BpmPlaybackRoute route)
{
    // 首先读取所有路由共享的有效视觉偏移。
    auto&                 audioManager = Audio::AudioManager::instance();
    PlaybackTimelineState state;
    state.visualOffset = Config::AppConfig::instance()
                             .getVisualConfig()
                             .getEffectiveVisualOffset();
    if ( route == BpmPlaybackRoute::Unavailable ) {
        // 不可用路由返回零时间和停止状态，但保留视觉偏移配置。
        return state;
    }

    // SourceNode 提供基础音频时间、时长和播放状态。
    state.audioTime = getPlaybackCurrentTime(route, audioManager);
    // 独立试听没有逻辑快照，视觉时间直接叠加配置偏移。
    state.visualTime = state.audioTime + state.visualOffset;
    state.totalTime  = getPlaybackTotalTime(route, audioManager);
    state.isPlaying  = getPlaybackStatus(route, audioManager) ==
                       Audio::PlaybackStatus::Playing;

    if ( route != BpmPlaybackRoute::SynchronizedWithEditor ) {
        // audition 路由到此已经获得完整状态，无需查询画布同步缓冲。
        return state;
    }

    // 同轨路由优先从活动画布同步快照取得逻辑一致时间。
    std::string activeCameraId =
        Logic::EditorEngine::instance().getActiveCameraId();
    // 活动画布缺失时回退主二维画布的稳定同步键。
    auto syncBuffer = Logic::EditorEngine::instance().getSyncBuffer(
        activeCameraId.empty() ? "Basic2DCanvas" : activeCameraId);
    if ( !syncBuffer ) {
        // 无同步缓冲时保留 SourceNode 基础状态作为降级结果。
        return state;
    }

    // 读取快照不阻塞生产者，空快照继续使用音频管理器值。
    auto snapshot = syncBuffer->getReadingSnapshot();
    if ( !snapshot ) {
        return state;
    }

    // 逻辑快照同时提供视觉域和音频域时间，保持主画布一致。
    state.visualTime = snapshot->currentTime;
    state.audioTime  = snapshot->playbackTime;
    state.isPlaying  = snapshot->isPlaying;

    if ( !snapshot->isPreviewDragging ) {
        // 非拖拽时按稳态时钟外推至当前帧，减少 UI 指针阶梯跳动。
        const double now =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        // 快照方法内部依据播放状态决定是否实际推进。
        state.visualTime = snapshot->resolveCurrentTimeAt(now);
        state.audioTime  = snapshot->resolvePlaybackTimeAt(now);
    }

    // 返回值不保存同步缓冲或快照指针。
    return state;
}

}  // namespace

/// @brief 构造 BPM 测量工具窗口。
/// @param name IUIView 与 ITextureLoader 使用的稳定视图名称。
/// @note 构造时恢复不依赖音轨的偏好，音频选择在打开流程确定。
/// @details 构造函数不解析项目资源、不加载音频、不创建 GPU 纹理，
/// 因而可在 UI 注册阶段安全调用；首次打开时再完成具体资源初始化。
BpmMeasurementToolView::BpmMeasurementToolView(const std::string& name)
    : IUIView(name), ITextureLoader(name)
{
    // 恢复倍速、视图范围和节拍器等全局用户偏好。
    restoreUserPreferences();
    m_statusText = TR("ui.tools.bpm_measure.select_track").data();
}

/// @brief 销毁窗口并等待后台分析任务和 GPU 资源释放。
/// @warning 低频析构路径：强制保存配置、等待工作线程并调用 Vulkan waitIdle。
/// @note waitIdle 仅用于确保频谱纹理不再被 GPU 引用后再销毁。
/// @details 清理顺序必须保持为偏好落盘、后台任务退出、试听轨卸载、
/// GPU 空闲、纹理容器清空；前一步都可能仍引用后一步管理的状态。
BpmMeasurementToolView::~BpmMeasurementToolView()
{
    // 析构时绕过消抖，尽力持久化最后偏好状态。
    flushUserPreferences(true);
    // 后台任务可能访问成员分析缓存，销毁前必须 join。
    stopAnalysisWorker();
    // 独立试听轨不应在窗口对象销毁后继续占用音频图。
    Audio::AudioManager::instance().unloadAuditionTrack();

    auto context = Graphic::VKContext::get();
    if ( context ) {
        // 纹理容器清空前等待所有提交完成，避免释放在途 GPU 资源。
        (void)context->get().getLogicalDevice().waitIdle();
    }
    // 设备空闲后由 RAII 纹理对象安全释放资源。
    m_spectrumTextures.clear();
}

/// @brief 设置测量结果导出回调，用于新建谱面向导等未打开谱面的流程。
/// @param callback 接收当前音频轨道 ID 和 BPM Timing 列表的回调。
/// @note 回调按值保存，可传入空函数以恢复常规应用谱面模式。
/// @warning 回调在 UI 线程触发，不应执行阻塞式长任务。
void BpmMeasurementToolView::setMeasurementExportCallback(
    MeasurementExportCallback callback)
{
    m_measurementExportCallback = std::move(callback);
}

/// @brief 由全局快捷键路由切换 BPM 工具当前音轨的播放状态。
/// @warning UI 输入路径：只在 BPM 工具聚焦且按下无修饰空格时调用。
/// @note 快捷键与播放按钮共享 togglePlayback，确保加载、回绕和错误状态一致。
void BpmMeasurementToolView::togglePlaybackFromShortcut()
{
    (void)togglePlayback();
}

/// @brief 打开窗口并选中指定项目音频轨道。
/// @param audioTrackId 项目内音频资源 ID；为空时选择活动谱面的默认音频。
/// @details 重复打开同一已分析音轨会复用可视化缓存；资源选择变化时
/// 卸载旧试听轨、同步新资源倍速并启动后台分析。该入口始终切换到前台模式。
void BpmMeasurementToolView::openWithAudioTrack(const std::string& audioTrackId)
{
    // 显式打开属于前台交互；覆盖可能残留的后台自动测量标记。
    m_backgroundAutomaticMeasurement = false;
    if ( !m_isOpen ) {
        // 关闭期间配置可能被其他窗口修改，重新打开时以持久化值为准。
        restoreUserPreferences();
    }
    m_isOpen = true;
    // 节拍器资源采用惰性加载，避免应用启动时无条件创建音频资源。
    (void)ensureMetronomeSoundEffects();

    // 调用方未指定音轨时，沿用活动谱面的主音频约定。
    const std::string targetAudioTrackId =
        audioTrackId.empty() ? defaultAudioTrackId() : audioTrackId;
    if ( targetAudioTrackId.empty() ) return;

    // 已有波形可复用；只有选择变化或没有可用分析数据时才重启任务。
    const bool selectionChanged = m_selectedAudioTrackId != targetAudioTrackId;
    const bool needsAnalysis =
        selectionChanged ||
        (m_waveTimes.empty() &&
         !m_analysisRunning.load(std::memory_order_relaxed));
    if ( selectionChanged ) {
        setSelectedAudioTrackId(targetAudioTrackId);
        if ( auto resource = selectedAudioResource() ) {
            // 工具试听速度跟随项目资源配置，同时限制在 UI 支持范围内。
            m_playbackSpeed =
                std::clamp<double>(resource->m_config.playbackSpeed, 0.25, 2.0);
        }
    }
    if ( needsAnalysis ) {
        requestAnalyzeSelectedTrack();
    }
}

/// @brief 请求 BPM 工具窗口在下一次绘制时获得焦点并置于前层。
/// @post 仅设置一次性请求位，不立即调用 ImGui。
/// @note 延迟执行允许菜单动作发生在窗口 Begin 调用之前。
void BpmMeasurementToolView::requestFocus()
{
    m_requestFocus = true;
}

/// @brief 对指定或默认项目音频轨道执行自动 BPM 测量。
/// @param audioTrackId 项目内音频资源 ID；为空时选择默认主音轨。
/// @param keepWindowVisible 测量前窗口已打开时保持可见；否则仅在后台运行。
/// @details 后台模式不构造节拍器音效，也不绘制窗口；结果完成后自动关闭。
/// 前台模式保留窗口并在结果就绪后展示确认或调用向导导出回调。
void BpmMeasurementToolView::requestAutomaticMeasurement(
    const std::string& audioTrackId, bool keepWindowVisible)
{
    // 策略函数阻止已有后台测量被重复请求覆盖。
    if ( !shouldStartBpmAutomaticMeasurement(
             m_backgroundAutomaticMeasurement) ) {
        return;
    }

    if ( keepWindowVisible && !m_isOpen ) {
        // 只有即将显示 UI 时才恢复视图偏好，后台任务无需触碰布局状态。
        restoreUserPreferences();
    }
    m_isOpen                         = true;
    m_backgroundAutomaticMeasurement = !keepWindowVisible;
    if ( keepWindowVisible ) {
        // 纯后台分析不播放节拍器，因此无需加载提示音。
        (void)ensureMetronomeSoundEffects();
    }

    const std::string targetAudioTrackId =
        audioTrackId.empty() ? defaultAudioTrackId() : audioTrackId;
    if ( targetAudioTrackId.empty() ) {
        // 缺少项目音频时保留窗口状态，由调用方决定是否继续显示提示。
        m_statusText = TR("ui.tools.bpm_measure.no_audio").data();
        return;
    }

    setSelectedAudioTrackId(targetAudioTrackId);
    if ( auto resource = selectedAudioResource() ) {
        // 自动测量结果仍应使用该资源约定的试听倍速展示。
        m_playbackSpeed =
            std::clamp<double>(resource->m_config.playbackSpeed, 0.25, 2.0);
    }
    requestAnalyzeSelectedTrack(true);
}

/// @brief 更新并绘制 BPM 测量工具 UI。
/// @param sourceManager 当前 UI 管理器。
/// @warning UI
/// 热路径：每帧执行；常规分支不得扫描文件系统、重新解码整段音频或创建 FFT
/// 计划。只有音轨路径身份变化的低频脏分支可以启动后台分析。
/// @details 每帧顺序固定为：
/// - 在项目稳定时刷新播放路由和选择身份；
/// - 根据身份脏位启动低频分析任务；
/// - 消费已完成的后台结果；
/// - 处理后台自动测量的隐式关闭；
/// - 更新节拍器调度；
/// - 设置窗口首次位置和两栏布局；
/// - 绘制分析区、控制区和模态弹窗；
/// - 根据窗口状态消抖或强制保存偏好。
void BpmMeasurementToolView::update(UIManager* sourceManager)
{
    // 项目切换期间会替换会话和资源列表，禁止读取正在迁移的对象。
    const bool projectTransition =
        sourceManager && sourceManager->isProjectTransitionInProgress();
    if ( !projectTransition ) {
        if ( !m_backgroundAutomaticMeasurement ) {
            // 前台窗口需要根据活动会话动态选择共享或独立播放路由。
            refreshPlaybackRoute();
        }
        if ( m_selectedAudioIdentityNeedsAnalysis &&
             !m_backgroundAutomaticMeasurement ) {
            // 路径或资源身份变化只设置脏位，实际任务统一在 UI 更新点启动。
            requestAnalyzeSelectedTrack();
        }
        // 消费阶段在 UI 线程迁移容器，后台线程只发布完整快照。
        consumePendingAnalysis();
        if ( m_backgroundAutomaticMeasurement ) {
            // acquire 与发布结果的 release 配对，确保结果可见后再判断结束。
            const bool analysisPending =
                m_analysisRunning.load(std::memory_order_relaxed) ||
                m_analysisFinished.load(std::memory_order_acquire);
            if ( !analysisPending ) {
                // 后台模式完成后自动关闭，不让隐藏工具继续参与播放更新。
                m_backgroundAutomaticMeasurement = false;
                m_isOpen                         = false;
            }
            return;
        }
        // 节拍器调度只依赖当前播放位置，必须在分析结果消费后更新。
        updateMetronomePlayback();
    }
    if ( m_backgroundAutomaticMeasurement ) {
        return;
    }

    if ( m_requestFocus ) {
        // 焦点请求延迟到窗口 Begin 前执行，满足 ImGui 的调用时序。
        ImGui::SetNextWindowFocus();
        m_requestFocus = false;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float          dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    if ( viewport ) {
        // 默认尺寸按 DPI 缩放，并限制在主视口工作区内部。
        const ImVec2 defaultWindowSize =
            calculateDefaultBpmMeasureWindowSize(*viewport, dpiScale);
        const ImVec2 defaultWindowPos = calculateDefaultBpmMeasureWindowPos(
            *viewport, defaultWindowSize, dpiScale);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowSize(defaultWindowSize, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(defaultWindowPos, ImGuiCond_Appearing);
    } else {
        // 极早期初始化没有主视口时使用逻辑像素兜底。
        ImGui::SetNextWindowSize(ImVec2(BPM_MEASURE_DEFAULT_WINDOW_WIDTH,
                                        BPM_MEASURE_DEFAULT_WINDOW_HEIGHT),
                                 ImGuiCond_FirstUseEver);
    }
    std::string windowTitle =
        TR("ui.tools.bpm_measure.title").toString() + "###BpmMeasurementTool";
    // 固定 ### ID 使本地化标题变化不会破坏窗口停靠状态。
    LayoutContext layoutContext(
        m_layoutCtx,
        windowTitle,
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse,
        &m_isOpen);

    if ( projectTransition ) {
        // 占位界面避免迁移过程中误触发音轨或谱面操作。
        Utils::renderProjectTransitionPlaceholder();
        return;
    }

    const float contentWidth = ImGui::GetContentRegionAvail().x;
    // 控制栏保持可操作宽度，剩余空间全部交给时频图。
    const float controlsWidth =
        std::clamp(contentWidth * 0.30f, 320.0f, 420.0f);
    if ( ImGui::BeginTable("##BpmMeasureLayout",
                           2,
                           ImGuiTableFlags_Resizable |
                               ImGuiTableFlags_SizingStretchProp |
                               ImGuiTableFlags_BordersInnerV) ) {
        ImGui::TableSetupColumn(
            "##Analysis", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(
            "##Controls", ImGuiTableColumnFlags_WidthFixed, controlsWidth);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if ( ImGui::BeginChild("##BpmMeasureAnalysisChild",
                               ImVec2(0.0f, 0.0f),
                               ImGuiChildFlags_None,
                               ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse) ) {
            // 分析画布自行处理缩放和拖动，禁用 ImGui 子窗口滚轮滚动。
            renderAnalysisPanel();
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        {
            // 右栏内容可纵向滚动，局部样式保证滚动条适配 DPI。
            Utils::VerticalScrollbarStyleScope scrollbarStyle(dpiScale);
            if ( ImGui::BeginChild("##BpmMeasureControlsChild",
                                   ImVec2(0.0f, 0.0f),
                                   ImGuiChildFlags_None,
                                   ImGuiWindowFlags_AlwaysVerticalScrollbar) ) {
                renderControlPanel();
            }
            ImGui::EndChild();
        }

        ImGui::EndTable();
    }

    renderAutoApplyOffsetPopup();
    renderApplyTimingPopup();

    if ( !m_isOpen ) {
        // 关闭是明确提交点：立即保存偏好并释放独立试听轨。
        flushUserPreferences(true);
        Audio::AudioManager::instance().unloadAuditionTrack();
    } else {
        // 打开期间采用消抖保存，避免拖动控件时频繁写配置。
        flushUserPreferences(false);
    }
}

/// @brief 判断频谱纹理是否需要上传。
/// @warning 渲染准备热路径：每帧查询；只读取低频变更脏位。
/// @return 待释放旧纹理、待上传新分块或重载进行中时返回 true。
bool BpmMeasurementToolView::needReload()
{
    return m_texturesNeedReload;
}

/// @brief 上传后台分析生成的频谱纹理。
/// @param physicalDevice Vulkan 物理设备。
/// @param logicalDevice Vulkan 逻辑设备。
/// @param cmdPool 上传命令池。
/// @param queue 上传队列。
/// @warning 低频资源准备路径：可能等待 GPU
/// 空闲并上传纹理，只能由音轨切换或重新分析触发。
/// @details 首帧等待设备空闲并释放旧纹理，后续每帧至多上传一个分块。
/// 无效分块会被跳过但仍推进游标；全部完成后才清除纹理重载脏位。
void BpmMeasurementToolView::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                            vk::Device&         logicalDevice,
                                            vk::CommandPool&    cmdPool,
                                            vk::Queue&          queue)
{
    if ( !m_texturesNeedReload ) {
        return;
    }

    if ( !m_spectrumTextureReloadStarted ) {
        // 旧纹理可能仍被上一帧命令引用，重载起点必须先等待设备空闲。
        (void)logicalDevice.waitIdle();
        // 以待上传块数预留容器，避免逐块上传期间反复扩容。
        m_spectrumTextures.clear();
        m_spectrumTextures.reserve(m_pendingSpectrumChunks.size());
        m_nextSpectrumChunkUploadIndex = 0;
        m_spectrumTextureReloadStarted = true;
    }

    constexpr size_t MAX_UPLOAD_CHUNKS_PER_FRAME = 1;
    // 每帧仅上传一个分块，控制大音频频谱重建造成的帧时间尖峰。
    size_t uploadedThisFrame = 0;
    while ( m_nextSpectrumChunkUploadIndex < m_pendingSpectrumChunks.size() &&
            uploadedThisFrame < MAX_UPLOAD_CHUNKS_PER_FRAME ) {
        const auto& chunk =
            m_pendingSpectrumChunks[m_nextSpectrumChunkUploadIndex];
        ++m_nextSpectrumChunkUploadIndex;
        ++uploadedThisFrame;

        if ( chunk.pixels.empty() || chunk.width == 0 || chunk.height == 0 ) {
            // 无效块仍需推进索引，避免坏数据让重载流程永久停滞。
            continue;
        }

        // VKTexture 构造负责暂存缓冲和布局转换，容器持有最终 GPU 资源。
        m_spectrumTextures.push_back(
            std::make_unique<Graphic::VKTexture>(chunk.pixels.data(),
                                                 chunk.width,
                                                 chunk.height,
                                                 physicalDevice,
                                                 logicalDevice,
                                                 cmdPool,
                                                 queue));
    }

    if ( m_nextSpectrumChunkUploadIndex >= m_pendingSpectrumChunks.size() ) {
        // 所有分块完成后才清除脏位，确保渲染端不会看到半完成状态为就绪。
        m_pendingSpectrumChunks.clear();
        m_nextSpectrumChunkUploadIndex = 0;
        m_spectrumTextureReloadStarted = false;
        m_texturesNeedReload           = false;
    }
}

/// @brief 尝试消费后台分析结果，并把完整快照转移到 UI 状态。
/// @warning UI 热路径：每帧轮询一个原子标志；仅在任务完成时短暂加锁并迁移容器。
/// @details 普通分析只更新波形、频谱和状态；自动分析还会建立首个
/// Timing 段、移动视图中心、重置节拍器，并按调用环境选择直接导出或确认弹窗。
void BpmMeasurementToolView::consumePendingAnalysis()
{
    // acquire 确保能观察到后台线程在发布标志前完成的结果写入。
    if ( !m_analysisFinished.load(std::memory_order_acquire) ) {
        return;
    }

    std::optional<AnalysisResult> result;
    {
        // 锁只保护待交付槽位，昂贵的数据处理不在临界区执行。
        std::lock_guard<std::mutex> lock(m_pendingResultMutex);
        if ( m_pendingResult ) {
            result = std::move(m_pendingResult);
            m_pendingResult.reset();
        }
    }

    if ( !result ) {
        // 防御性处理空发布，复位标志以免每帧重复进入消费路径。
        m_analysisFinished.store(false, std::memory_order_release);
        return;
    }

    // 一次性替换波形、频谱和时长，避免画布混用不同分析批次的数据。
    m_waveTimes = std::move(result->waveTimes);
    // 画布时间含可视偏移，源时间变化后必须延迟重算。
    m_waveCanvasTimes.clear();
    m_waveCanvasTimesOffset        = std::numeric_limits<double>::quiet_NaN();
    m_waveMin                      = std::move(result->waveMin);
    m_waveMax                      = std::move(result->waveMax);
    m_pendingSpectrumChunks        = std::move(result->spectrumChunks);
    m_duration                     = result->duration;
    m_spectrumSegmentsPerSecond    = result->spectrumSegmentsPerSecond;
    m_spectrumSegmentCount         = result->spectrumSegmentCount;
    m_spectrumBinCount             = result->spectrumBinCount;
    m_nextSpectrumChunkUploadIndex = 0;
    // GPU 上传在纹理加载回调分帧执行，消费结果只准备 CPU 数据。
    m_spectrumTextureReloadStarted = false;
    m_texturesNeedReload           = true;

    if ( result->failed ) {
        // 自动测量与普通加载使用不同提示，便于调用方区分失败阶段。
        m_statusText = result->autoTimingRequested
                           ? TR("ui.tools.bpm_measure.auto_failed").data()
                           : TR("ui.tools.bpm_measure.load_failed").data();
        m_analysisFinished.store(false, std::memory_order_release);
        return;
    }

    if ( result->autoTimingRequested ) {
        if ( result->autoTimingResult ) {
            // 自动结果先归一化 BPM，再由拍长约束首拍允许的负偏移范围。
            const auto& autoTiming = *result->autoTimingResult;
            m_bpm                  = ::MMM::normalizeBpmValue(autoTiming.bpm);
            m_beatLengthSeconds    = 60.0 / m_bpm;
            m_firstBeatTime = clampFirstBeatTime(autoTiming.offsetMs / 1000.0,
                                                 m_beatLengthSeconds,
                                                 playbackCanvasDuration());
            m_timingSegments.clear();
            // 自动算法只产生一个基准段；后续变速段由用户显式添加。
            m_timingSegments.push_back({ m_firstBeatTime, m_bpm });
            normalizeTimingSegments();
            m_viewCenter = std::clamp<double>(
                m_firstBeatTime, 0.0, std::max(0.0, playbackCanvasDuration()));
            resetMetronomeScheduler(m_viewCenter);
            // 精度同时显示毫秒和拍分数，便于不同 BPM 下比较误差。
            const std::string inaccuracyFraction = formatBeatFractionInaccuracy(
                autoTiming.alignmentInaccuracyMs, m_beatLengthSeconds);
            m_statusText = TR_FMT("ui.tools.bpm_measure.auto_ready",
                                  m_bpm,
                                  autoTiming.offsetMs,
                                  inaccuracyFraction,
                                  autoTiming.rawBpm,
                                  autoTiming.signature,
                                  autoTiming.division);
            if ( m_measurementExportCallback ) {
                // 向导模式没有已打开谱面，直接回传而不显示应用弹窗。
                exportMeasuredTimingsToCallback(false);
            } else {
                // 常规工具要求用户确认目标谱面，避免误写活动标签页。
                m_shouldOpenAutoApplyPopup = true;
            }
        } else {
            m_statusText = TR("ui.tools.bpm_measure.auto_failed").data();
        }
    } else {
        // 普通分析只准备可视化数据，不覆盖用户已有的手工 Timing。
        m_statusText = TR("ui.tools.bpm_measure.ready").data();
    }
    m_analysisFinished.store(false, std::memory_order_release);
}

/// @brief 确保至少存在一个 BPM 段落，并同步旧单段字段。
/// @post 调用后 m_timingSegments 非空，首段与兼容字段保持一致。
/// @note 只有空列表才创建段；已有列表不会在此重排或覆盖用户值。
void BpmMeasurementToolView::ensureTimingSegments()
{
    if ( m_timingSegments.empty() ) {
        // 兼容旧单段字段，首次打开段落 UI 时提升为统一列表表示。
        m_timingSegments.push_back({ m_firstBeatTime, m_bpm });
        normalizeTimingSegments();
    }
}

/// @brief 归一化 BPM 段落列表，保持按时间排序且 BPM 位于 0.1–10000.0。
/// @note 相同时间戳的后项覆盖前项，使一次拖动或新增操作得到确定结果。
/// @details 归一化依次执行无效项过滤、非空兜底、逐段参数钳制、
/// 稳定排序、重复起点合并及首段兼容字段同步。调用后列表满足：
/// - 至少存在一个段；
/// - 所有 BPM 都能转换为有限正拍长；
/// - 时间戳位于允许的画布范围；
/// - 起点严格按时间升序；
/// - 同一微秒附近只保留一个段。
void BpmMeasurementToolView::normalizeTimingSegments()
{
    const double canvasDuration = playbackCanvasDuration();
    // 非有限值无法参与排序，非正 BPM 也不能转换为有效拍长。
    std::erase_if(m_timingSegments, [](const auto& segment) {
        return !std::isfinite(segment.timestampSeconds) ||
               !std::isfinite(segment.bpm) || segment.bpm <= 0.0;
    });

    if ( m_timingSegments.empty() ) {
        // 列表必须始终保留基准段，供旧字段和节拍线绘制共同使用。
        m_timingSegments.push_back({ m_firstBeatTime, m_bpm });
    }

    for ( auto& segment : m_timingSegments ) {
        // 每段分别限制 BPM，并允许首拍向零点前延伸至一个拍长。
        segment.bpm = ::MMM::normalizeBpmValue(segment.bpm);
        segment.timestampSeconds =
            std::clamp(segment.timestampSeconds,
                       firstBeatMinSeconds(60.0 / segment.bpm),
                       std::max(0.0, canvasDuration));
    }

    // 稳定排序保留同时间戳项的输入次序，便于确定覆盖优先级。
    std::stable_sort(m_timingSegments.begin(),
                     m_timingSegments.end(),
                     [](const auto& lhs, const auto& rhs) {
                         return lhs.timestampSeconds < rhs.timestampSeconds;
                     });

    std::vector<BpmTimingSegment> normalized;
    // 最坏情况下全部保留，预留原容量避免归一化过程再次分配。
    normalized.reserve(m_timingSegments.size());
    for ( const auto& segment : m_timingSegments ) {
        if ( !normalized.empty() &&
             std::abs(normalized.back().timestampSeconds -
                      segment.timestampSeconds) < 1e-6 ) {
            // 微秒级相同起点无法形成可见区间，以后写入的段为准。
            normalized.back() = segment;
            continue;
        }
        normalized.push_back(segment);
    }
    // 替换后统一回写兼容字段，确保所有旧绘制路径看到同一基准段。
    m_timingSegments = std::move(normalized);
    syncPrimaryTimingFieldsFromSegments();
}

/// @brief 从第一段同步兼容旧绘制/输入路径的字段。
/// @post 非空列表时 BPM、拍长和首拍三个字段来自同一首段。
/// @note 此函数不修改段落列表，也不重置节拍器调度。
/// @details m_beatLengthSeconds 始终由归一化 BPM 反算，而不是信任旧缓存，
/// 使三者在段起点拖动和 BPM 拖动之后保持代数一致。
void BpmMeasurementToolView::syncPrimaryTimingFieldsFromSegments()
{
    if ( m_timingSegments.empty() ) {
        return;
    }
    // 第一段定义全局显示的 BPM、首拍和拍长三联字段。
    m_firstBeatTime = m_timingSegments.front().timestampSeconds;
    m_bpm           = ::MMM::normalizeBpmValue(m_timingSegments.front().bpm);
    m_beatLengthSeconds = 60.0 / m_bpm;
}

/// @brief 将兼容旧输入路径的字段写回第一段。
/// @post 首段承接当前 BPM 与首拍，随后整个列表满足归一化不变量。
/// @note 供仍使用单段控件的输入路径过渡到多段模型。
void BpmMeasurementToolView::syncPrimaryTimingFieldsToSegments()
{
    if ( m_timingSegments.empty() ) {
        // 旧控件可能先于段落面板修改字段，此时创建首段承接修改。
        m_timingSegments.push_back({ m_firstBeatTime, m_bpm });
    } else {
        // BPM 与首拍控件只编辑首段，其他变速段保持不变。
        m_timingSegments.front().timestampSeconds = m_firstBeatTime;
        m_timingSegments.front().bpm              = m_bpm;
    }
    normalizeTimingSegments();
}

/// @brief 查找指定时间所在的 BPM 段落索引。
/// @param timeSeconds 查询时间，单位为秒。
/// @return 段落索引。
/// @pre 段落列表已按时间升序归一化。
/// @details 返回最后一个起点不晚于查询时间的段；查询早于首段时回退
/// 索引零，使首段网格可以向零点前延伸。
std::size_t BpmMeasurementToolView::findSegmentIndexForTime(
    double timeSeconds) const
{
    if ( m_timingSegments.empty() ) {
        return 0;
    }

    std::size_t result = 0;
    // 有序列表中最后一个不晚于查询时间的段即为生效段。
    for ( std::size_t i = 0; i < m_timingSegments.size(); ++i ) {
        if ( timeSeconds + 1e-9 < m_timingSegments[i].timestampSeconds ) {
            break;
        }
        result = i;
    }
    return result;
}

/// @brief 获取段落拍长。
/// @param segmentIndex 段落索引。
/// @return 按 0.1–10000.0 BPM 计算的单拍时长，单位为秒。
/// @details 空列表回退兼容字段，越界索引钳制到末段，BPM 再次通过
/// 公共归一化入口后换算，避免调用者传播非法拍长。
double BpmMeasurementToolView::segmentBeatLengthSeconds(
    std::size_t segmentIndex) const
{
    if ( m_timingSegments.empty() ) {
        // 段落尚未初始化时沿用兼容字段，避免调用方额外分支。
        return m_beatLengthSeconds;
    }
    const auto& segment =
        m_timingSegments[std::min(segmentIndex, m_timingSegments.size() - 1)];
    return 60.0 / ::MMM::normalizeBpmValue(segment.bpm);
}

/// @brief 将当前段落列表按 0.1–10000.0 BPM 转换为可写入谱面的 Timing。
/// @return 与段落顺序一致、单位已转换为毫秒的 BPM Timing 列表。
/// @details 每个段生成一个 TimingEffect::BPM，时间从秒转换为毫秒，
/// beat_length 从归一化 BPM 反算，兼容字段 m_bpm 与参数值保持一致。
std::vector<::MMM::Timing> BpmMeasurementToolView::makeMeasuredTimings() const
{
    std::vector<::MMM::Timing> timings;
    // 一段对应一个 Timing，提前预留避免构造 DTO 时扩容。
    timings.reserve(m_timingSegments.size());
    for ( const auto& segment : m_timingSegments ) {
        // 写出前再次归一化，保证即使调用发生在 UI 提交前也不会越界。
        const double  bpm = ::MMM::normalizeBpmValue(segment.bpm);
        ::MMM::Timing timing;
        // 谱面模型使用毫秒，而工具画布内部统一使用秒。
        timing.m_timestamp             = segment.timestampSeconds * 1000.0;
        timing.m_timingEffect          = ::MMM::TimingEffect::BPM;
        timing.m_timingEffectParameter = bpm;
        timing.m_bpm                   = bpm;
        timing.m_beat_length           = 60000.0 / bpm;
        timings.push_back(timing);
    }
    return timings;
}

/// @brief 将当前测量 Timing 通过外部回调导出。
/// @param updateStatus 是否覆盖当前工具状态文本。
/// @details 回调只在向导等外部流程显式注入后可用；导出前确保段列表
/// 非空并转换为值语义 DTO，回调不获得内部容器引用。
void BpmMeasurementToolView::exportMeasuredTimingsToCallback(bool updateStatus)
{
    if ( !m_measurementExportCallback ) {
        // 常规窗口没有导出接收方，应用谱面入口负责后续处理。
        return;
    }

    ensureTimingSegments();
    auto timings = makeMeasuredTimings();
    if ( timings.empty() ) {
        // 理论上 ensure 已创建首段，此保护避免未来过滤逻辑传递空结果。
        return;
    }

    // 同时传递音轨 ID，向导可把 Timing 绑定到正确的资源。
    m_measurementExportCallback(m_selectedAudioTrackId, timings);
    if ( updateStatus ) {
        m_statusText = TR("ui.tools.bpm_measure.export_done").data();
    }
}

/// @brief 收集当前已打开且可写入的谱面列表。
/// @return 可用于应用弹窗的会话索引、相机 ID 与显示名称快照。
/// @details 欢迎页、空会话和未加载谱面的条目被排除；显示名称优先使用
/// 会话标签，缺失时使用谱面名称，并附加非空难度版本以区分同曲谱面。
std::vector<BpmMeasurementToolView::OpenBeatmapApplyOption>
BpmMeasurementToolView::collectApplyBeatmapOptions() const
{
    std::vector<OpenBeatmapApplyOption> options;
    // 会话条目是快照，弹窗每帧重建以跟随标签页打开和关闭。
    const auto entries = Logic::EditorEngine::instance().getSessionEntries();
    for ( std::size_t i = 0; i < entries.size(); ++i ) {
        const auto& entry = entries[i];
        if ( entry.isLogoPlaceholder || !entry.session ) {
            // 欢迎页和失效会话没有可写入的谱面模型。
            continue;
        }

        const auto& ctx = entry.session->getContext();
        if ( !ctx.currentBeatmap ) {
            // 项目容器已打开但尚未加载谱面的会话同样不可应用。
            continue;
        }

        std::string label = entry.displayName;
        if ( label.empty() ) {
            // 标签标题为空时使用谱面元数据，保证组合框始终可辨识。
            label = ctx.currentBeatmap->m_baseMapMetadata.name;
        }
        if ( !ctx.currentBeatmap->m_baseMapMetadata.version.empty() ) {
            // 难度名附在主名称后，区分同一歌曲的多个已打开谱面。
            label += " [" + ctx.currentBeatmap->m_baseMapMetadata.version + "]";
        }

        options.push_back({ static_cast<int32_t>(i), entry.cameraId, label });
    }
    return options;
}

/// @brief 请求打开应用到谱面的弹窗。
/// @post 下一次弹窗绘制会消费请求并打开 Modal。
/// @note 请求阶段只确保 Timing 数据有效，不提前枚举会话候选。
void BpmMeasurementToolView::requestOpenApplyTimingPopup()
{
    // 弹窗只处理已规范化的段落，防止应用过程中重新解释 UI 临时值。
    ensureTimingSegments();
    m_shouldOpenApplyTimingPopup = true;
}

/// @brief 将测量结果应用到当前弹窗选中的谱面。
/// @warning 用户确认的低频路径：会切换活动会话并投递可撤销命令。
/// @details 目标会话索引来自弹窗的最新候选快照；提交前同时请求会话
/// 聚焦，使后续画布反馈、撤销栈和状态文本指向同一谱面。
void BpmMeasurementToolView::applyMeasuredTimingsToSelectedBeatmap()
{
    if ( m_applyTargetSessionIndex < 0 ) {
        // 没有有效选择时不得把命令隐式投递给当前活动会话。
        return;
    }

    auto& engine = Logic::EditorEngine::instance();
    // 先激活并聚焦目标，使命令栈、画布和后续提示指向同一会话。
    engine.setActiveSessionIndex(m_applyTargetSessionIndex);
    engine.requestSessionFocus(m_applyTargetSessionIndex);
    // 通过可撤销命令替换 Timing，保留非 BPM 项的策略由复选框决定。
    engine.pushCommand(Logic::CmdReplaceBeatmapTimings{
        makeMeasuredTimings(), m_keepNonBpmTimingsOnApply });
    m_statusText = TR("ui.tools.bpm_measure.apply_done").data();
}

/// @brief 绘制右侧测量参数面板。
/// @warning UI 热路径：每帧绘制；仅遍历项目音频资源和少量 Timing 段。
/// @details 控制栏按职责分为：
/// - 音轨选择、重载和自动测量；
/// - 播放、暂停、位置与倍速；
/// - BPM、拍长、首拍和标记显示；
/// - 视野中心与缩放；
/// - 变速 Timing 段维护和应用；
/// - 后台分析进度及最终状态。
void BpmMeasurementToolView::renderControlPanel()
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) {
        // 没有项目时任何音轨和应用操作都无有效上下文。
        ImGui::TextColored(Utils::UIThemeUtils::getWarningColor(),
                           "%s",
                           TR("ui.tools.no_active_session").data());
        return;
    }
    ensureTimingSegments();

    if ( m_selectedAudioTrackId.empty() ) {
        // 选择为空时不尝试解析资源，直接显示引导文案。
        m_selectedAudioLabel = TR("ui.tools.bpm_measure.select_track").data();
    } else {
        // ID 是持久身份，标签每帧从项目资源刷新以反映重命名。
        bool foundSelected = false;
        for ( const auto& resource : project->m_audioResources ) {
            if ( resource.m_id == m_selectedAudioTrackId ) {
                m_selectedAudioLabel = makeAudioResourceLabel(resource);
                foundSelected        = true;
                break;
            }
        }
        if ( !foundSelected ) {
            // 项目资源已删除时清空选择及其关联分析状态。
            setSelectedAudioTrackId({});
            m_selectedAudioLabel =
                TR("ui.tools.bpm_measure.select_track").data();
        }
    }

    ImGui::SeparatorText(TR("ui.tools.bpm_measure.audio").data());
    if ( ::MMM::UI::FeedbackBeginCombo("##BpmMeasureAudioTrack",
                                       m_selectedAudioLabel.c_str()) ) {
        // 组合框按项目顺序呈现，避免另行排序改变用户熟悉的资源次序。
        for ( const auto& resource : project->m_audioResources ) {
            const bool  isSelected = resource.m_id == m_selectedAudioTrackId;
            std::string label      = makeAudioResourceLabel(resource);
            if ( ::MMM::UI::FeedbackSelectable(label.c_str(), isSelected) ) {
                // 选择变更同步试听倍速，并立即为新资源启动分析。
                setSelectedAudioTrackId(resource.m_id);
                m_selectedAudioLabel = label;
                m_playbackSpeed      = std::clamp<double>(
                    resource.m_config.playbackSpeed, 0.25, 2.0);
                requestAnalyzeSelectedTrack();
            }
            if ( isSelected ) {
                // 打开列表时滚动到当前资源，长资源列表仍能快速定位。
                ImGui::SetItemDefaultFocus();
            }
        }
        ::MMM::UI::FeedbackEndCombo();
    }

    if ( project->m_audioResources.empty() ) {
        ImGui::TextDisabled("%s", TR("ui.tools.bpm_measure.no_audio").data());
    }

    const bool hasSelection = !m_selectedAudioTrackId.empty();
    if ( !hasSelection ) {
        // 重载和自动测量都需要明确音轨，禁用而非静默失败。
        ImGui::BeginDisabled();
    }
    if ( ::MMM::UI::FeedbackButton(TR("ui.tools.bpm_measure.reload").data(),
                                   ImVec2(-1.0f, 0.0f)) ) {
        requestAnalyzeSelectedTrack();
    }
    if ( ::MMM::UI::FeedbackButton(
             TR("ui.tools.bpm_measure.auto_button").data(),
             ImVec2(-1.0f, 0.0f)) ) {
        requestAutoMeasureSelectedTrack();
    }
    if ( !hasSelection ) {
        ImGui::EndDisabled();
    }

    renderPlaybackControls();

    ImGui::Spacing();
    ImGui::SeparatorText(TR("ui.tools.bpm_measure.params").data());

    float bpm = static_cast<float>(m_bpm);
    if ( ::MMM::UI::FeedbackDragFloat(
             TR("ui.tools.bpm_measure.bpm").data(),
             &bpm,
             0.01f,
             static_cast<float>(::MMM::MIN_NORMALIZED_BPM),
             static_cast<float>(::MMM::MAX_NORMALIZED_BPM),
             "%.3f") ) {
        // BPM 是主编辑量；拍长和允许的首拍负偏移需同步重算。
        m_bpm               = ::MMM::normalizeBpmValue(bpm);
        m_beatLengthSeconds = 60.0 / m_bpm;
        m_firstBeatTime     = clampFirstBeatTime(
            m_firstBeatTime, m_beatLengthSeconds, playbackCanvasDuration());
        syncPrimaryTimingFieldsToSegments();
    }

    // 拍长输入范围严格由公共 BPM 归一化范围反推，避免两控件不一致。
    constexpr double minBeatLength = 60.0 / ::MMM::MAX_NORMALIZED_BPM;
    constexpr double maxBeatLength = 60.0 / ::MMM::MIN_NORMALIZED_BPM;
    float            beatLength    = static_cast<float>(m_beatLengthSeconds);
    if ( ::MMM::UI::FeedbackDragFloat(
             TR("ui.tools.bpm_measure.beat_length").data(),
             &beatLength,
             0.0001f,
             static_cast<float>(minBeatLength),
             static_cast<float>(maxBeatLength),
             "%.6f") ) {
        // 编辑拍长时反向计算 BPM，最终仍写回统一的首段模型。
        m_beatLengthSeconds =
            std::clamp<double>(beatLength, minBeatLength, maxBeatLength);
        m_bpm           = 60.0 / m_beatLengthSeconds;
        m_firstBeatTime = clampFirstBeatTime(
            m_firstBeatTime, m_beatLengthSeconds, playbackCanvasDuration());
        syncPrimaryTimingFieldsToSegments();
    }

    // 允许首拍位于音频零点前一拍，支持带前导空白的节拍网格对齐。
    const double minFirstBeat = firstBeatMinSeconds(m_beatLengthSeconds);
    float        firstBeat    = static_cast<float>(m_firstBeatTime);
    if ( ::MMM::UI::FeedbackDragFloat(
             TR("ui.tools.bpm_measure.first_beat").data(),
             &firstBeat,
             0.001f,
             static_cast<float>(minFirstBeat),
             static_cast<float>(std::max(0.001, playbackCanvasDuration())),
             "%.6f") ) {
        // 首拍变化不改变 BPM，只更新段起点并重新规范化列表。
        m_firstBeatTime = clampFirstBeatTime(
            firstBeat, m_beatLengthSeconds, playbackCanvasDuration());
        syncPrimaryTimingFieldsToSegments();
    }

    float markerWidth = static_cast<float>(m_markerWidthMs);
    if ( ::MMM::UI::FeedbackDragFloat(
             TR("ui.tools.bpm_measure.marker_width").data(),
             &markerWidth,
             0.1f,
             4.0f,
             1000.0f,
             "%.1f") ) {
        // 标记宽度只影响视觉显示，走显示偏好的持久化分组。
        m_markerWidthMs = std::clamp<double>(markerWidth, 4.0, 1000.0);
        markUserPreferencesChanged(true, false);
    }

    int beatDivisor = m_beatDivisor;
    if ( ::MMM::UI::FeedbackSliderInt(
             TR("ui.tools.bpm_measure.beat_divisor").data(),
             &beatDivisor,
             1,
             64) ) {
        // 细分上限限制为 64，避免高缩放级别产生过密绘制循环。
        m_beatDivisor = std::clamp(beatDivisor, 1, 64);
        markUserPreferencesChanged(true, false);
    }

    ImGui::Spacing();
    ImGui::SeparatorText(TR("ui.tools.bpm_measure.view").data());

    float center = static_cast<float>(m_viewCenter);
    if ( ::MMM::UI::FeedbackDragFloat(
             TR("ui.tools.bpm_measure.center").data(),
             &center,
             0.01f,
             0.0f,
             static_cast<float>(std::max(0.001, playbackCanvasDuration())),
             "%.3f") ) {
        // 视图中心独立于播放位置；只有跟随播放模式会持续覆盖它。
        m_viewCenter = std::clamp<double>(
            center, 0.0, std::max(0.0, playbackCanvasDuration()));
        markUserPreferencesChanged(false, true);
    }

    float zoom = static_cast<float>(m_zoomSeconds);
    if ( ::MMM::UI::FeedbackDragFloat(TR("ui.tools.bpm_measure.zoom").data(),
                                      &zoom,
                                      0.05f,
                                      0.1f,
                                      120.0f,
                                      "%.2f") ) {
        // zoom 表示整个可见时间跨度，统一限制在交互可用范围内。
        m_zoomSeconds = std::clamp<double>(zoom, 0.1, 120.0);
        markUserPreferencesChanged(false, true);
    }

    if ( ::MMM::UI::FeedbackButton(
             TR("ui.tools.bpm_measure.center_first").data(),
             ImVec2(-1.0f, 0.0f)) ) {
        // 居中首拍只改变视野，不对当前播放 transport 发起 seek。
        m_viewCenter = std::clamp<double>(
            m_firstBeatTime, 0.0, std::max(0.0, playbackCanvasDuration()));
        markUserPreferencesChanged(false, true);
    }

    renderTimingSegmentsPanel();

    ImGui::Spacing();
    ImGui::SeparatorText(TR("ui.tools.bpm_measure.status").data());
    if ( m_analysisRunning.load(std::memory_order_relaxed) ) {
        // 进度只用于提示，无需与结果发布建立跨线程顺序关系。
        const float progress =
            m_analysisProgress.load(std::memory_order_relaxed);
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));
        ImGui::Text("%s %.0f%%",
                    TR("ui.tools.bpm_measure.analyzing").data(),
                    progress * 100.0f);
    } else {
        ImGui::TextWrapped("%s", m_statusText.c_str());
    }

    if ( m_duration > 0.0 ) {
        // 时长来自分析结果，作为诊断信息而非可编辑参数显示。
        ImGui::TextDisabled(
            "%s %.3fs", TR("ui.tools.bpm_measure.duration").data(), m_duration);
    }
}

/// @brief 绘制 BPM 段落列表和应用入口。
/// @warning UI 热路径：每帧遍历当前段落；删除会中止本帧遍历以避免引用失效。
/// @details 第一段作为基准不可删除；新增段继承当前视野处的 BPM；
/// 所有编辑在本帧末尾统一归一化并重置节拍器。应用和向导导出共享
/// 同一个标准化 Timing 转换入口。
void BpmMeasurementToolView::renderTimingSegmentsPanel()
{
    ImGui::Spacing();
    ImGui::SeparatorText(TR("ui.tools.bpm_measure.segments").data());

    // 面板显示约三至四行并启用内部滚动，防止变速谱面挤占全部控制栏。
    const float rowHeight = ImGui::GetFrameHeightWithSpacing();
    const float childHeight =
        std::min(170.0f, std::max(rowHeight * 3.0f, rowHeight * 4.5f));
    bool changed = false;
    {
        // 滚动条样式仅作用于段落子区域，离开作用域自动恢复。
        Utils::VerticalScrollbarStyleScope scrollbarStyle;
        if ( ImGui::BeginChild("##BpmMeasureSegments",
                               ImVec2(0.0f, childHeight),
                               ImGuiChildFlags_Borders) ) {
            for ( std::size_t i = 0; i < m_timingSegments.size(); ++i ) {
                // 索引作为 ImGui ID，归一化排序后下一帧自然重建稳定控件。
                auto& segment = m_timingSegments[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::Text("#%zu", i + 1);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(78.0f);
                float time = static_cast<float>(segment.timestampSeconds);
                if ( ::MMM::UI::FeedbackDragFloat(
                         "##SegmentTime",
                         &time,
                         0.001f,
                         static_cast<float>(firstBeatMinSeconds(
                             60.0 / ::MMM::normalizeBpmValue(segment.bpm))),
                         static_cast<float>(
                             std::max(0.001, playbackCanvasDuration())),
                         "%.3fs") ) {
                    // 拖动期间暂存原始值，循环结束后统一排序和限幅。
                    segment.timestampSeconds = time;
                    changed                  = true;
                }
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip(
                        "%s", TR("ui.tools.bpm_measure.segment_time").data());
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(76.0f);
                float bpm = static_cast<float>(segment.bpm);
                if ( ::MMM::UI::FeedbackDragFloat(
                         "##SegmentBpm",
                         &bpm,
                         0.01f,
                         static_cast<float>(::MMM::MIN_NORMALIZED_BPM),
                         static_cast<float>(::MMM::MAX_NORMALIZED_BPM),
                         "%.3f") ) {
                    // BPM 的最终公共范围由 normalizeTimingSegments 统一施加。
                    segment.bpm = bpm;
                    changed     = true;
                }
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip(
                        "%s", TR("ui.tools.bpm_measure.segment_bpm").data());
                }
                ImGui::SameLine();
                if ( i == 0 ) {
                    // 第一段是基准 Timing，不允许删除以维持非空不变量。
                    ImGui::BeginDisabled();
                }
                pushBpmCompactTextButtonStyleVars();
                const bool deleteClicked = ::MMM::UI::FeedbackSmallButton(
                    TR("ui.common.delete").data());
                popBpmCompactTextButtonStyleVars();
                if ( deleteClicked ) {
                    // erase 会使当前引用及后续索引失效，因此立即结束循环。
                    m_timingSegments.erase(m_timingSegments.begin() +
                                           static_cast<std::ptrdiff_t>(i));
                    changed = true;
                    if ( i == 0 ) {
                        ImGui::EndDisabled();
                    }
                    ImGui::PopID();
                    break;
                }
                if ( i == 0 ) {
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    if ( changed ) {
        // 批量提交本帧编辑后再重排，并重置依赖段边界的节拍器游标。
        normalizeTimingSegments();
        resetMetronomeScheduler(m_viewCenter);
    }

    if ( ::MMM::UI::FeedbackButton(
             TR("ui.tools.bpm_measure.add_segment").data(),
             ImVec2(-1.0f, 0.0f)) ) {
        // 新段继承当前视野位置生效的 BPM，减少手工复制参数。
        const std::size_t sourceIndex = findSegmentIndexForTime(m_viewCenter);
        const double      bpm         = m_timingSegments.empty()
                                            ? m_bpm
                                            : m_timingSegments[sourceIndex].bpm;
        m_timingSegments.push_back(
            // 段起点必须位于当前音频画布范围内。
            { std::clamp<double>(
                  m_viewCenter, 0.0, std::max(0.0, playbackCanvasDuration())),
              bpm });
        normalizeTimingSegments();
        resetMetronomeScheduler(m_viewCenter);
    }

    // 是否保留 Scroll 等非 BPM Timing 会直接传给可撤销替换命令。
    ::MMM::UI::FeedbackCheckbox(TR("ui.tools.bpm_measure.keep_scroll").data(),
                                &m_keepNonBpmTimingsOnApply);
    if ( ::MMM::UI::FeedbackButton(
             TR("ui.tools.bpm_measure.apply_to_beatmap").data(),
             ImVec2(-1.0f, 0.0f)) ) {
        requestOpenApplyTimingPopup();
    }
    if ( m_measurementExportCallback ) {
        // 新建谱面向导注入回调后额外显示导出入口。
        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.tools.bpm_measure.export_to_wizard").data(),
                 ImVec2(-1.0f, 0.0f)) ) {
            exportMeasuredTimingsToCallback(true);
        }
    }
}

/// @brief 绘制自动测偏移后的应用确认弹窗。
/// @note 此弹窗只确认是否进入目标谱面选择，不直接写入任何会话。
/// @details 打开请求来自后台结果消费阶段，实际 OpenPopup 延迟到 UI 绘制。
/// “应用”只排队打开下一层目标选择；“取消”保留工具内测量结果供继续调整。
void BpmMeasurementToolView::renderAutoApplyOffsetPopup()
{
    const char* popupTitle = TR("ui.tools.bpm_measure.auto_apply_title").data();
    if ( m_shouldOpenAutoApplyPopup ) {
        // OpenPopup 必须在绘制线程触发，后台结果通过布尔请求延迟到此处。
        ::MMM::UI::FeedbackOpenPopup(popupTitle);
        m_shouldOpenAutoApplyPopup = false;
    }

    if ( ImGui::BeginPopupModal(
             popupTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize) ) {
        // 自动结果先供用户检查；应用按钮继续进入明确的目标选择流程。
        ImGui::TextWrapped(
            "%s", TR("ui.tools.bpm_measure.auto_apply_message").data());
        ImGui::Spacing();
        const float buttonWidth = 120.0f;
        if ( ::MMM::UI::FeedbackButton(TR("ui.common.apply").data(),
                                       ImVec2(buttonWidth, 0.0f)) ) {
            // 关闭当前确认框后，下一弹窗请求会在本帧末尾处理。
            requestOpenApplyTimingPopup();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                       ImVec2(buttonWidth, 0.0f)) ) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/// @brief 绘制将测量结果应用到已打开谱面的弹窗。
/// @warning UI 热路径：弹窗打开时按会话快照重建候选列表，不持有会话所有权。
/// @details 弹窗持续验证先前选择的会话索引；目标失效时回退首个候选，
/// 所有候选消失时禁用应用按钮。真正修改通过可撤销命令提交。
void BpmMeasurementToolView::renderApplyTimingPopup()
{
    const char* popupTitle =
        TR("ui.tools.bpm_measure.apply_popup_title").data();
    if ( m_shouldOpenApplyTimingPopup ) {
        // 请求位只消费一次，避免 Modal 已打开时重复压入弹窗栈。
        ::MMM::UI::FeedbackOpenPopup(popupTitle);
        m_shouldOpenApplyTimingPopup = false;
    }

    if ( ImGui::BeginPopupModal(
             popupTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize) ) {
        const auto options = collectApplyBeatmapOptions();
        if ( options.empty() ) {
            // 会话可能在确认弹窗期间关闭，空列表必须安全降级为只读提示。
            ImGui::TextColored(
                Utils::UIThemeUtils::getWarningColor(),
                "%s",
                TR("ui.tools.bpm_measure.no_apply_target").data());
        } else {
            const auto activeIt = std::find_if(
                options.begin(), options.end(), [&](const auto& option) {
                    return option.sessionIndex == m_applyTargetSessionIndex;
                });
            if ( activeIt == options.end() ) {
                // 原目标已关闭时选择首个仍有效会话，不保留悬空索引。
                m_applyTargetSessionIndex = options.front().sessionIndex;
            }

            std::string preview = options.front().displayName;
            // 预览文本从最新候选快照解析，避免缓存过期名称。
            for ( const auto& option : options ) {
                if ( option.sessionIndex == m_applyTargetSessionIndex ) {
                    preview = option.displayName;
                    break;
                }
            }

            ImGui::Text("%s", TR("ui.tools.bpm_measure.apply_target").data());
            ImGui::SetNextItemWidth(360.0f);
            if ( ::MMM::UI::FeedbackBeginCombo("##BpmApplyTarget",
                                               preview.c_str()) ) {
                for ( const auto& option : options ) {
                    // 会话索引是提交命令所需身份，相机 ID 仅保留在候选 DTO 中。
                    const bool selected =
                        option.sessionIndex == m_applyTargetSessionIndex;
                    if ( ::MMM::UI::FeedbackSelectable(
                             option.displayName.c_str(), selected) ) {
                        m_applyTargetSessionIndex = option.sessionIndex;
                    }
                    if ( selected ) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ::MMM::UI::FeedbackEndCombo();
            }
            ::MMM::UI::FeedbackCheckbox(
                TR("ui.tools.bpm_measure.keep_scroll").data(),
                &m_keepNonBpmTimingsOnApply);
        }

        ImGui::Spacing();
        const float buttonWidth = 120.0f;
        if ( options.empty() ) {
            // 没有目标时禁用应用按钮，但仍允许用户取消弹窗。
            ImGui::BeginDisabled();
        }
        if ( ::MMM::UI::FeedbackButton(TR("ui.common.apply").data(),
                                       ImVec2(buttonWidth, 0.0f)) ) {
            applyMeasuredTimingsToSelectedBeatmap();
            ImGui::CloseCurrentPopup();
        }
        if ( options.empty() ) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                       ImVec2(buttonWidth, 0.0f)) ) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/// @brief 绘制试听播放、暂停、进度和倍速控制。
/// @warning UI 热路径约束如下。
/// 热路径：每帧执行；只读取播放状态和处理用户输入，文件检查仅在按钮触发后发生。
/// @details 控件同时适配编辑器同步和独立试听两种 transport；进度滑块
/// 使用画布时间，倍速信息同时展示请求值与音频后端实际值。无选择或未加载
/// 时通过禁用控件保持布局稳定，不隐式触发文件访问。
void BpmMeasurementToolView::renderPlaybackControls()
{
    auto& audio = Audio::AudioManager::instance();
    // loaded 状态会在播放按钮触发后变化，因此使用可更新局部变量。
    bool       trackLoaded  = isSelectedTrackLoadedForPlayback();
    const bool hasSelection = !m_selectedAudioTrackId.empty();
    const bool isPlaying =
        // 共享与独立路由均通过同一时间线快照判断播放状态。
        trackLoaded && readPlaybackTimelineState(m_playbackRoute).isPlaying;

    ImGui::Spacing();
    ImGui::SeparatorText(TR("ui.tools.bpm_measure.playback").data());

    if ( !hasSelection ) {
        // 无选择时整组 transport 控件禁用，避免隐式加载默认轨道。
        ImGui::BeginDisabled();
    }

    const float iconButtonSize = ImGui::GetFrameHeight();
    Utils::pushFixedButtonStyleVars();
    if ( ::MMM::UI::FeedbackButton(isPlaying ? ICON_MMM_PAUSE : ICON_MMM_PLAY,
                                   ImVec2(iconButtonSize, iconButtonSize)) ) {
        // toggle 可能惰性加载音轨，随后重新查询 loaded 供停止按钮使用。
        (void)togglePlayback();
        trackLoaded = isSelectedTrackLoadedForPlayback();
    }
    Utils::popFixedButtonStyleVars();
    if ( ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("%s",
                          isPlaying ? TR("ui.tools.bpm_measure.pause").data()
                                    : TR("ui.tools.bpm_measure.play").data());
    }

    ImGui::SameLine();
    if ( !trackLoaded ) {
        // 尚未成功加载时停止与归零都没有明确 transport 目标。
        ImGui::BeginDisabled();
    }
    Utils::pushFixedButtonStyleVars();
    if ( ::MMM::UI::FeedbackButton(ICON_MMM_STOP,
                                   ImVec2(iconButtonSize, iconButtonSize)) ) {
        // 先暂停并归零统一路由，再对独立试听补发 stop 释放声音。
        setPlaybackState(false);
        seekPlaybackToCanvasTime(0.0);
        markUserPreferencesChanged(false, true);
        if ( shouldDirectlyControlBpmAudioTransport(m_playbackRoute) ) {
            // 编辑器同步路由由全局 transport 管理，不能在此直接停止。
            audio.stopAudition();
        }
    }
    Utils::popFixedButtonStyleVars();
    if ( ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("%s", TR("ui.tools.bpm_measure.stop").data());
    }
    if ( !trackLoaded ) {
        ImGui::EndDisabled();
    }

    if ( !hasSelection ) {
        ImGui::EndDisabled();
    }

    // 音频管理器时长与分析时长取较大值，兼容异步加载完成次序。
    const double totalTime =
        trackLoaded
            ? std::max(getPlaybackTotalTime(m_playbackRoute, audio), m_duration)
            : m_duration;
    const PlaybackTimelineState playbackState =
        trackLoaded ? readPlaybackTimelineState(m_playbackRoute)
                    : PlaybackTimelineState{};
    const double currentTime =
        // 未加载时用视图中心提供有意义的滑块预览位置。
        trackLoaded ? playbackState.visualTime
                    : std::clamp(m_viewCenter, 0.0, std::max(0.0, totalTime));
    float position = static_cast<float>(
        std::clamp(currentTime, 0.0, std::max(0.0, totalTime)));
    if ( !trackLoaded || totalTime <= 0.0 ) {
        // 无有效时长时仍绘制滑块以保持布局稳定，但禁止交互。
        ImGui::BeginDisabled();
    }
    ImGui::Text("%s", TR("ui.tools.bpm_measure.position").data());
    ImGui::SetNextItemWidth(-1.0f);
    if ( ::MMM::UI::FeedbackSliderFloat(
             "##BpmMeasurePlaybackPosition",
             &position,
             0.0f,
             static_cast<float>(std::max(0.001, totalTime)),
             "%.4fs") ) {
        // 滑块使用画布时间，路由偏移由 seekPlaybackToCanvasTime 统一转换。
        const double seekTime =
            std::clamp<double>(position, 0.0, std::max(0.0, totalTime));
        seekPlaybackToCanvasTime(seekTime);
        markUserPreferencesChanged(false, true);
    }
    if ( !trackLoaded || totalTime <= 0.0 ) {
        ImGui::EndDisabled();
    }

    // 文本使用共享时间格式化器，与编辑器其他播放控件保持一致。
    const std::string positionText =
        MMM::UI::Utils::formatCanvasTime(position) + " / " +
        MMM::UI::Utils::formatCanvasTime(totalTime);
    ImGui::TextDisabled("%s", positionText.c_str());

    char speedInfo[160] = { 0 };
    // 同时展示请求倍速和音频后端实际倍速，便于识别拉伸回退。
    std::snprintf(speedInfo,
                  sizeof(speedInfo),
                  TR("ui.audio_manager.speed_info").data(),
                  m_playbackSpeed,
                  trackLoaded ? (isPlaybackSynchronizedWithEditor()
                                     ? audio.getActualPlaybackSpeed()
                                     : audio.getActualAuditionPlaybackSpeed())
                              : m_playbackSpeed);
    ImGui::TextDisabled("%s", speedInfo);

    // 常用慢放倍率提供等宽快捷按钮，精确值仍可由下方滑块设置。
    const char* presetLabels[] = {
        TR("ui.audio_manager.speed_025x").data(),
        TR("ui.audio_manager.speed_050x").data(),
        TR("ui.audio_manager.speed_075x").data(),
        TR("ui.audio_manager.speed_100x").data(),
    };
    constexpr double presetSpeeds[] = { 0.25, 0.5, 0.75, 1.0 };
    const float      spacing        = ImGui::GetStyle().ItemSpacing.x;
    const float      buttonWidth    = std::max(
        0.0f, (ImGui::GetContentRegionAvail().x - spacing * 3.0f) / 4.0f);
    constexpr size_t presetCount =
        sizeof(presetSpeeds) / sizeof(presetSpeeds[0]);
    // 紧凑样式只覆盖倍率按钮，不改变其余控制栏的间距。
    pushBpmCompactTextButtonStyleVars();
    for ( size_t i = 0; i < presetCount; ++i ) {
        if ( i > 0 ) {
            ImGui::SameLine();
        }
        if ( ::MMM::UI::FeedbackButton(presetLabels[i],
                                       ImVec2(buttonWidth, 0.0f)) ) {
            applyPlaybackSpeed(presetSpeeds[i]);
        }
    }
    popBpmCompactTextButtonStyleVars();

    float speed = static_cast<float>(m_playbackSpeed);
    ImGui::Text("%s", TR("ui.audio_manager.speed_value").data());
    ImGui::SetNextItemWidth(-1.0f);
    if ( ::MMM::UI::FeedbackSliderFloat(
             "##BpmMeasurePlaybackSpeed", &speed, 0.25f, 2.0f, "%.4fx") ) {
        applyPlaybackSpeed(speed);
    }
}

/// @brief 绘制左侧波形和频谱面板。
/// @warning UI 热路径：每帧计算布局并调用三个内存绘制入口。
/// @details 波形和频谱共享时间中心与缩放，中间的全局滚动条用于长距离导航；
/// 跟随播放在布局前执行，保证三个区域观察同一帧的视野状态。
void BpmMeasurementToolView::renderAnalysisPanel()
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if ( avail.x <= 1.0f || avail.y <= 1.0f ) {
        // 折叠或尚未完成布局时跳过绘制，避免构造退化几何。
        return;
    }

    // 跟随逻辑先更新中心点，使本帧波形和频谱使用一致视野。
    followPlaybackIfNeeded();

    ImGui::Text("%s", TR("ui.tools.bpm_measure.waveform").data());
    const float textH    = ImGui::GetTextLineHeightWithSpacing();
    const float spacingY = ImGui::GetStyle().ItemSpacing.y;
    // 全局滚动条至少保持一个控件高度，确保窄窗口仍可拖动。
    const float overviewHeight =
        std::max(BPM_OVERVIEW_SCROLLBAR_MIN_HEIGHT, ImGui::GetFrameHeight());
    const float contentHeight = std::max(
        0.0f, avail.y - textH * 2.0f - overviewHeight - spacingY * 3.0f);
    // 波形约占可用区域四成，频谱获得余下空间并各自保留最低高度。
    const float waveHeight = std::max(120.0f, contentHeight * 0.42f);
    const float specHeight = std::max(160.0f, contentHeight - waveHeight);

    renderWaveformPlot(ImVec2(avail.x, waveHeight));

    ImGui::Spacing();
    renderOverviewTimelineScrollbar(ImVec2(avail.x, overviewHeight));
    ImGui::Spacing();

    ImGui::Text("%s", TR("ui.tools.bpm_measure.spectrum").data());
    renderSpectrumImage(ImVec2(avail.x, specHeight));
}

/// @brief 绘制波形和频谱之间的全局时间滚动条。
/// @param size 绘制区域尺寸。
/// @warning UI 热路径：每帧执行；拖动时同步更新视野和当前播放跳转，
/// 不访问文件系统。
/// @details 轨道始终映射完整画布时长，其中：
/// - 填充矩形表示当前可见区间；
/// - 红色细线表示 Timing 段起点；
/// - 高对比游标表示当前播放视觉时间；
/// - 点击或拖动同时改变视图中心；
/// - 已加载 transport 时同步执行 seek；
/// - 工具提示展示可见首尾和鼠标对应时间。
void BpmMeasurementToolView::renderOverviewTimelineScrollbar(const ImVec2& size)
{
    // 交互区域至少为一个像素，避免最小化窗口时出现除零。
    const float  width  = std::max(1.0f, size.x);
    const float  height = std::max(BPM_OVERVIEW_SCROLLBAR_MIN_HEIGHT, size.y);
    const double canvasDuration = playbackCanvasDuration();

    ImGui::InvisibleButton("##BpmMeasureOverviewTimeline",
                           ImVec2(width, height));
    // InvisibleButton 提供 hover/active 状态，视觉由当前窗口 DrawList 绘制。
    const ImVec2 rectMin  = ImGui::GetItemRectMin();
    const ImVec2 rectMax  = ImGui::GetItemRectMax();
    ImDrawList*  drawList = ImGui::GetWindowDrawList();

    // 轨道颜色取自当前主题，时间游标使用高对比固定色。
    const ImU32 bgColor     = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 borderColor = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 fillColor   = ImGui::GetColorU32(ImGuiCol_SliderGrab, 0.68f);
    const ImU32 fillBorderColor = ImGui::GetColorU32(ImGuiCol_SliderGrabActive);
    const ImU32 cursorColor     = IM_COL32(255, 60, 60, 230);
    drawList->AddRectFilled(rectMin, rectMax, bgColor, 4.0f);
    drawList->AddRect(rectMin, rectMax, borderColor, 4.0f, 0, 1.0f);

    if ( canvasDuration <= 0.0 ) {
        // 没有时间范围时保留空轨道并终止可能遗留的拖动状态。
        m_isOverviewTimelineDragging = false;
        return;
    }

    // 可视窗口以中心点为基准，并在音频首尾裁切。
    const double clampedCenter =
        std::clamp<double>(m_viewCenter, 0.0, canvasDuration);
    const double viewStart = std::max(0.0, clampedCenter - m_zoomSeconds);
    const double viewEnd =
        std::min(canvasDuration, clampedCenter + m_zoomSeconds);
    auto timeToX = [&](double time) {
        // 全局条始终映射完整音频范围，不受当前 zoom 影响。
        return rectMin.x +
               static_cast<float>(std::clamp(time / canvasDuration, 0.0, 1.0) *
                                  static_cast<double>(rectMax.x - rectMin.x));
    };
    auto xToTime = [&](float x) {
        // 鼠标超出轨道时钳制到首尾，拖动可以自然抵达边界。
        const double ratio = std::clamp<double>(
            (x - rectMin.x) / std::max(1.0f, rectMax.x - rectMin.x), 0.0, 1.0);
        return ratio * canvasDuration;
    };

    // 填充矩形表示波形和频谱当前共享的可见区间。
    const float viewX1 = timeToX(viewStart);
    const float viewX2 = timeToX(viewEnd);
    drawList->AddRectFilled(
        ImVec2(viewX1, rectMin.y + 3.0f),
        ImVec2(std::max(viewX1 + 3.0f, viewX2), rectMax.y - 3.0f),
        fillColor,
        4.0f);
    drawList->AddRect(ImVec2(viewX1, rectMin.y + 3.0f),
                      ImVec2(std::max(viewX1 + 3.0f, viewX2), rectMax.y - 3.0f),
                      fillBorderColor,
                      4.0f,
                      0,
                      1.5f);

    for ( const auto& segment : m_timingSegments ) {
        if ( segment.timestampSeconds < 0.0 ||
             segment.timestampSeconds > canvasDuration ) {
            // 首段可合法位于零点前，此处不把它绘制到全局轨道之外。
            continue;
        }
        const float markerX = timeToX(segment.timestampSeconds);
        drawList->AddLine(ImVec2(markerX, rectMin.y + 2.0f),
                          ImVec2(markerX, rectMax.y - 2.0f),
                          IM_COL32(255, 70, 70, 190),
                          1.5f);
    }

    if ( isSelectedTrackLoadedForPlayback() ) {
        // 播放指针使用视觉时间，包含编辑器路由的画布偏移补偿。
        const PlaybackTimelineState playbackState =
            readPlaybackTimelineState(m_playbackRoute);
        if ( playbackState.visualTime >= 0.0 &&
             playbackState.visualTime <= canvasDuration ) {
            const float cursorX = timeToX(playbackState.visualTime);
            drawList->AddLine(ImVec2(cursorX, rectMin.y),
                              ImVec2(cursorX, rectMax.y),
                              cursorColor,
                              2.0f);
        }
    }

    const bool active = ImGui::IsItemActive();
    if ( active && ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        // 首次按下与后续拖动共用路径，点击也能立即居中并 seek。
        const bool dragStarted       = !m_isOverviewTimelineDragging;
        m_isOverviewTimelineDragging = true;
        // 全局条接管水平导航时取消画布平移状态，避免双重更新。
        m_isTimelinePanning           = false;
        const double targetCanvasTime = std::clamp<double>(
            xToTime(ImGui::GetIO().MousePos.x), 0.0, canvasDuration);
        const bool targetChanged =
            std::abs(targetCanvasTime - m_viewCenter) > 1e-6;
        m_viewCenter = targetCanvasTime;
        if ( dragStarted || targetChanged ) {
            // 仅实际改变视野时标记偏好，减少无效配置写入。
            markUserPreferencesChanged(false, true);
        }
        if ( isSelectedTrackLoadedForPlayback() &&
             (dragStarted || targetChanged) ) {
            // 已加载时拖动既导航视野也控制播放，保持指针不脱离中心。
            seekPlaybackToCanvasTime(targetCanvasTime);
        }
    } else if ( !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        // 在控件外释放鼠标也必须结束拖动，防止状态黏住。
        m_isOverviewTimelineDragging = false;
    }

    if ( ImGui::IsItemHovered() || m_isOverviewTimelineDragging ) {
        // 提示同时给出可见区间与鼠标时间，便于长音频精确定位。
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        const double hoverTime = std::clamp<double>(
            xToTime(ImGui::GetIO().MousePos.x), 0.0, canvasDuration);
        const std::string tooltip =
            fmt::format("{} - {} / {}",
                        MMM::UI::Utils::formatCanvasTime(viewStart),
                        MMM::UI::Utils::formatCanvasTime(viewEnd),
                        MMM::UI::Utils::formatCanvasTime(hoverTime));
        ImGui::SetTooltip("%s", tooltip.c_str());
    }
}

/// @brief 播放时让分析视图自动跟随播放指针。
/// @warning UI 热路径约束如下。
/// 热路径：每帧执行；只读取播放同步快照并更新视图中心，不能访问文件系统。
/// @details 任意直接操纵视图、游标或拍线的手势都会暂时抑制跟随；
/// 暂停状态保持用户视野不变，只有实际播放且画布时长有效时更新中心。
void BpmMeasurementToolView::followPlaybackIfNeeded()
{
    if ( m_isTimelinePanning || m_isOverviewTimelineDragging ||
         m_isPlaybackCursorDragging || m_isBeatMarkerDragging ||
         !isSelectedTrackLoadedForPlayback() ) {
        // 任意显式拖动优先于自动跟随，避免视图与鼠标争夺控制权。
        return;
    }

    const PlaybackTimelineState playbackState =
        readPlaybackTimelineState(m_playbackRoute);
    if ( !playbackState.isPlaying ) {
        // 暂停时保留用户最后视野，不自动吸附到静止游标。
        return;
    }

    const double canvasDuration = playbackCanvasDuration();
    if ( canvasDuration <= 0.0 ) {
        // 时长尚未发布时无法安全归一化中心点。
        return;
    }

    // 播放期间让游标保持在当前视野中心，两个分析画布同步移动。
    m_viewCenter = std::clamp<double>(
        playbackState.visualTime, 0.0, std::max(0.0, canvasDuration));
}

/// @brief 更新 BPM 工具节拍器音效触发。
/// @warning UI 热路径约束如下。
/// 热路径：每帧执行；只读取播放同步快照并播放已预加载音效，不访问文件系统。
/// @details 调度器维护当前段落、首拍、拍长和下一拍索引的快照。
/// 以下情况会重建游标：
/// - 首次开始播放；
/// - 播放跨入新的 BPM 段；
/// - 用户修改首拍或 BPM；
/// - 播放位置倒退或发生大幅前跳。
/// 编辑器同步路由使用短前瞻计划播放，独立试听按帧补响刚越过的拍点；
/// 每帧触发数设有上限，过旧拍点只推进索引而不会集中播放。
void BpmMeasurementToolView::updateMetronomePlayback()
{
    ensureTimingSegments();
    if ( !isSelectedTrackLoadedForPlayback() || m_timingSegments.empty() ) {
        // 下次满足条件时必须重新从真实播放位置计算节拍索引。
        m_metronomeScheduleInitialized = false;
        return;
    }

    auto& audio = Audio::AudioManager::instance();
    if ( !m_metronomeSfxReady ) {
        // 热路径不访问文件，只查询此前预加载资源是否已经可用。
        m_metronomeSfxReady =
            audio.getSFXDuration(BPM_METRONOME_LOW_KEY) > 0.0 &&
            audio.getSFXDuration(BPM_METRONOME_HIGH_KEY) > 0.0;
        if ( !m_metronomeSfxReady ) {
            // 资源未就绪时静默跳过，前台打开流程负责低频预加载。
            return;
        }
    }

    if ( getPlaybackStatus(m_playbackRoute, audio) !=
         Audio::PlaybackStatus::Playing ) {
        // 暂停后旧的前瞻计划不再可信，恢复时重新建立调度游标。
        m_metronomeScheduleInitialized = false;
        return;
    }

    // 总时长兼顾画布、分析与音频后端，避免尾部节拍被提前裁掉。
    const double canvasDuration = playbackCanvasDuration();
    const double totalTime      = std::max(
        canvasDuration,
        std::max(m_duration, getPlaybackTotalTime(m_playbackRoute, audio)));
    if ( canvasDuration <= 0.0 || totalTime <= 0.0 ) {
        // 不完整时长不能用于调度边界计算。
        m_metronomeScheduleInitialized = false;
        return;
    }

    // 节拍调度使用原始音频时间，不使用含视觉偏移的画布时间。
    const double audioTime = std::clamp(
        getPlaybackCurrentTime(m_playbackRoute, audio), 0.0, totalTime);
    const std::size_t activeSegmentIndex = findSegmentIndexForTime(audioTime);
    const double      activeFirstBeatTime =
        m_timingSegments[activeSegmentIndex].timestampSeconds;
    const double activeBeatLength =
        segmentBeatLengthSeconds(activeSegmentIndex);
    // 段切换、首拍或 BPM 修改都会使已有节拍索引失效。
    const bool gridChanged =
        m_metronomeScheduledSegmentIndex != activeSegmentIndex ||
        std::abs(m_metronomeScheduledFirstBeatTime - activeFirstBeatTime) >
            1e-9 ||
        std::abs(m_metronomeScheduledBeatLength - activeBeatLength) > 1e-9;
    // 超过两拍或逆向移动视为 seek，不能沿用连续播放游标。
    const double jumpThreshold = std::max(0.25, activeBeatLength * 2.0);
    const bool   jumped = audioTime + 1e-4 < m_lastMetronomeAudioTime ||
                          audioTime - m_lastMetronomeAudioTime > jumpThreshold;
    if ( !m_metronomeScheduleInitialized || gridChanged || jumped ) {
        // 重置从当前时间附近寻找首个可触发拍点。
        resetMetronomeScheduler(audioTime);
    }

    const bool synchronizedPlayback = isPlaybackSynchronizedWithEditor();
    // 独立试听不借用主 BGM 的计划时钟，按帧即时补响以避免编辑器
    // seek/stop 清理全局预定音效时造成后续拍点漏触发。
    const double scheduleLookahead =
        synchronizedPlayback ? BPM_METRONOME_SCHEDULE_LOOKAHEAD_SECONDS : 1e-4;
    // 前瞻窗口不能越过音轨末尾。
    const double scheduleEndAudioTime =
        std::min(totalTime, audioTime + scheduleLookahead);

    int scheduledCount{ 0 };
    // 容许刚跨过的拍点即时补响，但过旧拍点只推进索引。
    const double pastTriggerWindow =
        metronomePastTriggerWindow(m_metronomeScheduledBeatLength);
    // 当前 BPM 段的调度必须在下一段起点前终止。
    const double scheduledSegmentEnd =
        m_metronomeScheduledSegmentIndex + 1 < m_timingSegments.size()
            ? m_timingSegments[m_metronomeScheduledSegmentIndex + 1]
                  .timestampSeconds
            : totalTime;
    while ( scheduledCount < BPM_METRONOME_MAX_TRIGGERED_PER_FRAME ) {
        // 索引相对当前段首拍，可为负值以覆盖零点前首拍模型。
        const double beatAudioTime =
            m_metronomeScheduledFirstBeatTime +
            static_cast<double>(m_nextMetronomeBeatIndex) *
                m_metronomeScheduledBeatLength;
        const bool latestBeatCrossedSinceLastUpdate =
            // 独立试听没有后端计划队列，只补发本帧刚越过的最新拍点。
            !synchronizedPlayback &&
            beatAudioTime > m_lastMetronomeAudioTime + 1e-4 &&
            beatAudioTime <= audioTime + 1e-4 &&
            beatAudioTime + m_metronomeScheduledBeatLength > audioTime + 1e-4;
        if ( beatAudioTime < audioTime - pastTriggerWindow &&
             !latestBeatCrossedSinceLastUpdate ) {
            // 过期拍点不可集中补响，只推进到当前时间附近。
            ++m_nextMetronomeBeatIndex;
            continue;
        }
        if ( beatAudioTime >= scheduledSegmentEnd - 1e-9 ) {
            // 下一帧进入新段后由 gridChanged 重建对应节拍网格。
            break;
        }
        if ( beatAudioTime > scheduleEndAudioTime ) {
            // 尚未进入前瞻窗口，保留索引供后续帧处理。
            break;
        }

        // 节拍器按真实音频/谱面时间轴调度；波形/频谱视觉偏移只影响绘制，
        // 不能在这里二次叠加。
        if ( beatAudioTime >= 0.0 && beatAudioTime <= totalTime ) {
            int64_t beatMod = m_nextMetronomeBeatIndex % 4;
            if ( beatMod < 0 ) {
                // C++ 负余数需校正，保证零点前拍序仍按四拍循环。
                beatMod += 4;
            }
            const char* key =
                beatMod == 0 ? BPM_METRONOME_HIGH_KEY : BPM_METRONOME_LOW_KEY;
            if ( beatAudioTime <= audioTime + 1e-4 ) {
                // 已跨过或恰好命中的拍点立即播放。
                audio.playSoundEffect(key, BPM_METRONOME_VOLUME_FACTOR);
            } else if ( synchronizedPlayback ) {
                // 编辑器同步路由支持绝对音频时间的低抖动预定播放。
                audio.playSoundEffectScheduled(
                    key, beatAudioTime, BPM_METRONOME_VOLUME_FACTOR);
            }
        }

        ++m_nextMetronomeBeatIndex;
        ++scheduledCount;
    }

    // 保存本帧时间用于下帧识别 seek 和独立试听的跨越拍点。
    m_lastMetronomeAudioTime = audioTime;
}

/// @brief 确保 BPM 工具节拍器音效已预加载。
/// @return 两个节拍器音效均可播放时返回 true。
/// @warning 低频路径：可能解析皮肤路径并从文件加载两个短音效。
/// @details 皮肤可覆盖普通拍和重拍文件及各自 lead-in；缺失覆盖时使用
/// 内置资源。任一音效加载失败都会保持节拍器未就绪，避免只播放半套节奏。
bool BpmMeasurementToolView::ensureMetronomeSoundEffects()
{
    auto& audio = Audio::AudioManager::instance();
    if ( audio.getSFXDuration(BPM_METRONOME_LOW_KEY) > 0.0 &&
         audio.getSFXDuration(BPM_METRONOME_HIGH_KEY) > 0.0 ) {
        // 两个资源都已存在时复用音频缓存，不重复解析文件。
        m_metronomeSfxReady = true;
        return true;
    }

    // 皮肤可分别覆盖普通拍和重拍音效；缺项时回退内置资源路径。
    const auto& skinData         = Config::SkinManager::instance().getData();
    auto        resolveAudioPath = [&](const char*                  key,
                                       const std::filesystem::path& fallback) {
        if ( const auto it = skinData.audioPaths.find(key);
             it != skinData.audioPaths.end() ) {
            // 皮肤表记录的路径已经由 SkinManager 解析为文件系统路径。
            return it->second;
        }
        return skinData.skinPath / Config::utf8ToPath("resources") / fallback;
    };

    const std::filesystem::path lowPath =
        resolveAudioPath(BPM_METRONOME_LOW_KEY,
                         Config::utf8ToPath("audio/metronome/beat_low.wav"));
    const std::filesystem::path highPath = resolveAudioPath(
        BPM_METRONOME_HIGH_KEY,
        Config::utf8ToPath("audio/metronome/downbeat_high.wav"));
    // lead-in 修正音频文件前导静音，使听感落点与网格时间一致。
    auto resolveLeadIn = [&](const char* key) {
        if ( const auto it = skinData.audioLeadInSeconds.find(key);
             it != skinData.audioLeadInSeconds.end() ) {
            return it->second;
        }
        return 0.0;
    };

    // 单个资源若已缓存则视为成功，仅加载缺失的另一种节拍音。
    const bool lowLoaded =
        audio.getSFXDuration(BPM_METRONOME_LOW_KEY) > 0.0 ||
        audio.preloadSoundEffect(BPM_METRONOME_LOW_KEY,
                                 Config::pathToUtf8(lowPath),
                                 1.0f,
                                 resolveLeadIn(BPM_METRONOME_LOW_KEY));
    const bool highLoaded =
        audio.getSFXDuration(BPM_METRONOME_HIGH_KEY) > 0.0 ||
        audio.preloadSoundEffect(BPM_METRONOME_HIGH_KEY,
                                 Config::pathToUtf8(highPath),
                                 1.0f,
                                 resolveLeadIn(BPM_METRONOME_HIGH_KEY));
    // 只有普通拍与重拍都可用时才启动调度，避免不完整节奏提示。
    m_metronomeSfxReady = lowLoaded && highLoaded;
    return m_metronomeSfxReady;
}

/// @brief 从当前音频调度时间重置节拍器调度游标。
/// @param audioTime 当前音频调度时间，单位为秒。
/// @details 入口先确定当前 Timing 段，再通过解析公式寻找不早于补响窗口的
/// 首个整数拍索引，同时保存网格参数快照供热路径检测变化。
void BpmMeasurementToolView::resetMetronomeScheduler(double audioTime)
{
    ensureTimingSegments();
    if ( m_timingSegments.empty() ) {
        // 防御空列表，后续更新会重新调用 ensure。
        m_metronomeScheduleInitialized = false;
        return;
    }

    // 以当前生效段独立计算拍长，支持中途变速。
    const std::size_t segmentIndex = findSegmentIndexForTime(audioTime);
    const double      beatLength   = segmentBeatLengthSeconds(segmentIndex);
    if ( beatLength <= 1e-6 ) {
        // 极小或无效拍长不能安全参与除法和循环调度。
        m_metronomeScheduleInitialized = false;
        return;
    }

    const double firstBeatTime =
        m_timingSegments[segmentIndex].timestampSeconds;
    const double pastTriggerWindow = metronomePastTriggerWindow(beatLength);
    // ceil 选择不早于容差窗口的首个拍点，避免恢复时重放旧拍。
    m_nextMetronomeBeatIndex = static_cast<int64_t>(std::ceil(
        (audioTime - firstBeatTime - pastTriggerWindow) / beatLength - 1e-6));
    m_lastMetronomeAudioTime = audioTime;
    // 保存网格快照，后续热路径通过比较检测参数变化。
    m_metronomeScheduledFirstBeatTime = firstBeatTime;
    m_metronomeScheduledBeatLength    = beatLength;
    m_metronomeScheduledSegmentIndex  = segmentIndex;
    m_metronomeScheduleInitialized    = true;
}

/// @brief 更新波形绘制用的画布时间缓存。
/// @param canvasOffset 画布时间相对音频采样时间的偏移，单位为秒。
/// @warning UI 热路径约束如下。
/// 热路径：波形图每帧查询；仅在画布偏移或波形缓存变化时重建时间数组。
/// @details 源时间数组保持音频坐标且有序；派生数组逐项叠加波形专用偏移，
/// 因此仍可用于 lower_bound 和 upper_bound 的可见区间二分查找。
void BpmMeasurementToolView::updateWaveCanvasTimes(double canvasOffset)
{
    if ( m_waveCanvasTimes.size() == m_waveTimes.size() &&
         std::abs(m_waveCanvasTimesOffset - canvasOffset) < 1e-9 ) {
        // 数据规模与偏移均未改变时直接复用缓存。
        return;
    }

    // 源时间保持音频坐标，单独缓存转换后的画布坐标供二分查询。
    m_waveCanvasTimes.resize(m_waveTimes.size());
    for ( size_t i = 0; i < m_waveTimes.size(); ++i ) {
        m_waveCanvasTimes[i] = m_waveTimes[i] + canvasOffset;
    }
    // 最后发布偏移标识，保证中途退出不会误判缓存有效。
    m_waveCanvasTimesOffset = canvasOffset;
}

/// @brief 绘制波形图，并在同一裁剪区叠加节拍与播放交互。
/// @param size 绘制区域尺寸。
/// @warning UI 热路径：每帧执行；通过二分查找只提交可见波形区间。
/// @details ImPlot 只负责坐标轴和包络填充；节拍线、播放游标及命中反馈
/// 使用 ImGui DrawList 叠加。覆盖层调用顺序确保对象拖动优先于背景平移，
/// 双击空白处则快速设置第一 Timing 段的首拍。
void BpmMeasurementToolView::renderWaveformPlot(const ImVec2& size)
{
    // 视野中心与半宽统一裁切到画布范围，并保证横轴至少有最小跨度。
    const double canvasDuration = std::max(0.0, playbackCanvasDuration());
    const double clampedCenter =
        std::clamp<double>(m_viewCenter, 0.0, canvasDuration);
    const double viewStart = std::max(0.0, clampedCenter - m_zoomSeconds);
    const double viewEnd = std::min(std::max(canvasDuration, viewStart + 0.001),
                                    clampedCenter + m_zoomSeconds);

    if ( ImPlot::BeginPlot("##BpmMeasureWaveform",
                           size,
                           ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect |
                               ImPlotFlags_NoMouseText) ) {
        // X 轴承担时间导航；Y 轴固定振幅范围以防自动缩放造成跳动。
        ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_None);
        ImPlot::SetupAxis(ImAxis_Y1,
                          nullptr,
                          ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_Lock);
        ImPlot::SetupAxisLimits(
            ImAxis_X1, viewStart, viewEnd, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -1.05, 1.05, ImGuiCond_Always);

        if ( !m_waveTimes.empty() ) {
            // 将音频采样时间转换为与编辑器一致的画布时间后再裁剪。
            updateWaveCanvasTimes(waveformCanvasOffset());
            // 三个数组必须取共同长度，且 ImPlot 点数受 int 上限约束。
            const size_t availableCount = std::min(
                { m_waveCanvasTimes.size(),
                  m_waveMin.size(),
                  m_waveMax.size(),
                  static_cast<size_t>(std::numeric_limits<int>::max()) });
            // 时间数组有序，可用二分定位可见样本而不扫描整段波形。
            auto beginIt = std::lower_bound(
                m_waveCanvasTimes.begin(),
                m_waveCanvasTimes.begin() +
                    static_cast<std::ptrdiff_t>(availableCount),
                viewStart);
            auto endIt = std::upper_bound(
                m_waveCanvasTimes.begin(),
                m_waveCanvasTimes.begin() +
                    static_cast<std::ptrdiff_t>(availableCount),
                viewEnd);
            if ( beginIt != m_waveCanvasTimes.begin() ) {
                // 左侧多保留一个包络点，避免裁剪边缘出现断口。
                --beginIt;
            }
            if ( endIt != m_waveCanvasTimes.begin() +
                              static_cast<std::ptrdiff_t>(availableCount) ) {
                // 右侧同样扩展一个点，使折线覆盖完整可见区间。
                ++endIt;
            }

            const size_t firstVisibleIndex =
                static_cast<size_t>(beginIt - m_waveCanvasTimes.begin());
            const int visibleCount = static_cast<int>(endIt - beginIt);
            if ( visibleCount >= 2 ) {
                // 上下包络填充比逐采样折线更适合压缩后的长音频概览。
                ImPlot::PlotShaded("##WaveEnvelope",
                                   m_waveCanvasTimes.data() + firstVisibleIndex,
                                   m_waveMin.data() + firstVisibleIndex,
                                   m_waveMax.data() + firstVisibleIndex,
                                   visibleCount,
                                   ImPlotSpec(ImPlotProp_FillAlpha, 0.55f));
            }
        }

        // 所有覆盖层限制在绘图区内，轴标签和相邻控件不会被污染。
        ImPlot::PushPlotClipRect();
        ImVec2 plotMin = ImPlot::GetPlotPos();
        ImVec2 plotMax = ImVec2(plotMin.x + ImPlot::GetPlotSize().x,
                                plotMin.y + ImPlot::GetPlotSize().y);
        drawBeatSubdivisionLines(
            *ImGui::GetWindowDrawList(), plotMin, plotMax, viewStart, viewEnd);
        drawBeatMarkers(
            *ImGui::GetWindowDrawList(), plotMin, plotMax, viewStart, viewEnd);
        drawPlaybackCursor(
            *ImGui::GetWindowDrawList(), plotMin, plotMax, viewStart, viewEnd);
        const bool interactionHovered = ImPlot::IsPlotHovered();
        // 播放游标优先于拍点拖动；各处理器通过共享状态避免争抢。
        handlePlaybackCursorDrag(
            plotMin, plotMax, viewStart, viewEnd, interactionHovered, 1);
        handleBeatMarkerDrag(
            plotMin, plotMax, viewStart, viewEnd, interactionHovered, 1);
        handleTimelineNavigation(
            plotMin, plotMax, viewStart, viewEnd, interactionHovered);
        ImPlot::PopPlotClipRect();

        if ( interactionHovered ) {
            // 悬停时间由 ImPlot 坐标直接提供，避免重复像素转换。
            ImPlotPoint  mousePos  = ImPlot::GetPlotMousePos();
            const double hoverTime = std::clamp<double>(
                mousePos.x, 0.0, std::max(0.0, canvasDuration));
            ImGui::SetTooltip(
                "%s", MMM::UI::Utils::formatCanvasTime(hoverTime).c_str());
            if ( ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ) {
                // 双击是快速设置基准首拍入口，并同步到第一 Timing 段。
                m_firstBeatTime = hoverTime;
                syncPrimaryTimingFieldsToSegments();
            }
        }

        ImPlot::EndPlot();
    }
}

/// @brief 绘制频谱图，并把分块纹理映射到当前画布时间范围。
/// @param size 绘制区域尺寸。
/// @warning UI 热路径：每帧执行；仅遍历已上传的固定宽度纹理分块。
/// @details 当前画布范围先移除频谱专用偏移，再换算为全局纹理像素列；
/// 每个相交分块分别计算 UV 和屏幕范围。纹理绘制完成后使用与波形相同的
/// 覆盖层和交互处理器，保证两个视图的行为一致。
void BpmMeasurementToolView::renderSpectrumImage(const ImVec2& size)
{
    // 频谱与波形共享同一视野，保证竖直节拍标记可以直接对齐。
    const double canvasDuration = std::max(0.0, playbackCanvasDuration());
    const double clampedCenter =
        std::clamp<double>(m_viewCenter, 0.0, canvasDuration);
    const double viewStart = std::max(0.0, clampedCenter - m_zoomSeconds);
    const double viewEnd = std::min(std::max(canvasDuration, viewStart + 0.001),
                                    clampedCenter + m_zoomSeconds);
    const double viewRange = std::max(0.001, viewEnd - viewStart);
    // 纹理像素位于原始音频时间，先移除视觉画布偏移再换算横向段号。
    const double spectrumOffset = spectrumCanvasOffset();
    const double audioViewStart = viewStart - spectrumOffset;
    const double audioViewEnd   = viewEnd - spectrumOffset;
    const double pixelStart     = audioViewStart * m_spectrumSegmentsPerSecond;
    const double pixelEnd       = audioViewEnd * m_spectrumSegmentsPerSecond;
    // 最小像素跨度避免极端缩放或空分析数据导致除零。
    const double pixelWidth = std::max(1.0, pixelEnd - pixelStart);

    // 频谱底色始终绘制，纹理尚未上传完成时也保持稳定视觉区域。
    ImVec2      imageMin = ImGui::GetCursorScreenPos();
    ImVec2      imageMax = ImVec2(imageMin.x + size.x, imageMin.y + size.y);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(imageMin, imageMax, IM_COL32(12, 14, 18, 255));

    if ( !m_texturesNeedReload && !m_spectrumTextures.empty() ) {
        // 仅完整上传后展示整组纹理，避免一半新数据与一半旧数据混合。
        for ( size_t i = 0; i < m_spectrumTextures.size(); ++i ) {
            const auto&  texture = m_spectrumTextures[i];
            const double texStart =
                static_cast<double>(i * static_cast<size_t>(MAX_TEXTURE_W));
            const double texEnd = texStart + texture->width();

            if ( texEnd < pixelStart || texStart > pixelEnd ) {
                // 分块不与可视音频像素区相交时直接剔除。
                continue;
            }

            // 相交范围同时转换为纹理 UV 和屏幕 X 坐标。
            const double intersectStart = std::max(texStart, pixelStart);
            const double intersectEnd   = std::min(texEnd, pixelEnd);
            const float  uv0x = static_cast<float>((intersectStart - texStart) /
                                                   texture->width());
            const float  uv1x = static_cast<float>((intersectEnd - texStart) /
                                                   texture->width());
            const float  screenX0 =
                imageMin.x + static_cast<float>((intersectStart - pixelStart) /
                                                pixelWidth * size.x);
            const float screenX1 =
                imageMin.x + static_cast<float>((intersectEnd - pixelStart) /
                                                pixelWidth * size.x);
            if ( screenX1 <= screenX0 ) {
                // 亚像素或反向交集不提交退化图像命令。
                continue;
            }

            drawList->AddImage(texture->getImTextureID(),
                               ImVec2(screenX0, imageMin.y),
                               ImVec2(screenX1, imageMax.y),
                               ImVec2(uv0x, 0.0f),
                               ImVec2(uv1x, 1.0f));
        }
    }

    // 覆盖层在纹理之后绘制，确保网格和游标始终可见。
    drawBeatSubdivisionLines(*drawList, imageMin, imageMax, viewStart, viewEnd);
    drawBeatMarkers(*drawList, imageMin, imageMax, viewStart, viewEnd);
    drawPlaybackCursor(*drawList, imageMin, imageMax, viewStart, viewEnd);

    // 恢复到图像起点创建等尺寸命中区，不额外占用布局空间。
    ImGui::SetCursorScreenPos(imageMin);
    ImGui::InvisibleButton(
        "##BpmMeasureSpectrumHover",
        ImVec2(imageMax.x - imageMin.x, imageMax.y - imageMin.y));
    const bool isSpectrumHovered = ImGui::IsItemHovered();
    // 使用不同区域 ID 让跨波形、频谱拖动时能识别当前命中来源。
    handlePlaybackCursorDrag(
        imageMin, imageMax, viewStart, viewEnd, isSpectrumHovered, 2);
    handleBeatMarkerDrag(
        imageMin, imageMax, viewStart, viewEnd, isSpectrumHovered, 2);
    handleTimelineNavigation(
        imageMin, imageMax, viewStart, viewEnd, isSpectrumHovered);
    if ( isSpectrumHovered ) {
        // 频谱未使用 ImPlot，需要手工把鼠标横坐标映射为画布时间。
        const ImVec2 mousePos = ImGui::GetMousePos();
        const double relX     = std::clamp<double>(
            (mousePos.x - imageMin.x) / std::max(1.0f, imageMax.x - imageMin.x),
            0.0,
            1.0);
        const double hoverTime = std::clamp<double>(
            viewStart + relX * viewRange, 0.0, std::max(0.0, canvasDuration));
        ImGui::SetTooltip("%s",
                          MMM::UI::Utils::formatCanvasTime(hoverTime).c_str());
        if ( ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ) {
            // 与波形保持一致的双击首拍语义。
            m_firstBeatTime = hoverTime;
            syncPrimaryTimingFieldsToSegments();
        }
    }
}

/// @brief 在指定矩形区域叠加拍线、黄色拍框和首拍红色覆盖框。
/// @param drawList 目标 ImGui 绘制列表。
/// @param rectMin 绘制区域左上角。
/// @param rectMax 绘制区域右下角。
/// @param viewStart 当前视图起始时间，单位为秒。
/// @param viewEnd 当前视图结束时间，单位为秒。
/// @warning UI 热路径：波形和频谱各调用一次，只枚举当前视野相交拍点。
/// @details 每个 Timing 段独立生成拍网格，段末由下一段接管。
/// 第一段允许向零点前延伸；跨段维护全局拍序用于标签；标记时间由整数
/// 索引解析计算，避免逐拍累加浮点误差。半框时间宽度参与可见性裁剪，
/// 每个段的 index 0 叠加红色样式表达变速边界。
void BpmMeasurementToolView::drawBeatMarkers(ImDrawList&   drawList,
                                             const ImVec2& rectMin,
                                             const ImVec2& rectMax,
                                             double        viewStart,
                                             double        viewEnd) const
{
    if ( m_timingSegments.empty() || viewEnd <= viewStart ||
         rectMax.x <= rectMin.x ) {
        // 缺少网格或绘图区退化时没有可生成的标记几何。
        return;
    }

    // 标记宽度来自毫秒偏好，转换后限制到可辨识且不过宽的范围。
    const auto&  segments = m_timingSegments;
    const double markerSeconds =
        std::clamp<double>(m_markerWidthMs / 1000.0, 0.004, 1.0);
    const double halfMarkerSeconds = markerSeconds * 0.5;
    const double width             = rectMax.x - rectMin.x;

    auto timeToX = [&](double time) {
        // 当前视野线性映射到裁剪矩形，允许边缘外少量宽度参与剔除。
        return rectMin.x + static_cast<float>((time - viewStart) /
                                              (viewEnd - viewStart) * width);
    };

    // 普通拍使用黄色框，段落首拍叠加红色以突出 BPM 边界。
    const ImU32 white           = IM_COL32(255, 255, 255, 255);
    const ImU32 fill            = IM_COL32(255, 220, 60, 70);
    const ImU32 border          = IM_COL32(255, 220, 60, 235);
    const ImU32 firstBeatFill   = IM_COL32(255, 40, 40, 82);
    const ImU32 firstBeatBorder = IM_COL32(255, 35, 35, 255);
    const ImU32 beatIndexText   = IM_COL32(255, 255, 255, 215);
    const ImU32 beatIndexShadow = IM_COL32(0, 0, 0, 180);

    // 所有标记、框和序号都限制在分析画布内部。
    drawList.PushClipRect(rectMin, rectMax, true);
    // 跨段累计拍序，保证显示编号不会在每个变速段重新从零开始。
    int64_t segmentBaseBeatIndex{ 0 };
    for ( std::size_t segmentIndex = 0; segmentIndex < segments.size();
          ++segmentIndex ) {
        const auto&  segment    = segments[segmentIndex];
        const double beatLength = 60.0 / ::MMM::normalizeBpmValue(segment.bpm);
        if ( beatLength <= 1e-6 ) {
            // 归一化通常阻止此情况，仍防御未来输入来源绕过规范化。
            continue;
        }

        const double segmentStart = segment.timestampSeconds;
        // 非末段严格止于下一段起点，末段只需覆盖当前视野后一个拍长。
        const double segmentEnd =
            segmentIndex + 1 < segments.size()
                ? segments[segmentIndex + 1].timestampSeconds
                : viewEnd + beatLength;
        if ( segmentIndex + 1 < segments.size() &&
             segmentEnd <= segmentStart + 1e-9 ) {
            // 零长或倒序段不生成标记，交由归一化流程在后续修正。
            continue;
        }
        auto advanceSegmentBaseBeatIndex = [&]() {
            if ( segmentIndex + 1 >= segments.size() ) {
                // 末段之后没有需要继承序号的后续段。
                return;
            }

            // ceil 统计在段末切换前出现的拍点数量，容差抑制浮点临界误差。
            const double  beatSpan  = (segmentEnd - segmentStart) / beatLength;
            const int64_t beatCount = std::max<int64_t>(
                0, static_cast<int64_t>(std::ceil(beatSpan - 1e-6)));
            segmentBaseBeatIndex += beatCount;
        };

        // 第一段允许向首拍之前延伸；后续段从各自起点开始绘制。
        const double visibleStart =
            segmentIndex == 0 ? viewStart : std::max(viewStart, segmentStart);
        const double visibleEnd = std::min(viewEnd, segmentEnd);
        if ( visibleEnd < visibleStart - halfMarkerSeconds ) {
            // 段不可见时仍更新全局拍序，为后续可见段提供正确编号。
            advanceSegmentBaseBeatIndex();
            continue;
        }

        // 将半框宽纳入索引范围，使中心刚出屏但框仍可见的标记不被裁掉。
        const int64_t firstIndex = static_cast<int64_t>(std::ceil(
            (visibleStart - segmentStart - halfMarkerSeconds) / beatLength));
        const int64_t lastIndex  = static_cast<int64_t>(std::floor(
            (visibleEnd - segmentStart + halfMarkerSeconds) / beatLength));
        for ( int64_t index = firstIndex; index <= lastIndex; ++index ) {
            // 当前段内按整数拍索引生成时间，避免增量累加误差。
            const double beatTime =
                segmentStart + static_cast<double>(index) * beatLength;
            if ( segmentIndex > 0 && beatTime < segmentStart - 1e-6 ) {
                // 后续段不得向前延伸到上一段的时间范围。
                continue;
            }
            if ( segmentIndex + 1 < segments.size() &&
                 beatTime >= segmentEnd + 1e-6 ) {
                // 段末及之后的拍点由下一段 BPM 网格负责。
                continue;
            }

            const float lineX             = timeToX(beatTime);
            const bool  isFirstBeatMarker = index == 0;
            // 时间宽度换算为像素后保留三像素下限，便于命中和辨识。
            const float halfBoxWidth =
                std::max(3.0f,
                         static_cast<float>(halfMarkerSeconds /
                                            (viewEnd - viewStart) * width));
            if ( lineX < rectMin.x - halfBoxWidth ||
                 lineX > rectMax.x + halfBoxWidth ) {
                // 完整框位于裁剪区外时不生成绘制命令。
                continue;
            }

            drawList.AddLine(ImVec2(lineX, rectMin.y),
                             ImVec2(lineX, rectMax.y),
                             white,
                             1.0f);

            const float boxX1 = lineX - halfBoxWidth;
            const float boxX2 = lineX + halfBoxWidth;
            if ( boxX2 <= boxX1 ) {
                // 防御浮点退化矩形。
                continue;
            }

            drawList.AddRectFilled(
                ImVec2(boxX1, rectMin.y), ImVec2(boxX2, rectMax.y), fill);
            drawList.AddRect(ImVec2(boxX1, rectMin.y),
                             ImVec2(boxX2, rectMax.y),
                             border,
                             0.0f,
                             0,
                             1.5f);
            if ( isFirstBeatMarker ) {
                // 每段起点使用更强红色描边，表达 Timing 变更边界。
                drawList.AddRectFilled(ImVec2(boxX1, rectMin.y),
                                       ImVec2(boxX2, rectMax.y),
                                       firstBeatFill);
                drawList.AddRect(ImVec2(boxX1, rectMin.y),
                                 ImVec2(boxX2, rectMax.y),
                                 firstBeatBorder,
                                 0.0f,
                                 0,
                                 2.4f);
                drawList.AddLine(ImVec2(lineX, rectMin.y),
                                 ImVec2(lineX, rectMax.y),
                                 firstBeatBorder,
                                 2.0f);
            }

            const int64_t beatLabelIndex = segmentBaseBeatIndex + index;
            if ( beatLabelIndex >= 0 ) {
                // 零点前的负拍不显示编号，正式时间轴从 #00 开始。
                char       label[32]{};
                const auto labelResult = fmt::format_to_n(
                    label, sizeof(label) - 1, "#{:02d}", beatLabelIndex);
                label[std::min(labelResult.size, sizeof(label) - 1)] = '\0';

                const ImVec2 labelSize = ImGui::CalcTextSize(label);
                // 序号贴近画布底部并水平居中于拍线。
                const ImVec2 labelPos(
                    lineX - labelSize.x * 0.5f,
                    rectMax.y - labelSize.y -
                        BEAT_MARKER_INDEX_LABEL_BOTTOM_PADDING);
                drawList.AddText(ImVec2(labelPos.x + 1.0f, labelPos.y + 1.0f),
                                 beatIndexShadow,
                                 label);
                // 一像素阴影提高文字在亮色频谱区域上的可读性。
                drawList.AddText(labelPos, beatIndexText, label);
            }
        }
        advanceSegmentBaseBeatIndex();
    }
    drawList.PopClipRect();
}

/// @brief 在指定矩形区域叠加分拍线。
/// @param drawList 目标 ImGui 绘制列表。
/// @param rectMin 绘制区域左上角。
/// @param rectMax 绘制区域右下角。
/// @param viewStart 当前视图起始时间，单位为秒。
/// @param viewEnd 当前视图结束时间，单位为秒。
/// @warning UI 热路径约束如下。
/// 热路径：波形图和频谱图每帧执行；按当前视野增量绘制分拍线，不得加入音频解码或文件访问。
/// @details 每段从首个可见细分索引直接开始，分母经最大公约数约分后映射
/// 到皮肤线条层级。多个细分落在同一屏幕像素列时仅绘制一次，限制极端
/// 缩放下的 draw command 数量；所有时间由整数步索引重新计算以抑制漂移。
void BpmMeasurementToolView::drawBeatSubdivisionLines(ImDrawList&   drawList,
                                                      const ImVec2& rectMin,
                                                      const ImVec2& rectMax,
                                                      double        viewStart,
                                                      double viewEnd) const
{
    // 每拍细分数在统一范围内参与所有段，保持波形和频谱一致。
    const int beatDivisor = std::clamp(m_beatDivisor, 1, 64);
    if ( m_timingSegments.empty() || viewEnd <= viewStart ||
         rectMax.x <= rectMin.x || rectMax.y <= rectMin.y ) {
        // 空网格、反向时间或退化矩形都不生成覆盖层。
        return;
    }

    const auto&  segments = m_timingSegments;
    const double width    = rectMax.x - rectMin.x;
    // 以屏幕像素列为去重单位，防止高倍率细分在同一列重复绘制。
    const int columnCount = std::max(1, static_cast<int>(std::ceil(width)));
    drawList.PushClipRect(rectMin, rectMax, true);
    for ( std::size_t segmentIndex = 0; segmentIndex < segments.size();
          ++segmentIndex ) {
        const auto&  segment    = segments[segmentIndex];
        const double beatLength = 60.0 / ::MMM::normalizeBpmValue(segment.bpm);
        const double stepDuration = beatLength / beatDivisor;
        if ( stepDuration <= 1e-6 ) {
            // 无法分辨的步长可能造成循环不前进，必须跳过。
            continue;
        }

        const double segmentStart = segment.timestampSeconds;
        // 细分线不能跨越下一 BPM 段，末段只覆盖当前可视区。
        const double segmentEnd =
            segmentIndex + 1 < segments.size()
                ? segments[segmentIndex + 1].timestampSeconds
                : viewEnd + stepDuration;
        if ( segmentIndex + 1 < segments.size() &&
             segmentEnd <= segmentStart + 1e-9 ) {
            // 非正长度段没有有效细分区间。
            continue;
        }

        // 第一段可向首拍之前生成反向网格，后续段不得侵入上一段。
        const double visibleStart =
            segmentIndex == 0 ? viewStart : std::max(viewStart, segmentStart);
        const double visibleEnd = std::min(viewEnd, segmentEnd);
        if ( visibleEnd < visibleStart ) {
            // 当前段完全落在视野外。
            continue;
        }

        // 起始索引由解析式计算，避免从段首逐步遍历到可见区。
        int64_t stepOffset = static_cast<int64_t>(
            std::ceil((visibleStart - segmentStart) / stepDuration - 1e-4));
        if ( segmentIndex == 0 && visibleStart < segmentStart ) {
            // 首段前方使用 floor 取得左侧首个候选负索引。
            stepOffset = static_cast<int64_t>(std::floor(
                (visibleStart - segmentStart) / stepDuration + 1e-4));
        }

        double t =
            segmentStart + static_cast<double>(stepOffset) * stepDuration;
        while ( t < visibleStart - 1e-4 ) {
            // 浮点容差后仍落在左边界外时推进到首个可见细分。
            ++stepOffset;
            t = segmentStart + static_cast<double>(stepOffset) * stepDuration;
        }

        // 相邻细分压缩到同一像素列时只保留一次绘制。
        int lastColumn = -1;
        while ( t <= visibleEnd + 1e-4 ) {
            const float lineX =
                rectMin.x + static_cast<float>((t - viewStart) /
                                               (viewEnd - viewStart) * width);
            const int column = static_cast<int>(std::floor(lineX - rectMin.x));
            if ( column >= 0 && column < columnCount && column != lastColumn ) {
                // 负步索引的余数校正到 [0, beatDivisor) 区间。
                int beatIndex = static_cast<int>(stepOffset % beatDivisor);
                if ( beatIndex < 0 ) {
                    beatIndex += beatDivisor;
                }

                int denominator = 1;
                if ( beatIndex != 0 ) {
                    // 约分后的分母决定二分、四分等视觉层级。
                    const int divisorGcd = std::gcd(beatIndex, beatDivisor);
                    denominator          = beatDivisor / divisorGcd;
                }

                // 整拍与不同细分等级使用不同透明度和线宽。
                const BeatLineStyle style = getBeatLineStyle(denominator);
                drawList.AddLine(ImVec2(lineX, rectMin.y),
                                 ImVec2(lineX, rectMax.y),
                                 style.color,
                                 style.width);
                lastColumn = column;
            }

            // 时间始终从整数索引重算，避免长循环累计浮点漂移。
            ++stepOffset;
            t = segmentStart + static_cast<double>(stepOffset) * stepDuration;
        }
    }
    drawList.PopClipRect();
}

/// @brief 在指定矩形区域叠加当前音频播放指针。
/// @param drawList 目标 ImGui 绘制列表。
/// @param rectMin 绘制区域左上角。
/// @param rectMax 绘制区域右下角。
/// @param viewStart 当前视图起始时间，单位为秒。
/// @param viewEnd 当前视图结束时间，单位为秒。
/// @warning UI 热路径约束如下。
/// 热路径：波形图和频谱图每帧执行；只读取当前播放路径、播放时间并绘制播放指针。
/// @details 指针使用路由快照中的视觉时间，只有位于当前视野时才绘制。
/// 深色外线提高亮背景对比，红色内线表达当前位置，顶部三角同时提供明确
/// 的拖动手柄视觉。该函数只绘制，命中和 seek 由独立处理器负责。
void BpmMeasurementToolView::drawPlaybackCursor(ImDrawList&   drawList,
                                                const ImVec2& rectMin,
                                                const ImVec2& rectMax,
                                                double        viewStart,
                                                double        viewEnd) const
{
    if ( viewEnd <= viewStart || rectMax.x <= rectMin.x ||
         rectMax.y <= rectMin.y || !isSelectedTrackLoadedForPlayback() ) {
        // 只有有效画布和已加载 transport 才能提供可靠游标位置。
        return;
    }

    const PlaybackTimelineState playbackState =
        readPlaybackTimelineState(m_playbackRoute);
    // 使用分析与播放后端时长的较大值，兼容异步状态更新。
    const double totalTime = std::max(m_duration, playbackState.totalTime);
    if ( totalTime <= 0.0 ) {
        return;
    }

    const double canvasTime = playbackState.visualTime;
    if ( canvasTime < viewStart || canvasTime > viewEnd ) {
        // 游标不在当前视野时无需提交任何几何。
        return;
    }

    // 视觉时间已包含路由偏移，可直接映射到共享画布坐标。
    const float lineX =
        rectMin.x +
        static_cast<float>((canvasTime - viewStart) / (viewEnd - viewStart) *
                           (rectMax.x - rectMin.x));

    // 深色外线与红色内线组合，在亮暗背景上均保持对比度。
    drawList.PushClipRect(rectMin, rectMax, true);
    drawList.AddLine(ImVec2(lineX, rectMin.y),
                     ImVec2(lineX, rectMax.y),
                     IM_COL32(80, 0, 0, 220),
                     4.0f);
    drawList.AddLine(ImVec2(lineX, rectMin.y),
                     ImVec2(lineX, rectMax.y),
                     IM_COL32(255, 40, 40, 255),
                     2.0f);
    // 顶部三角既是视觉指示，也是拖动命中区域的中心。
    const ImVec2 handleLeft(lineX - PLAYBACK_CURSOR_HANDLE_HALF_WIDTH,
                            rectMin.y);
    const ImVec2 handleRight(lineX + PLAYBACK_CURSOR_HANDLE_HALF_WIDTH,
                             rectMin.y);
    const ImVec2 handleTip(lineX, rectMin.y + PLAYBACK_CURSOR_HANDLE_HEIGHT);
    drawList.AddTriangleFilled(
        handleLeft, handleRight, handleTip, IM_COL32(80, 0, 0, 230));
    drawList.AddTriangleFilled(
        ImVec2(handleLeft.x + 1.0f, handleLeft.y + 1.0f),
        ImVec2(handleRight.x - 1.0f, handleRight.y + 1.0f),
        ImVec2(handleTip.x, handleTip.y - 1.0f),
        IM_COL32(255, 40, 40, 255));
    drawList.AddTriangle(
        handleLeft, handleRight, handleTip, IM_COL32(255, 170, 170, 220), 1.0f);
    drawList.PopClipRect();
}

/// @brief 处理 BPM 段落首拍红线和普通整拍白线的拖拽。
/// @param rectMin 交互区域左上角。
/// @param rectMax 交互区域右下角。
/// @param viewStart 当前视图起始时间，单位为秒。
/// @param viewEnd 当前视图结束时间，单位为秒。
/// @param interactionHovered ImGui 已裁决当前区域可接收鼠标输入时为 true。
/// @param ownerId 发起拖拽的视图标识，用于区分波形和频谱区域。
/// @warning UI 热路径约束如下。
/// 热路径：波形图和频谱图每帧执行；只处理鼠标状态和少量浮点计算，不访问文件系统。
/// @details 交互分为两种模式：
/// - SegmentStart 移动段起点并保持该段 BPM；
/// - BeatLength 移动非首拍并从整数拍索引反推 BPM。
/// 段起点可在一定像素范围内吸附到前一段分拍网格，但不能越过相邻段；
/// 拍线命中选择离鼠标最近的候选，并以固定扫描上限控制最坏帧成本。
void BpmMeasurementToolView::handleBeatMarkerDrag(
    const ImVec2& rectMin, const ImVec2& rectMax, double viewStart,
    double viewEnd, bool interactionHovered, int ownerId)
{
    ensureTimingSegments();
    const double canvasDuration = playbackCanvasDuration();
    if ( canvasDuration <= 0.0 || viewEnd <= viewStart ||
         rectMax.x <= rectMin.x || rectMax.y <= rectMin.y ) {
        if ( m_beatMarkerDragOwner == ownerId ) {
            // 发起区域失效时由所有者清理共享拖动状态。
            m_isBeatMarkerDragging = false;
            m_beatMarkerDragOwner  = 0;
            m_beatMarkerDragMode   = BeatMarkerDragMode::None;
        }
        return;
    }

    if ( m_isPlaybackCursorDragging ||
         (m_isBeatMarkerDragging && m_beatMarkerDragOwner != ownerId) ) {
        // 播放游标优先，且一次拖动只能由波形或频谱中的一个区域处理。
        return;
    }

    const double width   = rectMax.x - rectMin.x;
    auto         timeToX = [&](double time) {
        return rectMin.x + static_cast<float>((time - viewStart) /
                                              (viewEnd - viewStart) * width);
    };
    auto drawMarkerGlow = [&](double markerTime, BeatMarkerDragMode mode) {
        if ( markerTime < viewStart || markerTime > viewEnd ) {
            // 当前区域不可见的标记不绘制悬停反馈。
            return;
        }

        const float lineX = timeToX(markerTime);
        // 段起点保持红色语义，普通拍使用白色高亮。
        const ImU32 glowColor = mode == BeatMarkerDragMode::SegmentStart
                                    ? IM_COL32(255, 70, 70, 110)
                                    : IM_COL32(255, 255, 255, 105);
        const ImU32 coreColor = mode == BeatMarkerDragMode::SegmentStart
                                    ? IM_COL32(255, 60, 60, 240)
                                    : IM_COL32(255, 255, 255, 225);

        // 高亮在基础覆盖层之后绘制，并限制在当前交互区域。
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(rectMin, rectMax, true);
        drawList->AddLine(ImVec2(lineX, rectMin.y),
                          ImVec2(lineX, rectMax.y),
                          glowColor,
                          BEAT_MARKER_LINE_GLOW_WIDTH);
        drawList->AddLine(ImVec2(lineX, rectMin.y),
                          ImVec2(lineX, rectMax.y),
                          coreColor,
                          2.0f);
        drawList->PopClipRect();
    };

    // 命中扫描保留距离最近的线，解决高缩放下多个候选半径重叠。
    const ImVec2       mousePos = ImGui::GetMousePos();
    int64_t            hoveredIndex{ 0 };
    std::size_t        hoveredSegmentIndex{ 0 };
    double             hoveredMarkerTime{ 0.0 };
    float              hoveredDistance = std::numeric_limits<float>::max();
    bool               hasHoveredLine  = false;
    BeatMarkerDragMode hoveredMode{ BeatMarkerDragMode::None };

    auto considerMarkerLine = [&](double             markerTime,
                                  std::size_t        segmentIndex,
                                  int64_t            beatIndex,
                                  BeatMarkerDragMode mode) {
        // ImGui 必须先裁决区域 hover，避免被上层窗口遮挡时误捕获。
        if ( !interactionHovered ) return;
        if ( markerTime < viewStart - 1e-6 || markerTime > viewEnd + 1e-6 ) {
            // 仅扫描当前可视范围内的线。
            return;
        }

        const float  lineX = timeToX(markerTime);
        const ImVec2 hitMin(lineX - BEAT_MARKER_LINE_PICK_RADIUS, rectMin.y);
        const ImVec2 hitMax(lineX + BEAT_MARKER_LINE_PICK_RADIUS, rectMax.y);
        if ( !ImGui::IsMouseHoveringRect(hitMin, hitMax, true) ) {
            // 水平命中半径覆盖整条竖线，垂直范围由画布矩形限制。
            return;
        }

        const float distance = std::abs(mousePos.x - lineX);
        if ( distance < hoveredDistance ) {
            // 重叠命中时最靠近鼠标的候选获得拖动权。
            hoveredDistance     = distance;
            hoveredIndex        = beatIndex;
            hoveredSegmentIndex = segmentIndex;
            hoveredMarkerTime   = markerTime;
            hoveredMode         = mode;
            hasHoveredLine      = true;
        }
    };

    // 段起点数量通常很少，先扫描并赋予其与普通拍相同命中机制。
    for ( std::size_t segmentIndex = 0; segmentIndex < m_timingSegments.size();
          ++segmentIndex ) {
        considerMarkerLine(m_timingSegments[segmentIndex].timestampSeconds,
                           segmentIndex,
                           0,
                           BeatMarkerDragMode::SegmentStart);
    }

    // 极端缩放和 BPM 下限制每帧拍线命中扫描数量。
    constexpr int64_t MAX_SCANNED_BEAT_HANDLES = 4096;
    int64_t           scannedHandleCount       = 0;

    for ( std::size_t segmentIndex = 0;
          segmentIndex < m_timingSegments.size() &&
          scannedHandleCount < MAX_SCANNED_BEAT_HANDLES;
          ++segmentIndex ) {
        const auto&  segment      = m_timingSegments[segmentIndex];
        const double beatLength   = segmentBeatLengthSeconds(segmentIndex);
        const double segmentStart = segment.timestampSeconds;
        const double segmentEnd =
            segmentIndex + 1 < m_timingSegments.size()
                ? m_timingSegments[segmentIndex + 1].timestampSeconds
                : viewEnd + beatLength;
        if ( beatLength <= 1e-6 ||
             (segmentIndex + 1 < m_timingSegments.size() &&
              segmentEnd <= segmentStart + 1e-9) ) {
            // 无效拍长或区间不能产生可拖动整拍。
            continue;
        }

        const double visibleStart =
            segmentIndex == 0 ? viewStart : std::max(viewStart, segmentStart);
        const double visibleEnd = std::min(viewEnd, segmentEnd);
        if ( visibleEnd < visibleStart ) {
            // 完全不可见段跳过，无需从段首逐拍枚举。
            continue;
        }

        // 索引从 1 起，因为 index 0 已作为段起点单独处理。
        const int64_t firstIndex =
            std::max<int64_t>(1,
                              static_cast<int64_t>(std::ceil(
                                  (visibleStart - segmentStart) / beatLength)));
        const int64_t lastIndex = static_cast<int64_t>(
            std::floor((visibleEnd - segmentStart) / beatLength));
        for ( int64_t index = firstIndex;
              index <= lastIndex &&
              scannedHandleCount < MAX_SCANNED_BEAT_HANDLES;
              ++index, ++scannedHandleCount ) {
            const double beatTime =
                segmentStart + static_cast<double>(index) * beatLength;
            if ( segmentIndex + 1 < m_timingSegments.size() &&
                 beatTime >= segmentEnd + 1e-6 ) {
                // 段边界拍由下一段的起点控制，避免重复命中。
                continue;
            }

            considerMarkerLine(
                beatTime, segmentIndex, index, BeatMarkerDragMode::BeatLength);
        }
    }

    if ( hasHoveredLine || m_isBeatMarkerDragging ) {
        // 拖动过程中即使鼠标离开线也维持水平调整光标。
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    if ( hasHoveredLine && !m_isBeatMarkerDragging ) {
        // 尚未按下时只绘制预选高亮，不修改 Timing。
        drawMarkerGlow(hoveredMarkerTime, hoveredMode);
    }

    if ( hasHoveredLine && ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
        // 捕获候选身份，后续帧不再受重新命中其他线影响。
        m_isBeatMarkerDragging    = true;
        m_isTimelinePanning       = false;
        m_beatMarkerDragOwner     = ownerId;
        m_beatMarkerDragMode      = hoveredMode;
        m_draggedBeatIndex        = hoveredIndex;
        m_draggedBeatSegmentIndex = hoveredSegmentIndex;
    }

    if ( !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        if ( m_beatMarkerDragOwner == ownerId ) {
            // 鼠标在任意位置松开都结束所属区域的拖动。
            m_isBeatMarkerDragging = false;
            m_beatMarkerDragOwner  = 0;
            m_beatMarkerDragMode   = BeatMarkerDragMode::None;
        }
        return;
    }

    if ( !m_isBeatMarkerDragging ) {
        // 当前区域没有持有拖动时不计算目标时间。
        return;
    }

    if ( m_draggedBeatSegmentIndex >= m_timingSegments.size() ||
         m_beatMarkerDragMode == BeatMarkerDragMode::None ) {
        // 段列表在拖动期间变化时放弃本帧更新，避免越界引用。
        return;
    }

    const double rectWidth = std::max(1.0f, rectMax.x - rectMin.x);
    const double relX =
        std::clamp<double>((mousePos.x - rectMin.x) / rectWidth, 0.0, 1.0);
    // 鼠标只钳制到交互矩形；首段起点稍后可按专用规则允许负值。
    const double targetMarkerTime = viewStart + relX * (viewEnd - viewStart);
    const double targetBeatTime =
        std::clamp<double>(targetMarkerTime, 0.0, canvasDuration);
    auto& segment = m_timingSegments[m_draggedBeatSegmentIndex];

    if ( m_beatMarkerDragMode == BeatMarkerDragMode::SegmentStart ) {
        // 拖段起点保持本段 BPM，仅改变它与相邻段的时间边界。
        const double beatLength =
            segmentBeatLengthSeconds(m_draggedBeatSegmentIndex);
        /// @brief 将非首段段落起点吸附到前一段落的分拍网格。
        auto snapSegmentStartToPreviousSubdivision =
            [&](double candidateTime,
                double minSegmentStart,
                double maxSegmentStart) {
                if ( m_draggedBeatSegmentIndex == 0 ) {
                    // 第一段没有前一网格，允许连续自由拖动。
                    return candidateTime;
                }

                const std::size_t previousSegmentIndex =
                    m_draggedBeatSegmentIndex - 1;
                const double previousBeatLength =
                    segmentBeatLengthSeconds(previousSegmentIndex);
                const int    beatDivisor = std::clamp(m_beatDivisor, 1, 64);
                const double stepDuration =
                    previousBeatLength / static_cast<double>(beatDivisor);
                if ( stepDuration <= 1e-6 ) {
                    // 前一段无有效细分步长时关闭吸附。
                    return candidateTime;
                }

                const double previousSegmentStart =
                    m_timingSegments[previousSegmentIndex].timestampSeconds;
                // 以相邻前段的首拍为原点寻找最近分拍。
                const double nearestStep = std::round(
                    (candidateTime - previousSegmentStart) / stepDuration);
                const double snappedTime =
                    previousSegmentStart + nearestStep * stepDuration;
                if ( snappedTime < minSegmentStart - 1e-6 ||
                     snappedTime > maxSegmentStart + 1e-6 ) {
                    // 吸附结果不能越过相邻段边界。
                    return candidateTime;
                }

                // 像素半径换算为时间阈值，并限制在不足半个分拍内。
                const double pixelSnapThreshold =
                    static_cast<double>(BEAT_MARKER_SEGMENT_SNAP_RADIUS) /
                    rectWidth * (viewEnd - viewStart);
                const double snapThreshold =
                    std::min(pixelSnapThreshold, stepDuration * 0.45);
                if ( std::abs(snappedTime - candidateTime) > snapThreshold ) {
                    // 鼠标距离网格过远时保持连续拖动手感。
                    return candidateTime;
                }

                return snappedTime;
            };

        // 首段允许一拍负偏移；其他段必须严格晚于前一段。
        double minSegmentStart =
            m_draggedBeatSegmentIndex == 0
                ? firstBeatMinSeconds(beatLength)
                : m_timingSegments[m_draggedBeatSegmentIndex - 1]
                          .timestampSeconds +
                      1e-4;
        double maxSegmentStart = canvasDuration;
        if ( m_draggedBeatSegmentIndex + 1 < m_timingSegments.size() ) {
            // 非末段同样不能越过下一段，保留微小严格递增间隔。
            maxSegmentStart =
                std::min(maxSegmentStart,
                         m_timingSegments[m_draggedBeatSegmentIndex + 1]
                                 .timestampSeconds -
                             1e-4);
        }
        maxSegmentStart = std::max(minSegmentStart, maxSegmentStart);

        // 先尝试吸附，再做最终边界钳制。
        const double snappedStart = snapSegmentStartToPreviousSubdivision(
            targetMarkerTime, minSegmentStart, maxSegmentStart);
        const double clampedStart =
            std::clamp(snappedStart, minSegmentStart, maxSegmentStart);
        segment.timestampSeconds = clampedStart;
        // 第一段变化需要同步兼容字段，任意段变化都重置节拍器网格。
        syncPrimaryTimingFieldsFromSegments();
        resetMetronomeScheduler(clampedStart);
        drawMarkerGlow(clampedStart, BeatMarkerDragMode::SegmentStart);

        const std::string tooltip =
            fmt::format("{} / {:.3f} BPM",
                        MMM::UI::Utils::formatCanvasTime(clampedStart),
                        segment.bpm);
        ImGui::SetTooltip("%s", tooltip.c_str());
        return;
    }

    if ( m_draggedBeatIndex <= 0 ) {
        // BeatLength 模式只处理非首拍，避免除以零。
        return;
    }

    // 最大 BPM 决定该拍相对段首允许的最小距离。
    const double minTargetTime =
        segment.timestampSeconds + static_cast<double>(m_draggedBeatIndex) *
                                       (60.0 / ::MMM::MAX_NORMALIZED_BPM);
    const double clampedTargetTime = std::max(minTargetTime, targetBeatTime);
    // 用拖动拍的整数索引反推统一拍长，而不是只移动单根线。
    const double beatLength = (clampedTargetTime - segment.timestampSeconds) /
                              static_cast<double>(m_draggedBeatIndex);
    if ( beatLength > 1e-6 && std::isfinite(beatLength) ) {
        // 归一化后的 BPM 回写段模型，并同步首段兼容字段。
        segment.bpm = ::MMM::normalizeBpmValue(60.0 / beatLength);
        syncPrimaryTimingFieldsFromSegments();
        resetMetronomeScheduler(clampedTargetTime);
    }
    drawMarkerGlow(clampedTargetTime, BeatMarkerDragMode::BeatLength);

    const std::string tooltip =
        fmt::format("{} / {:.3f} BPM",
                    MMM::UI::Utils::formatCanvasTime(clampedTargetTime),
                    segment.bpm);
    ImGui::SetTooltip("%s", tooltip.c_str());
}

/// @brief 处理播放指针顶部三角手柄的拖拽预览和松手跳转。
/// @param rectMin 交互区域左上角。
/// @param rectMax 交互区域右下角。
/// @param viewStart 当前视图起始时间，单位为秒。
/// @param viewEnd 当前视图结束时间，单位为秒。
/// @param interactionHovered ImGui 已裁决当前区域可接收鼠标输入时为 true。
/// @param ownerId 发起拖拽的视图标识，用于区分波形和频谱区域。
/// @warning UI 热路径约束如下。
/// 热路径：波形图和频谱图每帧执行；拖到边缘时只滚动视野并更新预览，
/// 松手后才执行实际播放跳转，不访问文件系统。
/// @details 捕获后仅更新待提交画布时间并绘制半透明预览游标；鼠标进入
/// 左右边缘区会按帧时间连续移动视野。松手时只发送一次最终 seek，避免
/// 高频重置音频图；ownerId 防止波形和频谱同时处理同一次拖动。
void BpmMeasurementToolView::handlePlaybackCursorDrag(
    const ImVec2& rectMin, const ImVec2& rectMax, double viewStart,
    double viewEnd, bool interactionHovered, int ownerId)
{
    if ( viewEnd <= viewStart || rectMax.x <= rectMin.x ||
         rectMax.y <= rectMin.y || !isSelectedTrackLoadedForPlayback() ) {
        if ( m_playbackCursorDragOwner == ownerId ) {
            // 所属画布失效时取消预览，避免松手后跳转到过期位置。
            m_isPlaybackCursorDragging = false;
            m_playbackCursorDragOwner  = 0;
            m_hasPendingPlaybackSeek   = false;
        }
        return;
    }

    const PlaybackTimelineState playbackState =
        readPlaybackTimelineState(m_playbackRoute);
    // 分析时长可早于后端加载完成，取较大值形成稳定可拖范围。
    const double totalTime = std::max(m_duration, playbackState.totalTime);
    if ( totalTime <= 0.0 ) {
        if ( m_playbackCursorDragOwner == ownerId ) {
            // 时长消失表示 transport 已卸载，清理当前区域的捕获状态。
            m_isPlaybackCursorDragging = false;
            m_playbackCursorDragOwner  = 0;
            m_hasPendingPlaybackSeek   = false;
        }
        return;
    }

    if ( m_isPlaybackCursorDragging && m_playbackCursorDragOwner != ownerId ) {
        // 波形和频谱共享一个游标，只有发起拖动的区域继续更新。
        return;
    }

    // 命中位置使用实际播放视觉时间；拖动后另存预览值而不改后端。
    const double canvasTime  = playbackState.visualTime;
    const bool cursorVisible = canvasTime >= viewStart && canvasTime <= viewEnd;
    const float lineX =
        rectMin.x +
        static_cast<float>((canvasTime - viewStart) / (viewEnd - viewStart) *
                           (rectMax.x - rectMin.x));
    // 三角手柄周围增加两像素容差，竖线本身不抢占平移交互。
    const ImVec2 handleMin(lineX - PLAYBACK_CURSOR_HANDLE_HALF_WIDTH - 2.0f,
                           rectMin.y);
    const ImVec2 handleMax(lineX + PLAYBACK_CURSOR_HANDLE_HALF_WIDTH + 2.0f,
                           rectMin.y + PLAYBACK_CURSOR_HANDLE_HEIGHT + 3.0f);
    const bool   hoverHandle =
        interactionHovered && cursorVisible &&
        ImGui::IsMouseHoveringRect(handleMin, handleMax, true);

    if ( hoverHandle || m_isPlaybackCursorDragging ) {
        // 捕获后鼠标可离开手柄，仍保持横向拖动光标。
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if ( hoverHandle && !m_isPlaybackCursorDragging ) {
        // 仅悬停时绘制宽光晕，提示顶部手柄可以拖动。
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(rectMin, rectMax, true);
        drawList->AddLine(ImVec2(lineX, rectMin.y),
                          ImVec2(lineX, rectMax.y),
                          IM_COL32(255, 80, 80, 100),
                          BEAT_MARKER_LINE_GLOW_WIDTH);
        drawList->AddLine(ImVec2(lineX, rectMin.y),
                          ImVec2(lineX, rectMax.y),
                          IM_COL32(255, 80, 80, 220),
                          2.0f);
        drawList->PopClipRect();
    }

    if ( hoverHandle && ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
        // 捕获时冻结发起区域身份，并取消可能存在的画布平移。
        m_isPlaybackCursorDragging = true;
        m_isTimelinePanning        = false;
        m_playbackCursorDragOwner  = ownerId;
        m_pendingPlaybackSeekCanvasTime =
            // 初始预览从当前游标开始，避免首帧产生跳变。
            std::clamp<double>(canvasTime, 0.0, playbackCanvasDuration());
        m_hasPendingPlaybackSeek = true;
    }

    if ( !m_isPlaybackCursorDragging ) {
        // 未捕获手柄时让时间线导航处理剩余鼠标输入。
        return;
    }

    ImGuiIO&     io        = ImGui::GetIO();
    const bool   mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const double rectWidth = std::max(1.0f, rectMax.x - rectMin.x);

    // 拖到边缘时视野会移动，后续像素换算必须使用移动后的预览范围。
    double previewViewStart = viewStart;
    double previewViewEnd   = viewEnd;
    float  edgeDistance     = 0.0f;
    if ( io.MousePos.x < rectMin.x + PLAYBACK_CURSOR_EDGE_SCROLL_MARGIN ) {
        // 左侧边缘距离为负，直接决定向前滚动方向。
        edgeDistance =
            io.MousePos.x - (rectMin.x + PLAYBACK_CURSOR_EDGE_SCROLL_MARGIN);
    } else if ( io.MousePos.x >
                rectMax.x - PLAYBACK_CURSOR_EDGE_SCROLL_MARGIN ) {
        // 右侧边缘距离为正，决定向后滚动方向。
        edgeDistance =
            io.MousePos.x - (rectMax.x - PLAYBACK_CURSOR_EDGE_SCROLL_MARGIN);
    }

    // 复用预览系统的边缘滚动灵敏度，保持跨画布交互手感一致。
    const float edgeScrollSensitivity =
        std::max(0.0f,
                 Config::AppConfig::instance()
                     .getVisualConfig()
                     .previewConfig.edgeScrollSensitivity);
    if ( mouseDown && std::abs(edgeDistance) > 0.001f &&
         edgeScrollSensitivity > 0.0f ) {
        // 限制单帧时间跨度，避免窗口恢复或调试暂停后一次越过过长距离。
        const double frameSeconds = std::clamp<double>(io.DeltaTime, 0.0, 0.1);
        const double scrollDelta  = static_cast<double>(edgeDistance) *
                                    edgeScrollSensitivity * frameSeconds;
        m_viewCenter              = std::clamp<double>(
            m_viewCenter + scrollDelta, 0.0, playbackCanvasDuration());
        // 边缘滚动属于用户视野选择，需要纳入持久化偏好。
        markUserPreferencesChanged(false, true);
        const double clampedCenter =
            std::clamp<double>(m_viewCenter, 0.0, playbackCanvasDuration());
        // 当前帧立即重算预览边界，游标跟随滚动而非滞后一帧。
        previewViewStart = std::max(0.0, clampedCenter - m_zoomSeconds);
        previewViewEnd   = std::min(
            std::max(playbackCanvasDuration(), previewViewStart + 0.001),
            clampedCenter + m_zoomSeconds);
    }

    // 鼠标超出画布时钳制到边缘，结合边缘滚动连续扩展时间范围。
    const double relX =
        std::clamp<double>((io.MousePos.x - rectMin.x) / rectWidth, 0.0, 1.0);
    const double previewViewRange =
        std::max(0.001, previewViewEnd - previewViewStart);
    const double targetCanvasTime =
        std::clamp<double>(previewViewStart + relX * previewViewRange,
                           0.0,
                           playbackCanvasDuration());
    // 拖动期间只更新预览值，不向音频线程发送连续 seek。
    m_pendingPlaybackSeekCanvasTime = targetCanvasTime;
    m_hasPendingPlaybackSeek        = true;

    if ( !mouseDown ) {
        // 松手是提交点，只执行一次最终 seek，避免播放线程反复重置。
        seekPlaybackToCanvasTime(m_pendingPlaybackSeekCanvasTime);
        markUserPreferencesChanged(false, true);
        m_isPlaybackCursorDragging = false;
        m_playbackCursorDragOwner  = 0;
        m_hasPendingPlaybackSeek   = false;
        return;
    }

    // 后端游标保持原位，额外绘制半透明预览游标表达待提交位置。
    const float previewX =
        rectMin.x +
        static_cast<float>((targetCanvasTime - previewViewStart) /
                           previewViewRange * (rectMax.x - rectMin.x));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(rectMin, rectMax, true);
    drawList->AddLine(ImVec2(previewX, rectMin.y),
                      ImVec2(previewX, rectMax.y),
                      IM_COL32(255, 80, 80, 100),
                      BEAT_MARKER_LINE_GLOW_WIDTH);
    drawList->AddLine(ImVec2(previewX, rectMin.y),
                      ImVec2(previewX, rectMax.y),
                      IM_COL32(255, 80, 80, 185),
                      2.0f);
    drawList->AddTriangleFilled(
        ImVec2(previewX - PLAYBACK_CURSOR_HANDLE_HALF_WIDTH, rectMin.y),
        ImVec2(previewX + PLAYBACK_CURSOR_HANDLE_HALF_WIDTH, rectMin.y),
        ImVec2(previewX, rectMin.y + PLAYBACK_CURSOR_HANDLE_HEIGHT),
        IM_COL32(255, 80, 80, 180));
    drawList->PopClipRect();

    // 工具提示展示待 seek 的精确画布时间。
    ImGui::SetTooltip(
        "%s", MMM::UI::Utils::formatCanvasTime(targetCanvasTime).c_str());
}

/// @brief 处理分析视图的滚轮缩放和鼠标拖动平移。
/// @param rectMin 交互区域左上角。
/// @param rectMax 交互区域右下角。
/// @param viewStart 当前视图起始时间，单位为秒。
/// @param viewEnd 当前视图结束时间，单位为秒。
/// @param interactionHovered ImGui 已裁决当前区域可接收鼠标输入时为 true。
/// @warning UI 热路径：波形图和频谱图每帧执行；只处理鼠标状态和少量浮点计算。
/// @details 滚轮缩放以鼠标下时间为锚点，左键单击拖动平移视野；双击保留
/// 给首拍设置。具体对象拖动期间背景导航被禁用，所有结果即时更新本地视野
/// 并通过消抖偏好写入记录，不等待任何固定时长同步。
void BpmMeasurementToolView::handleTimelineNavigation(const ImVec2& rectMin,
                                                      const ImVec2& rectMax,
                                                      double        viewStart,
                                                      double        viewEnd,
                                                      bool interactionHovered)
{
    const double canvasDuration = playbackCanvasDuration();
    if ( canvasDuration <= 0.0 || rectMax.x <= rectMin.x ||
         viewEnd <= viewStart ) {
        // 交互几何失效时清除平移捕获，避免恢复布局后意外继续拖动。
        m_isTimelinePanning = false;
        return;
    }

    if ( m_isPlaybackCursorDragging || m_isBeatMarkerDragging ) {
        // 具体对象拖动优先于背景平移，互斥状态避免同帧双重变换。
        m_isTimelinePanning = false;
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    // 同时要求 ImGui 区域裁决和矩形命中，尊重遮挡与弹窗。
    const bool hover =
        interactionHovered && ImGui::IsMouseHoveringRect(rectMin, rectMax);
    if ( hover || m_isTimelinePanning ) {
        // 平移捕获后允许鼠标越界，仍显示横向调整光标。
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    const double rectWidth = std::max(1.0f, rectMax.x - rectMin.x);
    const double viewRange = viewEnd - viewStart;
    // 中心点始终位于画布时间域，视野本身可在首尾被裁切。
    auto clampCenter = [&](double center) {
        return std::clamp(center, 0.0, std::max(0.0, canvasDuration));
    };

    if ( hover && io.MouseWheel != 0.0f ) {
        // 缩放锚定鼠标下的时间，避免波形内容在滚轮时横向漂移。
        const double mouseRatio = std::clamp<double>(
            (io.MousePos.x - rectMin.x) / rectWidth, 0.0, 1.0);
        const double anchorTime = viewStart + mouseRatio * viewRange;
        // 指数倍率使不同滚轮步数形成平滑、方向对称的缩放。
        const double zoomFactor = std::pow(0.86, io.MouseWheel);
        const double maxZoom = std::max(0.1, std::max(canvasDuration, 120.0));
        const double nextZoom =
            std::clamp(m_zoomSeconds * zoomFactor, 0.01, maxZoom);
        // 由锚点在新视野中的相同比例反解新的中心位置。
        const double nextViewStart = anchorTime - mouseRatio * nextZoom * 2.0;
        m_zoomSeconds              = nextZoom;
        m_viewCenter               = clampCenter(nextViewStart + nextZoom);
        markUserPreferencesChanged(false, true);
    }

    if ( hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
         !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ) {
        // 双击保留给首拍设置，单击拖动才捕获背景平移。
        m_isTimelinePanning = true;
    }

    if ( !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        // 在区域外释放也结束平移捕获。
        m_isTimelinePanning = false;
    }

    if ( m_isTimelinePanning &&
         ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f) ) {
        // 向右拖内容等价于时间中心向前移动，因此使用负号。
        const double timeDelta =
            -static_cast<double>(io.MouseDelta.x) / rectWidth * viewRange;
        m_viewCenter = clampCenter(m_viewCenter + timeDelta);
        markUserPreferencesChanged(false, true);
    }
}

/// @brief 查找当前选中的音频资源。
/// @return 成功时返回音频资源副本，否则返回空。
/// @warning UI 热路径低频调用：只遍历当前项目音频资源，不访问文件系统。
/// @details 返回副本隔离项目容器的重分配和会话切换；后续需要路径身份时
/// 仍由专用入口重新绑定当前项目，避免长期缓存资源引用。
std::optional<AudioResource>
BpmMeasurementToolView::selectedAudioResource() const
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || m_selectedAudioTrackId.empty() ) {
        // 资源查找始终限定在当前项目，禁止跨项目复用旧 ID。
        return std::nullopt;
    }

    for ( const auto& resource : project->m_audioResources ) {
        if ( resource.m_id == m_selectedAudioTrackId ) {
            // 返回值副本避免调用方持有项目资源容器中的短期引用。
            return resource;
        }
    }

    return std::nullopt;
}

/// @brief 更新当前选择并刷新规范化音频同步键。
/// @param audioTrackId 新的项目音频资源 ID。
/// @warning 低频路径：选择变化时可能规范化一次文件系统路径。
/// @details 选择变化会使独立试听、派生路径、同步键、播放路由和分析缓存
/// 依次失效；统一入口保证所有调用场景执行相同的刷新顺序。
void BpmMeasurementToolView::setSelectedAudioTrackId(
    const std::string& audioTrackId)
{
    if ( m_selectedAudioTrackId != audioTrackId ) {
        // 独立试听轨可能引用旧资源，选择切换前立即卸载。
        Audio::AudioManager::instance().unloadAuditionTrack();
    }

    // 清空派生身份缓存，随后从当前项目路径重新建立同步键。
    m_selectedAudioTrackId = audioTrackId;
    m_selectedAudioProjectRoot.clear();
    m_selectedAudioResourcePath.clear();
    m_selectedAudioSyncKey.clear();
    refreshSelectedAudioIdentity();
    refreshPlaybackRoute();
}

/// @brief 在项目根目录或资源路径变化时刷新选中音轨身份缓存。
/// @warning UI
/// 热路径：每帧只比较当前项目音轨的路径字段；仅脏分支规范化文件路径。
/// @details 快路径只比较项目根、资源相对路径和非空同步键；变化时卸载
/// 旧试听轨，更新资源倍速，并用 weakly_canonical 构造跨表示稳定的 UTF-8 键。
/// 文件暂不存在时使用词法规范化，实际存在性留给加载或分析入口检查。
void BpmMeasurementToolView::refreshSelectedAudioIdentity()
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || m_selectedAudioTrackId.empty() ) {
        if ( !m_selectedAudioSyncKey.empty() ) {
            // 上一身份已加载独立试听时，失去项目或选择必须卸载。
            Audio::AudioManager::instance().unloadAuditionTrack();
        }
        m_selectedAudioProjectRoot.clear();
        m_selectedAudioResourcePath.clear();
        m_selectedAudioSyncKey.clear();
        // 空身份不应触发后台分析重试。
        m_selectedAudioIdentityNeedsAnalysis = false;
        return;
    }

    // 每帧只按 ID 扫描项目的小型音频资源列表，不执行文件操作。
    const AudioResource* selectedResource = nullptr;
    for ( const auto& resource : project->m_audioResources ) {
        if ( resource.m_id == m_selectedAudioTrackId ) {
            selectedResource = &resource;
            break;
        }
    }
    if ( !selectedResource ) {
        // 项目已删除所选资源时清除所有派生状态。
        Audio::AudioManager::instance().unloadAuditionTrack();
        m_selectedAudioProjectRoot.clear();
        m_selectedAudioResourcePath.clear();
        m_selectedAudioSyncKey.clear();
        m_selectedAudioIdentityNeedsAnalysis = false;
        return;
    }

    if ( m_selectedAudioProjectRoot == project->m_projectRoot &&
         m_selectedAudioResourcePath == selectedResource->m_path &&
         !m_selectedAudioSyncKey.empty() ) {
        // 项目根、相对路径和既有键都未变化时复用规范化结果。
        return;
    }

    // 身份变化会让独立试听和分析缓存失效，先释放旧 transport。
    Audio::AudioManager::instance().unloadAuditionTrack();
    m_selectedAudioProjectRoot  = project->m_projectRoot;
    m_selectedAudioResourcePath = selectedResource->m_path;
    m_playbackSpeed =
        std::clamp<double>(selectedResource->m_config.playbackSpeed, 0.25, 2.0);
    // 同步键以项目根和资源相对路径组合后的绝对身份为基础。
    const auto audioPath =
        project->m_projectRoot / Config::utf8ToPath(selectedResource->m_path);
    std::error_code canonicalError;
    auto            canonicalPath =
        std::filesystem::weakly_canonical(audioPath, canonicalError);
    if ( canonicalError ) {
        // 文件暂不存在时退回纯词法规范化，仍得到稳定可比较的键。
        canonicalPath = audioPath.lexically_normal();
    }
    m_selectedAudioSyncKey = Config::pathToUtf8(canonicalPath);
    // 延迟到 update 启动分析，避免身份刷新函数直接创建工作线程。
    m_selectedAudioIdentityNeedsAnalysis = true;
}

/// @brief 将 BPM 工具固定路由到独立试听通道。
/// @warning UI 热路径：每帧调用一次，只比较本地缓存路由。
/// @details 路由解析集中在此处，调用方无需区分以下状态：
/// - 没有选中音轨时不可播放；
/// - 普通 BPM 工具音轨使用独立试听通道；
/// - 若未来恢复与编辑器同轨同步，切换时必须释放不再使用的试听轨；
/// - 路由变化会使已有节拍器前瞻计划失效。
void BpmMeasurementToolView::refreshPlaybackRoute()
{
    // 路由判断依赖最新规范化同步键，先刷新低频身份缓存。
    refreshSelectedAudioIdentity();
    const BpmPlaybackRoute nextRoute =
        resolveBpmPlaybackRoute(m_selectedAudioSyncKey, std::string_view{});
    if ( nextRoute != m_playbackRoute ) {
        if ( nextRoute != BpmPlaybackRoute::Audition ) {
            // 离开独立试听时主动释放资源，避免两条 transport 同时占用音频图。
            Audio::AudioManager::instance().unloadAuditionTrack();
        }
        // 新路由的时间轴可能不同，节拍器必须在下帧重新对齐。
        m_playbackRoute                = nextRoute;
        m_metronomeScheduleInitialized = false;
    }

    if ( m_playbackRoute == BpmPlaybackRoute::SynchronizedWithEditor ) {
        // 同步模式下编辑器是倍速真源，工具只镜像其当前请求值。
        m_playbackSpeed = std::clamp(
            Audio::AudioManager::instance().getPlaybackSpeed(), 0.25, 2.0);
    }
}

/// @brief 判断当前播放控制是否应与活动编辑器同步。
/// @return 同轨同步路由返回 true。
/// @note 此判断只解释缓存路由，不访问音频设备或项目文件。
bool BpmMeasurementToolView::isPlaybackSynchronizedWithEditor() const
{
    return shouldDispatchBpmPlaybackToEditor(m_playbackRoute);
}

/// @brief 切换当前选中音轨的播放或暂停状态。
/// @return 成功切换或暂停已有播放时返回 true。
/// @details 已播放状态优先直接暂停；未播放状态会按需加载音轨。
/// 播放位置已抵达结尾时先回到画布起点，避免立即再次停止。
bool BpmMeasurementToolView::togglePlayback()
{
    // 用户可能在上帧切换会话或资源，按钮响应前刷新路由。
    refreshPlaybackRoute();
    if ( m_playbackRoute == BpmPlaybackRoute::Unavailable ) {
        // 不可用路由没有合法 transport，向快捷键调用方报告失败。
        return false;
    }

    auto&      audio       = Audio::AudioManager::instance();
    const bool trackLoaded = isSelectedTrackLoadedForPlayback();
    if ( trackLoaded && readPlaybackTimelineState(m_playbackRoute).isPlaying ) {
        // 暂停已有播放不需要重新检查文件或重新加载资源。
        setPlaybackState(false);
        return true;
    }

    if ( !loadSelectedTrackForPlayback() ) {
        // 加载函数已经设置面向用户的具体失败状态。
        return false;
    }

    const double totalTime = getPlaybackTotalTime(m_playbackRoute, audio);
    if ( totalTime > 0.0 &&
         getPlaybackCurrentTime(m_playbackRoute, audio) >= totalTime - 0.001 ) {
        // 末尾一毫秒内视为已结束，再次播放从起点开始。
        seekPlaybackToCanvasTime(0.0);
    }
    setPlaybackState(true);
    return true;
}

/// @brief 确保当前选中音轨已加载到播放图。
/// @return 加载成功或已经加载时返回 true。
/// @warning 用户触发的低频路径：会检查文件存在性并可能创建解码资源。
/// @details 同步路由只验证编辑器主时间线；独立路由按同步键复用试听轨，
/// 从当前视图中心附近开始，并在每次调用后应用工具倍速。
bool BpmMeasurementToolView::loadSelectedTrackForPlayback()
{
    // 播放开始前准备节拍器，避免首次拍点才同步加载音效。
    (void)ensureMetronomeSoundEffects();
    refreshPlaybackRoute();

    auto resource = selectedAudioResource();
    auto path     = selectedAudioAbsolutePath();
    if ( !resource || !path ) {
        // 选择失效与未选择共用引导状态，避免暴露内部资源 ID。
        m_statusText = TR("ui.tools.bpm_measure.select_track").data();
        return false;
    }

    std::error_code ec;
    if ( !std::filesystem::exists(*path, ec) || ec ) {
        // 使用 error_code 遵守无异常约束，权限错误也按不可用文件处理。
        m_statusText = TR("ui.tools.bpm_measure.file_missing").data();
        return false;
    }

    const std::string pathString = Config::pathToUtf8(*path);
    auto&             audio      = Audio::AudioManager::instance();
    if ( isPlaybackSynchronizedWithEditor() ) {
        // 同步路由严禁在工具内替换编辑器已加载的主音频时间线。
        if ( !audio.hasLoadedAudioTimeline() ) {
            m_statusText = TR("ui.tools.bpm_measure.load_failed").data();
            return false;
        }
        return true;
    }

    // 规范化同步键相等表示当前试听解码资源可以直接复用。
    const bool needsAuditionLoad =
        audio.getLoadedAuditionSyncKey() != m_selectedAudioSyncKey;
    if ( needsAuditionLoad ) {
        // 资源配置包含音量、偏移和解码设置，必须随路径一起交给后端。
        if ( !audio.loadAuditionTrack(pathString, resource->m_config) ) {
            m_statusText = TR("ui.tools.bpm_measure.load_failed").data();
            return false;
        }
        const double startAudioTime =
            // 视图中心是画布时间，加载后需移除视觉偏移再 seek 音频。
            std::max(0.0, m_viewCenter - playbackVisualOffset());
        audio.seekAudition(startAudioTime);
    }
    // 即使复用已加载轨道，也要同步可能刚修改的工具倍速。
    audio.setAuditionPlaybackSpeed(m_playbackSpeed);
    return true;
}

/// @brief 判断播放图当前加载的是否为选中音轨。
/// @return 当前加载音轨与选中音轨路径一致时返回 true。
/// @warning UI 热路径：每帧读取播放路径；不得在此加入文件存在性检查或音频加载。
/// @details 同步路由由活动编辑器时间线的存在性判定；独立试听还要求后端
/// 已加载同步键与当前规范化键完全相等，防止误控制上一选择的资源。
bool BpmMeasurementToolView::isSelectedTrackLoadedForPlayback() const
{
    if ( m_selectedAudioSyncKey.empty() ) {
        // 空同步键不能与后端空值偶然匹配为已加载。
        return false;
    }

    const auto& audio = Audio::AudioManager::instance();
    if ( isPlaybackSynchronizedWithEditor() ) {
        // 同步路由身份已由路由解析保证，只需确认主时间线存在。
        return audio.hasLoadedAudioTimeline();
    }
    // 独立试听必须同时匹配路由类型与规范化资源键。
    return m_playbackRoute == BpmPlaybackRoute::Audition &&
           audio.getLoadedAuditionSyncKey() == m_selectedAudioSyncKey;
}

/// @brief 应用 BPM 工具倍速；同轨时同步编辑器，异轨时只修改独立试听。
/// @param speed 目标倍速。
/// @note 未加载音轨时只缓存请求值，首次加载后再应用到后端。
void BpmMeasurementToolView::applyPlaybackSpeed(double speed)
{
    // UI 与音频后端共同支持 0.25x–2.0x，入口统一钳制。
    m_playbackSpeed = std::clamp(speed, 0.25, 2.0);
    if ( isSelectedTrackLoadedForPlayback() ) {
        if ( isPlaybackSynchronizedWithEditor() ) {
            // 编辑器路由通过命令队列更新，以纳入逻辑线程状态管理。
            Logic::EditorEngine::instance().pushCommand(
                Logic::CmdSetPlaybackSpeed{ m_playbackSpeed });
        } else {
            // 独立试听不影响谱面会话，可直接调用专用 transport。
            Audio::AudioManager::instance().setAuditionPlaybackSpeed(
                m_playbackSpeed);
        }
    }
}

/// @brief 从全局配置恢复不依赖具体音轨的 BPM 工具偏好。
/// @details 非有限浮点配置回退默认值，合法值也会限制到当前 UI 范围；
/// 音轨、BPM 和 Timing 段属于项目工作状态，不在此偏好结构中恢复。
void BpmMeasurementToolView::restoreUserPreferences()
{
    // 持有只读引用期间不调用 save，避免配置对象被重建。
    const auto& preferences = Config::AppConfig::instance()
                                  .getEditorSettings()
                                  .bpmMeasurementToolPreferences;
    // 旧版本或手工编辑配置可能包含 NaN，先替换再钳制。
    m_markerWidthMs = std::clamp(std::isfinite(preferences.markerWidthMs)
                                     ? preferences.markerWidthMs
                                     : 80.0,
                                 4.0,
                                 1000.0);
    // 分拍数是整数，不需要有限值检查。
    m_beatDivisor = std::clamp(preferences.beatDivisor, 1, 64);
    // 视图偏好不能恢复到音频零点前，实际时长稍后再进一步钳制。
    m_viewCenter = std::max(0.0,
                            std::isfinite(preferences.viewCenterSeconds)
                                ? preferences.viewCenterSeconds
                                : 0.0);
    // 半宽独立于具体音轨时长，使用交互支持的全局上下限。
    m_zoomSeconds = std::clamp(std::isfinite(preferences.viewHalfWidthSeconds)
                                   ? preferences.viewHalfWidthSeconds
                                   : 8.0,
                               0.01,
                               120.0);
}

/// @brief 将指定类别的 BPM 工具偏好同步到内存配置并启动延迟落盘。
/// @param measurementDisplayChanged 是否同步拍框宽度与分拍数。
/// @param viewChanged 是否同步视图中心与半宽。
/// @warning UI 热路径低频分支：只写入少量标量，禁止在此执行文件访问。
/// @details 两个布尔参数允许测量显示和视野交互分别更新自身字段；只要
/// 任一类别变化就覆盖保存倒计时，实现连续拖动的尾沿合并。
void BpmMeasurementToolView::markUserPreferencesChanged(
    bool measurementDisplayChanged, bool viewChanged)
{
    // 直接更新内存配置，使同进程内重新打开窗口可立即读取最新值。
    auto& preferences = Config::AppConfig::instance()
                            .getEditorSettings()
                            .bpmMeasurementToolPreferences;
    if ( measurementDisplayChanged ) {
        // 显示类参数一起提交，避免不同控件各自触发保存计时器。
        preferences.markerWidthMs = std::clamp(m_markerWidthMs, 4.0, 1000.0);
        preferences.beatDivisor   = std::clamp(m_beatDivisor, 1, 64);
    }
    if ( viewChanged ) {
        // 只持久化非负中心和受支持半宽，不记录临时拖动标志。
        preferences.viewCenterSeconds = std::max(0.0, m_viewCenter);
        preferences.viewHalfWidthSeconds =
            std::clamp(m_zoomSeconds, 0.01, 120.0);
    }
    if ( measurementDisplayChanged || viewChanged ) {
        // 每次连续输入都覆盖倒计时，实现非阻塞尾沿消抖。
        m_userPreferencesDirty = true;
        m_userPreferencesSaveDelaySeconds =
            BPM_USER_PREFERENCE_SAVE_DELAY_SECONDS;
    }
}

/// @brief 在用户停止连续操作后将 BPM 工具偏好写入配置文件。
/// @param force 是否忽略延迟并立即保存，供窗口关闭和析构流程使用。
/// @warning 低频配置路径：可能访问文件系统，不得在未修改偏好时调用。
/// @details 非强制模式逐帧递减倒计时，并等待鼠标松开且没有活动控件；
/// 强制模式用于关闭和析构。失败不会丢弃内存修改，而是保留脏位稍后重试。
void BpmMeasurementToolView::flushUserPreferences(bool force)
{
    if ( !m_userPreferencesDirty ) {
        // 无修改时绝不触发配置文件访问。
        return;
    }

    if ( !force ) {
        // 以帧增量推进非阻塞倒计时，UI 和渲染线程继续正常工作。
        m_userPreferencesSaveDelaySeconds =
            std::max(0.0,
                     m_userPreferencesSaveDelaySeconds -
                         static_cast<double>(ImGui::GetIO().DeltaTime));
        if ( m_userPreferencesSaveDelaySeconds > 0.0 ||
             ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
             ImGui::IsAnyItemActive() ) {
            // 用户仍在连续操作时延后落盘，内存值已即时生效。
            return;
        }
    }

    if ( Config::AppConfig::instance().save() ) {
        // 成功后清除脏位；关闭窗口前不会再次无条件写入。
        m_userPreferencesDirty            = false;
        m_userPreferencesSaveDelaySeconds = 0.0;
    } else {
        // 保存失败保留脏位，并用较长间隔避免每帧重试文件系统。
        m_userPreferencesSaveDelaySeconds =
            BPM_USER_PREFERENCE_SAVE_RETRY_DELAY_SECONDS;
    }
}

/// @brief 获取当前配置下的视觉偏移，单位为秒。
/// @return 音频时间转换为视觉时间时需要叠加的偏移。
/// @note 播放 transport 使用此综合偏移，不等同于各分析图专用偏移。
/// @warning UI 热路径：只读取内存配置，不执行配置加载或保存。
double BpmMeasurementToolView::playbackVisualOffset() const
{
    return Config::AppConfig::instance()
        .getVisualConfig()
        .getEffectiveVisualOffset();
}

/// @brief 获取 BPM 波形采样内容使用的专用偏移，单位为秒。
/// @return 波形采样时间转换为 BPM 画布时间时需要叠加的专用偏移。
/// @note 波形源时间数组保持不变，偏移只用于派生画布时间缓存。
/// @warning UI 热路径：只读取内存配置，不重建波形数组。
double BpmMeasurementToolView::waveformCanvasOffset() const
{
    return Config::AppConfig::instance()
        .getVisualConfig()
        .getWaveformEffectiveVisualOffset();
}

/// @brief 获取 BPM 频谱采样内容使用的专用偏移，单位为秒。
/// @return 频谱采样时间转换为 BPM 画布时间时需要叠加的专用偏移。
/// @note 频谱绘制通过反向偏移求纹理像素范围，不修改 GPU 纹理。
/// @warning UI 热路径：只读取内存配置，不访问纹理资源。
double BpmMeasurementToolView::spectrumCanvasOffset() const
{
    return Config::AppConfig::instance()
        .getVisualConfig()
        .getSpectrumEffectiveVisualOffset();
}

/// @brief 获取当前音频对应的 BPM 工具画布时间轴总长度。
/// @return 画布时间轴上可显示的最大时间。
/// @note 画布当前沿用分析时长，负值统一视为零。
/// @warning UI 热路径：只读取已发布的分析时长。
double BpmMeasurementToolView::playbackCanvasDuration() const
{
    return std::max(0.0, m_duration);
}

/// @brief 跳转到指定音频时间；仅同轨时同步活动主画布。
/// @param audioTime 目标音频时间，单位为秒。
/// @details 编辑器命令允许到达负视觉偏移对应的音频位置；硬件试听
/// 无法播放负时间，因此独立路由会进一步钳制到零。提交后视图中心与
/// 节拍器调度游标同步更新，保证下一帧视觉和听觉使用同一时间基准。
void BpmMeasurementToolView::seekPlaybackToAudioTime(double audioTime)
{
    auto&        audio = Audio::AudioManager::instance();
    const double totalTime =
        std::max(m_duration, getPlaybackTotalTime(m_playbackRoute, audio));
    // 视觉零点可能对应负音频命令时间，保留这段前导空间给编辑器路由。
    const double visualOffset = playbackVisualOffset();
    double       minTime      = -visualOffset;
    if ( minTime > totalTime ) {
        // 极端偏移超过音频时长时收敛为单点合法区间。
        minTime = totalTime;
    }

    const double commandAudioTime =
        std::clamp(audioTime, minTime, std::max(minTime, totalTime));
    if ( isPlaybackSynchronizedWithEditor() ) {
        // 主时间线只能通过逻辑命令修改，不能跨线程直写播放位置。
        Logic::EditorEngine::instance().pushCommand(
            Logic::CmdSeek{ commandAudioTime });
    } else {
        // 独立音频后端不接受负位置，单独应用硬件边界。
        const double hardwareAudioTime =
            std::clamp(commandAudioTime, 0.0, std::max(0.0, totalTime));
        audio.seekAudition(hardwareAudioTime);
    }

    // 视图中心回到视觉时间域，使 seek 后游标落在当前画布中心。
    const double canvasDuration = playbackCanvasDuration();
    m_viewCenter                = std::clamp<double>(
        commandAudioTime + visualOffset, 0.0, std::max(0.0, canvasDuration));
    // seek 破坏连续调度假设，立即以命令音频时间重建拍索引。
    resetMetronomeScheduler(commandAudioTime);
}

/// @brief 跳转到指定 BPM 工具画布时间；仅同轨时同步活动主画布。
/// @param canvasTime 目标画布时间，单位为秒。
/// @note 此入口负责坐标域转换，所有具体路由逻辑由音频时间入口统一处理。
void BpmMeasurementToolView::seekPlaybackToCanvasTime(double canvasTime)
{
    seekPlaybackToAudioTime(canvasTime - playbackVisualOffset());
}

/// @brief 切换播放状态；同轨时同步活动主画布，异轨时只控制独立试听。
/// @param shouldPlay true 表示播放，false 表示暂停。
/// @details 开始播放前重新按当前视觉时间 seek，确保配置偏移或路由变化
/// 不会让声音从旧位置继续；暂停仅停止推进，不强制归零。
void BpmMeasurementToolView::setPlaybackState(bool shouldPlay)
{
    auto& audio = Audio::AudioManager::instance();
    if ( !shouldPlay ) {
        // 暂停使前瞻计划失效，恢复时按实际位置重新调度。
        m_metronomeScheduleInitialized = false;
    }

    if ( isPlaybackSynchronizedWithEditor() ) {
        if ( shouldPlay ) {
            // 先准备节拍器并对齐位置，再投递播放命令。
            (void)ensureMetronomeSoundEffects();
            const PlaybackTimelineState playbackState =
                readPlaybackTimelineState(
                    BpmPlaybackRoute::SynchronizedWithEditor);
            const double canvasTime =
                // 快照视觉时间裁切到当前分析画布范围。
                std::clamp<double>(playbackState.visualTime,
                                   0.0,
                                   std::max(0.0, playbackCanvasDuration()));
            Logic::EditorEngine::instance().pushCommand(
                Logic::CmdSeek{ canvasTime - playbackVisualOffset() });
        }
        Logic::EditorEngine::instance().pushCommand(
            // 播放状态由编辑器命令栈的逻辑线程统一应用。
            Logic::CmdSetPlayState{ shouldPlay });
        return;
    }

    if ( shouldPlay ) {
        // 独立试听同样先对齐当前画布位置，避免加载后的起点陈旧。
        (void)ensureMetronomeSoundEffects();
        const PlaybackTimelineState playbackState =
            readPlaybackTimelineState(BpmPlaybackRoute::Audition);
        const double canvasTime =
            std::clamp<double>(playbackState.visualTime,
                               0.0,
                               std::max(0.0, playbackCanvasDuration()));
        seekPlaybackToCanvasTime(canvasTime);
        audio.playAudition();
    } else {
        // pause 保留当前位置，便于再次播放继续。
        audio.pauseAudition();
    }
}

/// @brief 请求重新分析当前选择的音频轨道。
/// @param autoMeasure 是否在分析完成后自动估算 BPM 和 offset。
/// @warning 用户触发的低频路径：会停止旧任务、检查文件并提交后台解码任务。
/// @details 请求流程遵循以下状态顺序：
/// - 清除身份脏位并等待上一任务退出；
/// - 保留 GPU 纹理到安全重载点，同时清空 CPU 分析数据；
/// - 验证资源路径并创建仅供分析使用的 AudioTrack；
/// - 在 UI 线程预计算时长和初始视野；
/// - 捕获不可变任务参数后提交到应用线程池；
/// - 后台线程通过待消费槽位发布一个完整结果。
void BpmMeasurementToolView::requestAnalyzeSelectedTrack(bool autoMeasure)
{
    // 当前请求已经承接身份变化，无论成败都不在下一帧自动重复启动。
    m_selectedAudioIdentityNeedsAnalysis = false;
    // 单槽结果模型不允许两个分析任务并发写入，先停止上一任务。
    stopAnalysisWorker();
    // 新请求不能继续展示旧音轨的波形和频谱元数据。
    clearAnalysisData();

    auto path = selectedAudioAbsolutePath();
    if ( !path ) {
        // 项目或选择在请求前失效时回到选择提示。
        m_statusText = TR("ui.tools.bpm_measure.select_track").data();
        return;
    }

    std::error_code ec;
    if ( !std::filesystem::exists(*path, ec) || ec ) {
        // 避免异常文件 API；访问错误和不存在均不启动后台任务。
        m_statusText = TR("ui.tools.bpm_measure.file_missing").data();
        return;
    }

    // 音频管理器接口使用 UTF-8 字符串，路径转换集中在边界处。
    const std::string pathString = Config::pathToUtf8(*path);
    auto              track =
        Audio::AudioManager::instance().loadTrackForAnalysis(pathString);
    if ( !track ) {
        // 解码器创建失败时尚未启动后台线程，可直接更新 UI 状态。
        m_statusText = TR("ui.tools.bpm_measure.load_failed").data();
        return;
    }

    // 分析轨采用引擎内部格式，帧数可据固定采样率换算时长。
    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    m_duration = sampleRate > 0.0
                     ? static_cast<double>(track->num_frames()) / sampleRate
                     : 0.0;
    // 新时长可能缩短合法首拍和视图范围，启动前统一裁切。
    m_firstBeatTime = clampFirstBeatTime(
        m_firstBeatTime, m_beatLengthSeconds, playbackCanvasDuration());
    m_viewCenter =
        std::clamp<double>(autoMeasure ? m_firstBeatTime : m_viewCenter,
                           0.0,
                           std::max(0.0, playbackCanvasDuration()));
    // 自动测量包含额外 BPM 检测阶段，使用独立进度提示。
    m_statusText = autoMeasure
                       ? TR("ui.tools.bpm_measure.auto_analyzing").data()
                       : TR("ui.tools.bpm_measure.analyzing").data();
    // 进度仅作显示；完成标志负责结果内存可见性的同步。
    m_analysisProgress.store(0.0f, std::memory_order_relaxed);
    m_analysisFinished.store(false, std::memory_order_release);
    m_analysisRunning.store(true, std::memory_order_relaxed);

    // 任务启动时冻结频谱细节配置，本次分析过程中不响应配置变化。
    const auto spectrumProfile = Config::spectrumDetailProfile(
        Config::AppConfig::instance().getVisualConfig().spectrumDetailLevel);

    auto* appThreadPool = MMM::Runtime::AppThreadPool::instance().get();
    if ( !appThreadPool ) {
        // 应用线程池是生命周期前置条件，缺失时回滚运行标志并记录诊断。
        m_analysisRunning.store(false, std::memory_order_relaxed);
        m_statusText = TR("ui.tools.bpm_measure.load_failed").data();
        XERROR("AppThreadPool is not initialized before BPM analysis.");
        return;
    }

    // 每个任务使用新的停止源，旧任务的取消状态不能泄漏到新任务。
    m_analysisStopSource            = std::stop_source{};
    const std::stop_token stopToken = m_analysisStopSource.get_token();
    // track 的共享所有权移动到任务，保证整个解码期间资源存活。
    m_analysisFuture = appThreadPool->enqueue([this,
                                               stopToken,
                                               track    = std::move(track),
                                               duration = m_duration,
                                               autoMeasure,
                                               spectrumProfile]() {
        analyzeTrack(stopToken, track, duration, autoMeasure, spectrumProfile);
    });
}

/// @brief 从后台线程发布一次分析失败结果。
/// @param autoMeasure 本次任务是否属于自动 BPM/offset 测量。
/// @warning 后台线程路径：只写入受互斥锁保护的待消费结果和原子状态。
/// @details 结果对象先在互斥区写入，随后以 release 发布完成标志；
/// UI 线程使用 acquire 读取完成标志后才能移动该结果。
void BpmMeasurementToolView::publishAnalysisFailure(bool autoMeasure)
{
    // 失败结果仍记录任务种类，使 UI 选择对应的本地化提示。
    AnalysisResult result;
    result.autoTimingRequested = autoMeasure;
    result.failed              = true;

    {
        // 单槽覆盖是安全的，因为请求入口保证上一任务已经退出。
        std::lock_guard<std::mutex> lock(m_pendingResultMutex);
        m_pendingResult = std::move(result);
    }
    // 先结束进度与运行状态，最后发布 finished 作为同步点。
    m_analysisProgress.store(1.0f, std::memory_order_relaxed);
    m_analysisRunning.store(false, std::memory_order_relaxed);
    m_analysisFinished.store(true, std::memory_order_release);
}

/// @brief 请求自动测量当前选择的音频轨道。
/// @note 没有显式选择时自动采用活动谱面的默认主音轨。
/// @details 该入口只负责补全选择并转发到统一分析请求，避免按钮路径和
/// 外部自动测量路径维护两套解码或结果处理逻辑。
void BpmMeasurementToolView::requestAutoMeasureSelectedTrack()
{
    if ( m_selectedAudioTrackId.empty() ) {
        // 默认选择逻辑与外部自动测量入口保持一致。
        const std::string targetAudioTrackId = defaultAudioTrackId();
        if ( targetAudioTrackId.empty() ) {
            // 项目确实没有音频资源时给出明确状态并终止。
            m_statusText = TR("ui.tools.bpm_measure.no_audio").data();
            return;
        }
        // 通过统一选择入口刷新同步键和播放路由。
        setSelectedAudioTrackId(targetAudioTrackId);
    }

    requestAnalyzeSelectedTrack(true);
}

/// @brief 查找当前项目默认用于 BPM 测量的音频资源 ID。
/// @return 优先返回活动谱面的歌曲提示或 Main 自动采样，否则回退项目资源。
/// @details 选择优先级与谱面播放资源解析保持一致：
/// 活动谱面的明确映射最高，其次是项目 Main 类型，最后才使用首个资源。
std::string BpmMeasurementToolView::defaultAudioTrackId() const
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    if ( !project || project->m_audioResources.empty() ) {
        // 没有当前项目或资源列表为空时不存在合法默认值。
        return {};
    }

    const auto activeSession = engine.getActiveSession();
    if ( activeSession ) {
        // 活动会话的谱面映射比项目级 Main 标记更具体。
        const auto& context = activeSession->getContext();
        if ( context.currentBeatmap ) {
            const auto& beatmap = *context.currentBeatmap;
            const auto& mapPath = beatmap.m_baseMapMetadata.map_path;
            if ( const auto* resource = Logic::ProjectResourceService::
                     findDefaultBeatmapAudioResource(
                         *project, beatmap, mapPath) ) {
                // 服务同时处理歌曲提示与 Main 自动采样规则。
                return resource->m_id;
            }
        }
    }

    for ( const auto& resource : project->m_audioResources ) {
        if ( resource.m_type == AudioTrackType::Main ) {
            // 没有活动谱面映射时采用项目首个主音轨。
            return resource.m_id;
        }
    }

    // 非空资源列表的最终兜底保持项目原始顺序。
    return project->m_audioResources.front().m_id;
}

/// @brief 停止并等待当前后台分析任务。
/// @warning
/// 不可中断低频路径：会等待后台线程退出，只能在关闭窗口或重新选择音轨时执行。
/// @details 停止令牌由波形、频谱和完整单声道读取循环定期检查；wait 保证
/// future 任务不再访问成员后才允许清理缓存或析构本对象。
void BpmMeasurementToolView::stopAnalysisWorker()
{
    if ( m_analysisFuture.valid() ) {
        // 先发协作式停止请求，让解码循环在下一个检查点尽快退出。
        m_analysisStopSource.request_stop();
        // 成员结果槽与 this 被任务引用，销毁或重用前必须等待完成。
        m_analysisFuture.wait();
        // 丢弃已完成 future，后续请求可以明确判断是否存在任务。
        m_analysisFuture = std::future<void>{};
    }
    // 取消路径可能已清除此位，重复写 false 保持幂等。
    m_analysisRunning.store(false, std::memory_order_relaxed);
}

/// @brief 清理当前分析缓存并延迟释放频谱 GPU 资源。
/// @warning 低频资源路径：由用户切换音轨或重新分析触发；GPU 纹理只打脏位，
/// 实际释放必须延迟到下一次资源准备阶段，避免当前 ImGui draw list 仍引用旧
/// descriptor set。
/// @details CPU 波形、派生时间和待上传像素可立即清空；已有 VKTexture
/// 仍可能被本帧命令引用，因此只设置重载脏位。分析进度和完成标志同时复位，
/// 但当前状态文本由新请求路径决定。
void BpmMeasurementToolView::clearAnalysisData()
{
    // 波形源坐标及派生画布坐标必须作为一组清空。
    m_waveTimes.clear();
    m_waveCanvasTimes.clear();
    // NaN 强制下一批波形首次绘制时重建偏移缓存。
    m_waveCanvasTimesOffset = std::numeric_limits<double>::quiet_NaN();
    m_waveMin.clear();
    m_waveMax.clear();
    // 尚未上传的 CPU 分块属于旧任务，可立即释放。
    m_pendingSpectrumChunks.clear();
    m_nextSpectrumChunkUploadIndex = 0;
    m_spectrumTextureReloadStarted = false;
    // 已上传纹理由资源回调在 GPU 安全点释放，不能在当前 draw list 后直接销毁。
    m_texturesNeedReload   = !m_spectrumTextures.empty();
    m_spectrumSegmentCount = 0;
    m_spectrumBinCount     = 0;
    // 原子状态复位后 UI 显示等待新任务的普通状态。
    m_analysisProgress = 0.0f;
    m_analysisFinished = false;
}

/// @brief 后台分析线程执行体。
/// @param stopToken 线程停止令牌。
/// @param track 待分析音频轨道，后台线程持有共享所有权。
/// @param duration 音频时长，单位为秒。
/// @param autoMeasure 是否在频谱分析后继续执行自动 BPM/offset 测量。
/// @warning 后台耗时路径：执行完整音频解码和 FFT；不在 UI/渲染热路径中运行。
/// @details 分析分为四个阶段：
/// - 按固定显示密度解码波形最小值与最大值包络；
/// - 使用 Hann 窗和实数 FFT 计算时频能量；
/// - 可选读取单声道全轨并运行自动 BPM 检测；
/// - 将热力图转换为受 GPU 最大宽度约束的 RGBA 分块。
/// 任一失败由守卫发布统一失败结果；取消只清理运行状态，不覆盖新请求状态。
void BpmMeasurementToolView::analyzeTrack(
    std::stop_token stopToken, std::shared_ptr<ice::AudioTrack> track,
    double duration, bool autoMeasure,
    Config::SpectrumDetailProfile spectrumProfile)
{
    if ( !track ) {
        // 防御异步捕获为空，使用统一失败发布路径。
        publishAnalysisFailure(autoMeasure);
        return;
    }

    const double sampleRate =
        static_cast<double>(ice::ICEConfig::internal_format.samplerate);
    const size_t totalFrames = track->num_frames();
    if ( duration <= 0.0 || sampleRate <= 0.0 || totalFrames == 0 ) {
        // 无有效时间域时波形和频谱都无法建立。
        publishAnalysisFailure(autoMeasure);
        return;
    }

    /// @brief 后台分析失败兜底，避免早退后 UI 长期停在分析状态。
    /// @details 守卫区分失败与协作取消：失败需要通知 UI，取消只结束运行位。
    struct AnalysisFailureGuard {
        /// @brief 当前 BPM 工具实例。
        BpmMeasurementToolView& view;

        /// @brief 是否属于自动 BPM/offset 测量任务。
        bool autoMeasure{ false };

        /// @brief 仍需要在析构时发布失败。
        bool active{ true };

        /// @brief 任务成功完成，不再发布失败。
        /// @post active 变为 false，析构不触碰结果槽。
        void dismiss() { active = false; }

        /// @brief 任务被取消，只清理运行状态。
        /// @note 取消是正常控制流，不显示分析失败提示。
        void cancel()
        {
            active = false;
            view.m_analysisRunning.store(false, std::memory_order_relaxed);
        }

        /// @brief 兜底发布失败结果。
        /// @warning 仅在后台线程退出路径调用。
        ~AnalysisFailureGuard()
        {
            if ( active ) {
                view.publishAnalysisFailure(autoMeasure);
            }
        }
    } failureGuard{ *this, autoMeasure };

    // 波形点数按时长线性增长，并至少保留首尾两个点。
    const int wavePointCount =
        std::max(2, static_cast<int>(duration * m_wavePointsPerSecond) + 1);
    const double spectrumSegmentsPerSecond = spectrumProfile.segmentsPerSecond;
    // 频谱横向段数由细节配置决定，至少生成一列。
    const int spectrumSegmentCount =
        std::max(1, static_cast<int>(duration * spectrumSegmentsPerSecond) + 1);
    const int spectrumBinCount = spectrumProfile.frequencyBins;
    // 固定 FFT 大小平衡低频分辨率和后台分析成本。
    const int fftSize = 2048;
    // hopSize 将目标每秒段数换算为解码帧步进。
    const size_t hopSize = std::max<size_t>(
        1, static_cast<size_t>(sampleRate / spectrumSegmentsPerSecond));
    const uint16_t channelCount = ice::ICEConfig::internal_format.channels;
    // 进度按波形点、频谱列及可选自动检测三个工作单元累计。
    const int totalWork =
        wavePointCount + spectrumSegmentCount + (autoMeasure ? 1 : 0);
    int finishedWork = 0;

    // 所有结果先构造在线程局部对象，完成后一次性发布。
    AnalysisResult result;
    result.duration                  = duration;
    result.spectrumSegmentsPerSecond = spectrumSegmentsPerSecond;
    result.spectrumSegmentCount      = spectrumSegmentCount;
    result.spectrumBinCount          = spectrumBinCount;
    result.autoTimingRequested       = autoMeasure;
    result.waveTimes.resize(wavePointCount);
    result.waveMin.assign(wavePointCount, 0.0);
    result.waveMax.assign(wavePointCount, 0.0);

    auto updateProgress = [&]() {
        if ( totalWork <= 0 ) {
            // 防御配置异常，避免进度除零。
            return;
        }
        // relaxed 足够，因为 UI 不依赖进度同步其他数据。
        m_analysisProgress.store(std::clamp(static_cast<float>(finishedWork) /
                                                static_cast<float>(totalWork),
                                            0.0f,
                                            1.0f),
                                 std::memory_order_relaxed);
    };

    // 复用一个解码缓冲，逐显示区间读取而不常驻完整 PCM。
    ice::AudioBuffer waveBuffer;
    for ( int point = 0; point < wavePointCount; ++point ) {
        if ( stopToken.stop_requested() ) {
            // 每个波形点检查取消，限制重新选择音轨的等待时间。
            failureGuard.cancel();
            return;
        }

        // 每个包络点代表固定时间桶，与屏幕缩放无关。
        const double timeStart =
            static_cast<double>(point) / m_wavePointsPerSecond;
        const double timeEnd =
            static_cast<double>(point + 1) / m_wavePointsPerSecond;
        const size_t startFrame =
            std::min(totalFrames, static_cast<size_t>(timeStart * sampleRate));
        const size_t endFrame =
            std::min(totalFrames, static_cast<size_t>(timeEnd * sampleRate));
        // 末尾桶可短于常规桶，空桶保留零包络。
        const size_t frameCount =
            endFrame > startFrame ? endFrame - startFrame : 0;

        result.waveTimes[point] = timeStart;
        if ( frameCount > 0 ) {
            // 缓冲按当前桶调整，解码器可返回少于请求的帧数。
            waveBuffer.resize(ice::ICEConfig::internal_format, frameCount);
            waveBuffer.clear();
            const size_t decoded =
                track->read(waveBuffer, startFrame, frameCount);
            // 只遍历实际解码且不超过缓冲请求的帧。
            const size_t usableFrames = std::min(decoded, frameCount);
            double       minValue     = 0.0;
            double       maxValue     = 0.0;
            float**      data         = waveBuffer.raw_ptrs();

            for ( size_t frame = 0; frame < usableFrames; ++frame ) {
                // 多声道求算术平均生成用于可视化的单声道幅值。
                double mixed = 0.0;
                for ( uint16_t ch = 0; ch < channelCount; ++ch ) {
                    mixed += data[ch][frame];
                }
                mixed /= std::max<uint16_t>(1, channelCount);
                // 初值为零确保静音轴总被包络包含。
                minValue = std::min(minValue, mixed);
                maxValue = std::max(maxValue, mixed);
            }
            result.waveMin[point] = minValue;
            result.waveMax[point] = maxValue;
        }

        // 每完成一个时间桶就发布一次近似进度。
        ++finishedWork;
        updateProgress();
    }

    // 热力图按 [频率 bin][时间段] 行主序存储，初值代表静音下限。
    std::vector<float> heatmap(
        static_cast<size_t>(spectrumBinCount) * spectrumSegmentCount, -100.0f);
    // Hann 窗降低有限 FFT 帧边缘不连续导致的频谱泄漏。
    std::vector<double> window(fftSize);
    for ( int i = 0; i < fftSize; ++i ) {
        window[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (fftSize - 1)));
    }

    // FFTW 要求使用其分配器保证 SIMD 所需对齐。
    double* fftInput =
        static_cast<double*>(fftw_malloc(sizeof(double) * fftSize));
    fftw_complex* fftOutput = static_cast<fftw_complex*>(
        fftw_malloc(sizeof(fftw_complex) * (fftSize / 2 + 1)));
    if ( !fftInput || !fftOutput ) {
        // 两次分配可能只成功一次，分别释放已获得的缓冲。
        if ( fftInput ) {
            fftw_free(fftInput);
        }
        if ( fftOutput ) {
            fftw_free(fftOutput);
        }
        return;
    }

    // FFTW 计划创建与销毁使用进程级互斥，规避库的计划器并发限制。
    fftw_plan fftPlan = nullptr;
    {
        std::lock_guard<std::mutex> lock(fftwPlanMutex());
        fftPlan =
            // ESTIMATE 避免后台任务为测量最优计划额外阻塞。
            fftw_plan_dft_r2c_1d(fftSize, fftInput, fftOutput, FFTW_ESTIMATE);
    }

    if ( !fftPlan ) {
        // 计划失败时守卫负责发布失败，手工释放原始 FFTW 缓冲。
        fftw_free(fftInput);
        fftw_free(fftOutput);
        return;
    }

    // 频谱缓冲固定为一个 FFT 窗口，并在各时间段间复用。
    ice::AudioBuffer spectrumBuffer;
    spectrumBuffer.resize(ice::ICEConfig::internal_format, fftSize);

    // 忽略低于 20 Hz 的次声区域，上限由工具配置控制。
    const double fmin      = 20.0;
    const double fmax      = m_maxFrequency;
    const double freqRange = std::max(1.0, fmax - fmin);
    // 指数偏置为低频分配更多垂直像素，便于观察节奏基频。
    const double logBias      = m_logFrequencyBias;
    const double expKMinus1   = std::exp(logBias) - 1.0;
    auto         binFrequency = [&](double progress) {
        if ( std::abs(logBias) < 1e-4 ) {
            // 接近零偏置时改用线性公式，避免 expKMinus1 数值不稳定。
            return fmin + freqRange * progress;
        }
        return fmin +
               freqRange * (std::exp(logBias * progress) - 1.0) / expKMinus1;
    };

    for ( int segment = 0; segment < spectrumSegmentCount; ++segment ) {
        if ( stopToken.stop_requested() ) {
            {
                // 计划销毁与创建使用同一互斥约束。
                std::lock_guard<std::mutex> lock(fftwPlanMutex());
                fftw_destroy_plan(fftPlan);
            }
            fftw_free(fftInput);
            fftw_free(fftOutput);
            // 协作取消不发布失败提示，调用方正在启动新任务或销毁窗口。
            failureGuard.cancel();
            return;
        }

        // 时间段索引乘 hopSize 得到窗口起点，末尾安全钳制到总帧数。
        const size_t startFrame =
            std::min(totalFrames, static_cast<size_t>(segment) * hopSize);
        // 音频尾部窗口不足 fftSize 时只解码剩余帧，其余位置补零。
        const size_t frameCount =
            startFrame < totalFrames
                ? std::min<size_t>(fftSize, totalFrames - startFrame)
                : 0;

        // clear 先把整个窗口置零，为短尾窗提供显式零填充。
        spectrumBuffer.clear();
        const size_t decoded =
            frameCount > 0 ? track->read(spectrumBuffer, startFrame, frameCount)
                           : 0;
        float** data = spectrumBuffer.raw_ptrs();
        for ( int i = 0; i < fftSize; ++i ) {
            // 每个采样位置先混合声道，再乘预计算窗函数。
            double mixed = 0.0;
            if ( static_cast<size_t>(i) < decoded ) {
                for ( uint16_t ch = 0; ch < channelCount; ++ch ) {
                    mixed += data[ch][i];
                }
                // 声道平均避免声道数改变频谱整体幅度标尺。
                mixed /= std::max<uint16_t>(1, channelCount);
            }
            fftInput[i] = mixed * window[i];
        }

        // 计划绑定已分配输入输出缓冲，可重复执行无需重新规划。
        fftw_execute(fftPlan);

        for ( int bin = 0; bin < spectrumBinCount; ++bin ) {
            // 每个显示 bin 对应经偏置映射的一段连续频率区间。
            const double freqStart =
                binFrequency(static_cast<double>(bin) / spectrumBinCount);
            const double freqEnd =
                binFrequency(static_cast<double>(bin + 1) / spectrumBinCount);
            // 频率换算为 FFT 索引并限制在实数变换的 Nyquist 半谱。
            const int binStart =
                std::clamp(static_cast<int>(freqStart * fftSize / sampleRate),
                           0,
                           fftSize / 2);
            const int binEnd =
                std::clamp(static_cast<int>(freqEnd * fftSize / sampleRate),
                           binStart,
                           fftSize / 2);

            // 显示 bin 取区间峰值，保留窄带瞬态而非被平均稀释。
            double maxMagnitude = 0.0;
            for ( int i = binStart; i <= binEnd; ++i ) {
                // 使用模平方比较避免循环内重复开方。
                const double magSq = fftOutput[i][0] * fftOutput[i][0] +
                                     fftOutput[i][1] * fftOutput[i][1];
                maxMagnitude       = std::max(maxMagnitude, magSq);
            }
            // 仅峰值确定后开方并归一化到 FFT 大小，再转换为 dB。
            const double db =
                maxMagnitude > 1e-9
                    ? 20.0 * std::log10(std::sqrt(maxMagnitude) / fftSize)
                    : -100.0;
            // 截断到显示色标支持的能量范围，抑制异常幅值。
            heatmap[static_cast<size_t>(bin) * spectrumSegmentCount + segment] =
                static_cast<float>(std::clamp(db, -100.0, 0.0));
        }

        // 每个频谱时间列完成后更新近似进度。
        ++finishedWork;
        updateProgress();
    }

    {
        // 正常路径同样在计划器互斥下销毁 FFTW plan。
        std::lock_guard<std::mutex> lock(fftwPlanMutex());
        fftw_destroy_plan(fftPlan);
    }
    // plan 销毁后输入输出缓冲不再被 FFTW 引用。
    fftw_free(fftInput);
    fftw_free(fftOutput);

    if ( autoMeasure ) {
        if ( duration >= 10.0 ) {
            // 自动检测需要足够节奏样本，短于十秒时保留空结果。
            if ( auto monoSamples =
                     readMonoSamplesForAutoTiming(stopToken, track) ) {
                if ( !stopToken.stop_requested() ) {
                    // 检测器接收引擎采样率与完整单声道 PCM。
                    result.autoTimingResult = BpmAutoDetector::detect(
                        *monoSamples,
                        ice::ICEConfig::internal_format.samplerate);
                }
            }
        }

        // 自动检测无论产生结果与否都算作一个完成工作单元。
        ++finishedWork;
        updateProgress();
    }

    // 横向段按 Vulkan 纹理宽度上限切块，最后一块允许较窄。
    const int chunkCount =
        (spectrumSegmentCount + static_cast<int>(MAX_TEXTURE_W) - 1) /
        static_cast<int>(MAX_TEXTURE_W);
    // 每个块最终对应一个 VKTexture，提前预留结果容器。
    result.spectrumChunks.reserve(static_cast<size_t>(chunkCount));

    for ( int chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex ) {
        // chunkStart 是全局时间列索引，而不是音频帧索引。
        const uint32_t chunkStart =
            static_cast<uint32_t>(chunkIndex) * MAX_TEXTURE_W;
        const uint32_t chunkWidth = std::min<uint32_t>(
            MAX_TEXTURE_W,
            static_cast<uint32_t>(spectrumSegmentCount) - chunkStart);

        // CPU RGBA 数据在线程中生成，GPU 上传留给渲染资源回调。
        TextureChunkData chunk;
        chunk.width  = chunkWidth;
        chunk.height = static_cast<uint32_t>(spectrumBinCount);
        chunk.pixels.resize(static_cast<size_t>(chunk.width) * chunk.height *
                            4);

        for ( uint32_t y = 0; y < chunk.height; ++y ) {
            // 热力图低 bin 在下，纹理坐标原点在上，因此翻转垂直索引。
            const int bin = spectrumBinCount - 1 - static_cast<int>(y);
            for ( uint32_t x = 0; x < chunk.width; ++x ) {
                const uint32_t globalX = chunkStart + x;
                // 从全局热力图读取该块对应的时间列。
                const float value =
                    heatmap[static_cast<size_t>(bin) * spectrumSegmentCount +
                            globalX];
                // dB 色标转换为固定四通道像素。
                const auto   color = spectrumColorFromDb(value);
                const size_t offset =
                    (static_cast<size_t>(y) * chunk.width + x) * 4;
                // 颜色数组布局与目标 RGBA 缓冲一致，可直接复制四字节。
                std::memcpy(&chunk.pixels[offset], color.data(), 4);
            }
        }

        // 移动大像素容器，避免后台结果构建阶段复制。
        result.spectrumChunks.push_back(std::move(chunk));
    }

    // 所有 CPU 结果完整后先更新进度，再进入结果发布临界区。
    m_analysisProgress.store(1.0f, std::memory_order_relaxed);
    {
        // 待消费槽一次接收整批波形、频谱与自动测量结果。
        std::lock_guard<std::mutex> lock(m_pendingResultMutex);
        m_pendingResult = std::move(result);
    }
    // 槽位写入成功后解除失败守卫，最后用 release 发布完成标志。
    failureGuard.dismiss();
    m_analysisRunning.store(false, std::memory_order_relaxed);
    m_analysisFinished.store(true, std::memory_order_release);
}

/// @brief 读取完整音轨并混合为单声道采样，供自动 BPM 检测使用。
/// @param stopToken 后台线程停止令牌。
/// @param track 待读取的音频轨道。
/// @return 成功时返回单声道采样，否则返回空。
/// @warning 后台耗时路径：会读取完整音频，只能由手动触发的分析任务调用。
/// @details 采用固定块读取以限制临时 AudioBuffer 大小；最终单声道数组
/// 仍按实际解码帧数保留完整轨道，解码提前结束时收缩到有效前缀。
std::optional<std::vector<float>>
BpmMeasurementToolView::readMonoSamplesForAutoTiming(
    std::stop_token                         stopToken,
    const std::shared_ptr<ice::AudioTrack>& track) const
{
    if ( !track ) {
        // 空轨道不能读取，返回空交由自动测量流程判定失败。
        return std::nullopt;
    }

    const size_t totalFrames = track->num_frames();
    if ( totalFrames == 0 ) {
        // 零帧轨道没有可用于节奏检测的样本。
        return std::nullopt;
    }

    const uint16_t channelCount = ice::ICEConfig::internal_format.channels;
    if ( channelCount == 0 ) {
        // 引擎内部格式必须至少一个声道，防御配置损坏和除零。
        return std::nullopt;
    }

    // 块大小平衡解码调用开销和临时内存占用，不影响最终样本顺序。
    constexpr size_t   AUTO_TIMING_READ_CHUNK_FRAMES = 32768;
    std::vector<float> monoSamples(totalFrames, 0.0f);
    ice::AudioBuffer   buffer;

    size_t readOffset = 0;
    while ( readOffset < totalFrames ) {
        if ( stopToken.stop_requested() ) {
            // 每块检查一次取消，避免重新分析长期等待全轨读取完成。
            return std::nullopt;
        }

        // 最后一块只请求剩余帧数。
        const size_t frameCount =
            std::min(AUTO_TIMING_READ_CHUNK_FRAMES, totalFrames - readOffset);
        // 清零保证解码器短读不会留下上一块的残余样本。
        buffer.resize(ice::ICEConfig::internal_format, frameCount);
        buffer.clear();

        const size_t decoded = track->read(buffer, readOffset, frameCount);
        if ( decoded == 0 ) {
            // 解码器以零帧表示无法继续，保留此前已读取前缀。
            break;
        }

        float** data = buffer.raw_ptrs();
        for ( size_t frame = 0; frame < decoded; ++frame ) {
            // 对每帧求声道平均，保持样本数量与原轨帧数一致。
            double mixed = 0.0;
            for ( uint16_t ch = 0; ch < channelCount; ++ch ) {
                mixed += data[ch][frame];
            }
            monoSamples[readOffset + frame] =
                static_cast<float>(mixed / channelCount);
        }

        // 以实际解码量推进，支持短读后继续请求下一段。
        readOffset += decoded;
    }

    if ( readOffset == 0 ) {
        // 完全无法解码时不向检测器传递全零占位数组。
        return std::nullopt;
    }
    if ( readOffset < monoSamples.size() ) {
        // 提前结束时移除未写入的尾部零值，避免扭曲节奏统计。
        monoSamples.resize(readOffset);
    }
    return monoSamples;
}

/// @brief 查找当前选中音频轨道的绝对路径。
/// @return 成功时返回绝对路径，否则返回空。
/// @note 只组合项目根与资源相对路径，不检查文件存在性。
/// @warning 只遍历当前项目资源；存在性和权限检查由调用方低频路径负责。
std::optional<std::filesystem::path>
BpmMeasurementToolView::selectedAudioAbsolutePath() const
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || m_selectedAudioTrackId.empty() ) {
        // 路径解析必须绑定当前项目，旧选择不可跨项目使用。
        return std::nullopt;
    }

    for ( const auto& resource : project->m_audioResources ) {
        if ( resource.m_id == m_selectedAudioTrackId ) {
            // 资源路径按项目格式存为 UTF-8，相加前转换为平台路径。
            return project->m_projectRoot / Config::utf8ToPath(resource.m_path);
        }
    }

    return std::nullopt;
}

/// @brief 将 dB 值映射为热力图 RGBA 颜色。
/// @param db 频谱能量，单位为 dB。
/// @return RGBA 颜色。
/// @details 显示范围固定为 -80 dB 到 -8 dB，归一化后依次提升
/// 红、绿、蓝通道，形成从黑到红、黄、白的分段线性色标。
std::array<unsigned char, 4> BpmMeasurementToolView::spectrumColorFromDb(
    double db) const
{
    // 低于下限统一为黑色，高于上限统一为满亮白色。
    const double scaleMin = -80.0;
    const double scaleMax = -8.0;
    const double t =
        std::clamp((db - scaleMin) / (scaleMax - scaleMin), 0.0, 1.0);
    // 三个通道错开三分之一范围开启，构成连续热力渐变。
    const double r = std::clamp(3.0 * t, 0.0, 1.0);
    const double g = std::clamp(3.0 * t - 1.0, 0.0, 1.0);
    const double b = std::clamp(3.0 * t - 2.0, 0.0, 1.0);

    // GPU 纹理使用不透明八位 RGBA，与 VKTexture 输入格式一致。
    return { static_cast<unsigned char>(r * 255.0),
             static_cast<unsigned char>(g * 255.0),
             static_cast<unsigned char>(b * 255.0),
             255 };
}

}  // namespace MMM::UI
