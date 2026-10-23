#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

/**
 * @file MalodyEdgeCaseTest.cpp
 * @brief 验证 Malody MC 与统一谱面模型之间的边界转换契约。
 *
 * Malody 的 Key 与 Slide 模式虽然共用 meta、time、effect 和 note 数组，
 * 但物件字段、坐标系统、长条表示和 SOUND 形态并不相同。本文件通过
 * 程序化夹具覆盖普通资源谱面难以稳定包含的异常组合，并直接检查导出 JSON
 * 与重新加载后的统一模型，防止宽容读取掩盖错误保存结果。
 *
 * 测试按六组职责组织：
 *
 * 1. 折线清理与模式降级
 *    - Slide 折线中的零长度 Hold 不应生成无效 seg；
 *    - 零 Hold 后的 Flick 需要保留方向并提升为普通 Flick；
 *    - 连续同向 Flick 在零长度节点清理后合并位移；
 *    - 所有子节点被清理时，折线根退化为普通 Note；
 *    - Key 不支持折线结构时，只输出可表示的 Hold 或 Note；
 *    - 折线子节点绑定采样无法放入 seg，必须拒绝而非静默丢失。
 *
 * 2. Key 与 Slide 字段协议
 *    - Key Note 使用 column，Hold 额外使用绝对 endbeat；
 *    - Slide Note 使用 x 与 w，折线使用相对 seg；
 *    - Slide Flick 使用 dir 表达跨轨方向；
 *    - Key Flick 没有等价手势语义，因此降级为单个 Note；
 *    - 7K 与 8K 的 x、w 必须匹配 Rhythm Master 皮肤分轨宽度；
 *    - 不支持的 mode 必须在创建目标文件之前被保存器拒绝。
 *
 * 3. 自动采样与玩家命中采样
 *    - Key 自动采样以数值 type=1 表示，轨道写在 x；
 *    - Slide 自动采样以字符串 SOUND 表示，且不输出可玩坐标 x；
 *    - 玩家物件的 sound/vol 是命中绑定，不属于自动采样集合；
 *    - 多个同拍 SOUND 不能按时间戳去重；
 *    - 每个 SOUND 的 beat、offset、x 和 vol 必须独立往返；
 *    - SOUND 的绝对轨道不能扩大玩家 key 数量；
 *    - song.file 只是资源提示，不会自动获得播放调度权限。
 *
 * 4. 历史采样轨道兼容
 *    - 缺少 x 的旧 SOUND 按 Malody Pro Editor 规则从轨道 10 开始布局；
 *    - 同一触发时间的多个旧采样分配到连续 BGM 轨；
 *    - 新触发时间可以重新使用旧版起始轨道；
 *    - 显式合法 x 必须保持不变；
 *    - 落入玩家轨区域的非法 x 搬到第一条 BGM 轨；
 *    - 修复性搬移保留 original_x 并生成非致命结构化诊断。
 *
 * 5. timing delay、主音频相位与拍号换算
 *    - 普通 timing.delay 只移动自身锚点以及锚点后的内容；
 *    - 自动采样 offset 只改变 effectiveTimestamp，不移动 timing 或 Note；
 *    - 首 timing 与时间零主音频可能构成 Malody 的配对相位表示；
 *    - 配对 delay 按首 BPM 的拍长回卷为非负相位；
 *    - 正相位可能要求 note/effect 的导出拍号整体前移一拍；
 *    - 晚于首拍的第一条红线需要增加合成锚点并保留原红线；
 *    - 多 BPM 时间线只能在第一 timing 上保存配对 delay；
 *    - 用户移动 timing、Note、effect 或 SOUND 后，内部时间戳优先于导入 beat。
 *
 * 6. 缺字段、精度和内部元数据
 *    - 字符串 BPM 与数值 BPM 都能读取；
 *    - 只有 meta 的 MC 获得默认 120 BPM 和四条玩家轨；
 *    - 空难度名导出为可显示的 default；
 *    - initialDelay、audioOffset 与 original_structure 只服务内部兼容，
 *      不得泄漏到公开 MC JSON；
 *    - 时间线绝对拍号和 seg 相对拍号使用同一固定分母候选；
 *    - 1919/1920 与 287/288 等高精度分拍不能被粗糙约分候选破坏。
 *
 * @par 两层测试判定
 *
 * 对保存器行为，测试先读取输出 JSON，检查字段形态、拍号和是否存在废弃键。
 * 对转换语义，再从该文件加载 BeatMap，检查绝对时间、轨道、音量和对象分类。
 * 只验证其中一层会留下盲区：读取器可能容忍保存器的旧字段，而 JSON 结构
 * 正确也不代表统一模型能恢复原始绝对时间。
 *
 * @par 时间表示约定
 *
 * Malody beat 使用三元数组 [整数拍, 分子, 分母]。统一模型使用毫秒时间戳，
 * BPM timing 提供两者之间的分段映射。delay 是 timing 锚点相对拍轴的相位，
 * SOUND offset 是单个音频相对自身 beat 的偏移，二者不能合并为全局平移。
 * 当首 timing 与主 SOUND 构成历史配对形态时，加载器会规范化内部模型；
 * 保存器随后恢复等价的非负 delay，并按规范化相位决定主 SOUND 偏移。
 *
 * @par 玩家对象与自动采样边界
 *
 * note[] 中同时容纳游戏物件和自动采样。isSoundNode 只根据 type 判定后者，
 * 不能因为节点含 sound 就归入自动采样，因为普通 Note 也允许绑定命中音效。
 * 自动采样进入 m_audioSamples，游戏物件进入 m_allNotes；两个集合的数量、
 * 轨道推断和保存字段均分别断言，以防资源名相同导致错误消费。
 *
 * @par Key 与 Slide 坐标约定
 *
 * Key 的玩家轨由 mode_ext.column 定义，column 是玩家轨内的离散索引。
 * 自动采样的 x 是统一绝对轨道：首条 BGM 轨紧随玩家轨。Slide 的 x/w
 * 则是皮肤坐标与宽度，不能直接按玩家轨索引写出；其 SOUND 又不使用 x。
 * 因此测试在筛选玩家物件时始终排除 SOUND，并分别验证两种模式的字段集合。
 *
 * @par 折线表示约定
 *
 * Slide 折线根保存绝对 beat、x 和 w，seg 节点保存相对根节点的拍号与横向
 * 偏移。Hold 在同一位置停留仍是有效 seg；只有 duration 为零且不提供其它
 * 可见变化时才应清理。Flick 的方向可以在清理过程中合并，但合并后的对象
 * 必须脱离 sub-note 身份，否则再次保存会重复嵌套。
 *
 * @par 保存失败的事务边界
 *
 * mode 不受支持或折线子节点含不可表示的采样绑定时，返回 false 是预期结果。
 * 这类场景还会先删除旧目标并检查路径不存在，确保能力验证发生在打开或截断
 * 文件之前。日志中的 error 级消息不自动代表测试失败，应以 TEST_ASSERT
 * 和最终进程退出码为准。
 *
 * @par 浮点与整数比较
 *
 * 由 BPM 换算得到的毫秒时间使用 1e-6 或与拍号量化相符的容差；JSON 中的
 * column、x、offset、vol 及明确的 beat 三元组使用精确比较。音量在统一模型
 * 中是比例值，在 Malody 中是相对百分比增益，例如 -20 对应 0.8，16 对应
 * 1.16。测试同时覆盖负增益、正增益和字段缺失时的单位音量。
 *
 * @par 可重复执行
 *
 * 所有临时 MC 写入系统临时目录，并使用场景专属文件名。需要证明拒绝后无
 * 残留的测试会主动删除目标；其它成功场景允许覆盖上次产物。夹具不读取用户
 * 配置、项目最近记录或 tests/data，因此测试结果不依赖本地编辑器状态。
 *
 * @par 维护扩展规则
 *
 * 新增字段兼容场景时，应同时说明源字段形态、统一模型语义和目标格式能力。
 * 新增拍号算法场景时，应至少检查输出 beat、重新加载的绝对时间，以及编辑
 * 后缓存是否失效。新增拒绝路径时必须检查目标文件不存在。不要通过只比较
 * source 与 source 的成员来代替真实保存、解析和重载过程。
 */

/**
 * @brief 场景索引与主要回归信号。
 *
 * 以下索引按 main 中的执行顺序描述每个场景的唯一判定重点，便于从失败日志
 * 快速定位到对应格式职责，而不需要先通读全部程序化 JSON。
 *
 * @par test_zero_length_hold_degrade_to_flick
 *
 * - 输入是一组同拍零 Hold 与 +1 Flick；
 * - 输出模型应只有一个顶层 Flick；
 * - Polyline 与 Hold 容器都必须为空；
 * - dtrack=1 和非 sub-note 身份是保留语义的证据。
 *
 * @par test_multiple_zero_holds_same_flicks_merge
 *
 * - 输入为三组连续同向零时长节点；
 * - 清理后只能产生一个顶层 Flick；
 * - 位移应累计为 +3，不能只取首项或末项；
 * - 失败通常指向折线规范化中的方向合并循环。
 *
 * @par test_polyline_all_cleaned_degrade_to_note
 *
 * - 输入折线没有任何有效持续或方向变化；
 * - 根点击仍需退化为一个普通 Note；
 * - 所有子类型容器都应清空；
 * - 整条消失说明清理阶段没有保留折线根语义。
 *
 * @par test_key_mode_hold_uses_endbeat
 *
 * - Key Hold 的位置字段是 column；
 * - 结束时间使用绝对 endbeat；
 * - seg 与 x 都不应出现；
 * - 失败优先检查保存器的 mode 分派。
 *
 * @par test_slide_mode_saves_xw
 *
 * - Slide 普通 Note 必须同时包含 x 与 w；
 * - column 和 endbeat 属于错误的 Key 分支；
 * - 普通点击不应生成 seg；
 * - 失败通常来自轨道到皮肤坐标的换算入口。
 *
 * @par test_slide_mode_7k_8k_uses_skin_compatible_layout
 *
 * - 7K、8K 分别检查普通键、Flick 与 Polyline；
 * - x/w 使用皮肤可见键区，不是简单全宽均分；
 * - seg 横向偏移落在目标轨中心；
 * - 重载后的 dtrack 证明坐标换算可逆。
 *
 * @par test_unsupported_malody_mode_rejected
 *
 * - 输入结构有效但 mode=4 未实现；
 * - saveToFile 必须明确返回 false；
 * - 不能用 Key 或 Slide 字段猜测降级；
 * - 失败表示格式能力边界被意外放宽。
 *
 * @par test_key_mode_polyline_exports_key_fields
 *
 * - 混合折线只保留 Key 可表示的 Hold；
 * - 输出只有 column/endbeat 游戏节点；
 * - Flick、seg、dir、x、w 均不能泄漏；
 * - 失败指向折线展平或模式字段过滤。
 *
 * @par test_key_mode_flick_exports_single_note
 *
 * - Key 不保存 Flick 方向，只保留起点点击；
 * - 最终必须恰有一个 column Note；
 * - 不生成 endbeat 或任何 Slide 字段；
 * - 多节点说明降级阶段重复输出。
 *
 * @par testKeyAudioNodeUsesNumericType
 *
 * - Key 主音频以数值 type=1 识别；
 * - x=4 表示四轨后的第一条 BGM 轨；
 * - offset=0、vol=0 表示无偏移单位音量；
 * - column 出现说明 SOUND 被误当玩家节点。
 *
 * @par testSlideAudioNodeUsesSoundType
 *
 * - Slide 主音频使用字符串 SOUND；
 * - sound 路径保持 audio.ogg；
 * - 自动采样不带玩家 x 坐标；
 * - 数值 type 表示错误复用了 Key 输出协议。
 *
 * @par test_internal_offset_metadata_not_exported
 *
 * - initialDelay 与 audioOffset 不属于公开 meta；
 * - 真正的 sample offset=-75 仍需写出；
 * - 两类字段必须分别过滤和保存；
 * - 同时丢失说明过滤范围过宽。
 *
 * @par test_empty_version_exports_default_metadata
 *
 * - 空 version 必须替换成 default；
 * - song 子对象仍保留标题和作者；
 * - 不能因一个空字段丢弃整个 meta；
 * - 失败影响新建未命名难度的首次保存。
 *
 * @par test_sound_track_does_not_expand_key_count
 *
 * - SOUND x=8 不改变四条玩家轨；
 * - BGM 轨计数扩展到覆盖绝对轨道 8；
 * - 玩家集合只有一个普通 Note；
 * - 自动采样集合只有一个 SOUND。
 *
 * @par test_multiple_sound_objects_round_trip_without_global_shift
 *
 * - 三个 SOUND 保留独立资源、轨道、offset 和 gain；
 * - 同拍同轨采样不得去重；
 * - 玩家 hit sample 不进入自动采样集合；
 * - 任意 offset 都不能平移 timing 或玩家 Note。
 *
 * @par test_timing_delay_and_sample_offset_round_trip_independently
 *
 * - 同拍 timing delay 按来源顺序建立锚点；
 * - delay 前后的玩家 Note 使用各自时间段；
 * - SOUND offset 只修改有效播放时刻；
 * - 清除来源 beat 后仍能从 timestamp 逆算原拍号。
 *
 * @par test_non_malody_lead_in_exports_timing_origin_and_audio_compensation
 *
 * - 237ms 首 timing 回卷成 263ms 非负 delay；
 * - 首 timing 位于前半拍时，主 SOUND 仍用 offset 抵消回卷 delay；
 * - 普通 Note 的导出 beat 前移一拍；
 * - Key 与 Slide 重载后都恢复 237ms 绝对时间。
 *
 * @par test_late_first_timing_prepends_anchor_and_shifts_all_content
 *
 * - 首拍内后半相位不添加冗余 timing；
 * - 晚于首拍的原红线前添加合成 beat 0 锚点；
 * - 原红线、后续 BPM、SCROLL 和 Note 全部保留；
 * - 只有合成锚点携带配对 delay。
 *
 * @par test_first_timing_delay_unwraps_with_its_bpm
 *
 * - 首 BPM 拍长是相位取模的唯一模数；
 * - 只有同名、时间零主 SOUND 可与首 timing 配对；
 * - 亚毫秒与整拍编辑必须覆盖导入缓存；
 * - 历史形态最终收敛为稳定规范输出。
 *
 * @par test_malody_note_phase_shift_boundaries
 *
 * - 负首 timing 转为非负 delay 与主 offset；
 * - 整拍首 timing 的模相位精确为零；
 * - 低 BPM 前半拍保持正相位，后半拍回退为负相位；
 * - 玩家 Note 拍号按边界使用对应的完整拍补偿；
 * - 两种模式重载后都保持原绝对时间。
 *
 * @par test_paired_first_delay_round_trips_variable_bpm
 *
 * - 多 BPM 拍号按分段拍长累计；
 * - 配对正相位统一作用于 timing、effect 和 note；
 * - 后续红线不重复保存首 delay；
 * - 编辑后的 timestamp 替换各对象的来源 beat。
 *
 * @par test_legacy_samples_without_x_use_pro_editor_tracks
 *
 * - 同拍无 x SOUND 从绝对轨道 10 连续展开；
 * - 新拍点重新使用轨道 10；
 * - 显式合法 x 不参与重排；
 * - 规范回写后每个 SOUND 都拥有稳定 x。
 *
 * @par test_invalid_sample_track_and_song_hint_conflict
 *
 * - song.file 只保留提示语义；
 * - 同名玩家 sound 继续作为命中绑定；
 * - 落入玩家区的 SOUND 搬到首条 BGM 轨；
 * - original_x 与关联源路径的诊断同时保留。
 *
 * @par testEditedSampleTimestampOverridesImportedBeat
 *
 * - 输入 SOUND 的来源 beat 为 1；
 * - 模型编辑后 timestamp 对应 beat 3；
 * - 保存结果必须采用 beat 3；
 * - 重载后的 1500ms 证明编辑值成为权威。
 *
 * @par testStringBpmInNearlyEmptyMapLoads
 *
 * - time[].bpm 使用字符串 "234"；
 * - timing 与 preference_bpm 都解析成 234；
 * - SOUND 不进入玩家集合；
 * - 缺 vol 的 SOUND 使用单位音量。
 *
 * @par testMetadataOnlyMapLoadsWithDefaults
 *
 * - 输入只有 meta，没有三个内容数组；
 * - 玩家集合保持为空；
 * - 自动建立 120 BPM timing；
 * - Slide 缺轨数时使用四轨默认值。
 *
 * @par test_original_structure_not_leaked
 *
 * - 内部来源标记不得出现在顶层 note；
 * - seg 子节点也必须递归过滤；
 * - 发现时日志保留截断节点供定位；
 * - 最终对任意一次泄漏统一判失败。
 *
 * @par test_hold_stay_at_head_creates_valid_seg
 *
 * - Hold 与折线根横向位置相同；
 * - 非零 duration 仍表示有效持续；
 * - 输出必须含非空 seg；
 * - 该场景防止按零位移误删节点。
 *
 * @par testPolylineSubnoteSampleBindingRejected
 *
 * - Slide seg 无法表达玩家命中采样；
 * - 导出不得静默删除 sound/volume；
 * - saveToFile 应返回 false；
 * - 被拒绝目标不得留下半成品文件。
 *
 * @par testMalodyTimelineUsesFixedHighPrecisionFractions
 *
 * - effect 绝对拍号保留 1919/1920；
 * - seg 相对拍号保留 287/288；
 * - 两种入口共用固定分母候选；
 * - 接近整数拍时也不能错误进位。
 */

/**
 * @brief 按失败症状定位实现职责的检查指南。
 *
 * @par 玩家物件数量异常
 *
 * 若 m_allNotes 包含 SOUND，先检查 LoadMalodyMap 的 type 分类是否在读取
 * column、x 或 sound 之前执行。若一个 Polyline 变成多个顶层 Note，则检查
 * SaveMalodyMap 的零长度节点清理是否在折线根降级之前完成。Key Flick 或
 * Polyline 重复输出通常表示通用物件遍历与具体容器遍历同时消费了子节点。
 *
 * @par 玩家轨或 BGM 轨计数异常
 *
 * 玩家轨应优先来自 mode_ext.column 或 Slide 推断结果，SOUND x 只用于
 * BGM 绝对轨道范围。缺 x 历史采样需要先按触发时间分组再布局，不能先用
 * 默认零值参与轨数统计。非法 x 搬移后还需重新计算 bgm_track_count，但不得
 * 改写 track_count。
 *
 * @par SOUND 数量或身份异常
 *
 * 检查 isSoundNode 对数值 1、浮点 1.0 和字符串 SOUND 的兼容，以及普通
 * type=0 Note 是否仍保留自己的 sound 绑定。不要用 song.file 与 sound 路径
 * 相同作为主音频对象存在的证据；只有显式 SOUND 才进入 m_audioSamples。
 * 同拍节点必须以独立 JSON 对象保留，不能按 beat/x 组成的键去重。
 *
 * @par 音量断言异常
 *
 * Malody vol 是相对百分比增益，内部 volume 是线性比例。换算关系为
 * volume = 1 + vol/100；字段缺失与 vol=0 都对应 1.0。正增益允许得到大于
 * 1 的比例，不能套用只适合 UI 滑条的 0..1 限制。玩家绑定与自动采样使用
 * 同一数值关系，但存放在不同模型对象中。
 *
 * @par timing 绝对时间异常
 *
 * 普通 delay 只改变所属 BPM 锚点，并影响该锚点后的拍号换算。同 beat 多条
 * timing 需要保持来源顺序。首拍配对相位则是单独兼容规则，取模拍长必须来自
 * 第一条 BPM timing；不要用 preference_bpm 替代，因为后者可能只是显示值。
 *
 * @par 玩家 Note 或 effect 整体差一拍
 *
 * 先确认首 timing 的相位位于前半拍还是后半拍，以及导出是否插入合成锚点。
 * 正配对相位会让普通 note[] 和 effect[] 的导出 beat 一致前移，但 Slide seg
 * 保存的是相对根节点拍号，不应再次前移。导入时应对普通内容抵消该偏移。
 *
 * @par 主音频偏移异常
 *
 * 只有资源等于 song.file、有效播放位于时间零且 offset 与首 delay 构成历史
 * 配对时，加载器才把主采样规范为 timestamp=0、offset=0。普通效果音、数值
 * 不匹配的 offset、用户移动后的主采样都必须保留自己的时序，不能共享全局
 * 配对状态。
 *
 * @par 编辑后仍输出旧 beat
 *
 * 来源 beat metadata 只能在对象未移动时帮助保持精确分数。保存前应比较当前
 * timestamp 与由来源 beat 恢复的时间；不一致时重新量化。该判断分别适用于
 * BPM timing、SCROLL、玩家 Note、Hold 端点与自动采样，不能只修复某一容器。
 *
 * @par 高精度分拍异常
 *
 * 绝对时间线与相对 seg 都应调用同一固定分母候选逻辑。失败结果若变成相邻
 * 整数拍，通常是容差过大；若分母变成任意巨大数，通常是直接使用浮点有理化
 * 而绕过固定候选。测试要求值等价且表示稳定，便于 Malody 编辑器继续修改。
 *
 * @par 输出包含内部键
 *
 * map、timing、note 与 sample 的格式专属 metadata 都需要白名单或显式过滤。
 * initialDelay、audioOffset、original_structure、original_structure_flick
 * 仅用于 兼容恢复，不能成为 MC 协议的一部分。折线根和 seg
 * 节点需要分别检查过滤。
 *
 * @par 保存拒绝后仍有文件
 *
 * mode 能力检查和折线子节点采样检查必须发生在创建 ofstream 之前。不能依赖
 * 析构或删除补救，因为目标可能是用户已有文件。测试会清除旧产物后检查路径，
 * 因而任何残留都说明事务边界发生退化。
 */

/**
 * @brief MC 字段与统一模型权威来源对照。
 *
 * - meta.mode 由 MALODY 格式专属 metadata 决定，并选择 Key 或 Slide 协议；
 * - meta.free 是 mode 的派生字段，Key 为 0、Slide 为 1；
 * - meta.mode_ext.column 只描述 Key 玩家轨，不包含 BGM 轨；
 * - meta.song.file 对应 song_file_hint，只提供资源候选和主音频身份提示；
 * - meta.song.bpm 是展示偏好，实际拍轴仍由第一条 BPM timing 决定；
 * - meta.version 来自难度名，空值保存为 default 以维持可显示性；
 * - time[].beat 由当前 timing timestamp 按分段 BPM 时间线量化；
 * - time[].delay 是对应 BPM 锚点相位，不是全局音频 offset；
 * - time[].bpm 接受数字或可解析字符串，保存时规范为数值；
 * - effect[].beat 与普通 note[] 接受相同的首拍相位补偿；
 * - effect[].scroll 由 SCROLL timing 的效果参数决定；
 * - Key note.column 来自玩家绝对轨道在 Key 区域内的离散索引；
 * - Key note.endbeat 来自 Hold 结束 timestamp 的绝对拍号；
 * - Slide note.x 与 note.w 来自轨道范围到皮肤坐标的映射；
 * - Slide note.dir 来自 Flick 的有符号 dtrack；
 * - Slide note.seg 保存折线子节点相对根节点的时间和横向变化；
 * - seg[].beat 是相对拍号，不重复应用根节点的首拍全局相位；
 * - 玩家 note.sound 与 note.vol 来自 AudioSampleBinding；
 * - 自动采样 note.type 决定节点进入 m_audioSamples 而非 m_allNotes；
 * - 自动采样 note.beat 来自 m_timestamp，不包含 m_offsetMs；
 * - 自动采样 note.offset 直接来自有符号 m_offsetMs；
 * - 自动采样 note.x 在 Key 中是 BGM 绝对轨道，在 Slide 中通常省略；
 * - 自动采样 note.vol 与内部 m_volume 按百分比增益互换；
 * - bgm_track_count 是自动采样最大绝对轨道相对玩家轨末端的覆盖范围；
 * - original_x 只记录修复前的非法来源值，不反向覆盖规范轨道；
 * - collaborationId 等内部稳定身份不属于 MC 公开字段；
 * - original_structure 系列标记只辅助兼容重建，不允许写入 note 或 seg；
 * - 导入 beat metadata 仅在对象未编辑时帮助保持原分数；
 * - 当前 timestamp 与来源 beat 冲突时，以用户编辑后的 timestamp 为准；
 * - 首 timing 与时间零同名主 SOUND 满足条件时共享配对相位；
 * - 非主 SOUND、非零主 SOUND 或不匹配 offset 均按普通采样处理；
 * - 合成 beat 0 timing 只补足 Malody 时间原点，不取代晚到的原红线；
 * - 保存能力检查先于文件创建，无法表达的模型不进行有损降级。
 *
 * 该对照用于解释断言为什么同时检查 JSON 字段和重载模型。修改任一字段
 * 映射时，应确认权威来源没有被提示字段、来源缓存或另一模式的协议字段替代。
 * 若新增协议字段同时影响 Key 和 Slide，应分别增加正向字段断言和反向缺失
 * 断言；若字段参与时间换算，还需加入编辑后重新保存场景，证明来源 metadata
 * 不会覆盖当前模型。若只能由某一模式表达，则另一模式必须明确降级或拒绝，
 * 并在测试中检查是否产生多余节点或残留文件。这样才能同时约束读取兼容、
 * 规范保存、用户编辑和事务失败四条路径。
 */

using json = nlohmann::json;

namespace fs = std::filesystem;

static int g_failed = 0;

/// @brief 断言场景条件并在首次失败时结束当前测试函数。
///
/// 每个测试函数相互独立，失败只增加全局计数而不终止整个进程，使一次执行
/// 可以列出多个场景的回归。宏内 return 只返回当前 void 测试函数。
/// @param cond 必须成立的转换契约。
/// @param msg 失败时写入日志的稳定说明。
#define TEST_ASSERT(cond, msg)       \
    do {                             \
        if ( !(cond) ) {             \
            XERROR("FAIL: {}", msg); \
            g_failed++;              \
            return;                  \
        }                            \
    } while ( 0 )

/// @brief 判断 Malody JSON 节点是否为自动采样对象。
/// @param node 待检查的 note 节点。
/// @return 数值 1 或兼容字符串 SOUND 时返回 true。
static bool isSoundNode(const json& node)
{
    // 缺少 type 的节点可能仍含玩家命中 sound，不能据此判成自动采样。
    if ( !node.contains("type") ) return false;
    // Slide 历史文件使用可读字符串，Key 与当前规范输出使用数值 1。
    if ( node["type"].is_string() ) {
        return node["type"].get<std::string>() == "SOUND";
    }
    // 其它数字和非整数 JSON 类型仍属于玩家物件或未知节点。
    return node["type"].is_number_integer() && node["type"].get<int>() == 1;
}

/**
 * @brief 创建含首 timing、基础元数据和主音频的最小 BeatMap。
 *
 * 公共夹具把 BPM 固定为 120，使一拍恰好为 500ms，便于场景直接构造绝对
 * 时间。玩家轨数由参数决定，唯一主音频放在紧随玩家轨的首条 BGM 轨。
 * Malody mode 和 id 写入格式专属 metadata，确保保存器选择目标协议。
 *
 * 返回前执行 sync，建立通用物件索引。各测试可以继续修改具体容器，但在
 * 保存前仍需再次 sync，使新增对象进入保存器实际读取的视图。
 *
 * @param mode Malody mode，当前有效场景主要使用 0 Key 与 7 Slide。
 * @param trackCount 玩家轨数量，不包含 BGM 轨。
 * @return 可以直接扩展并保存的最小统一谱面。
 */
static MMM::BeatMap makeMinimalBeatMap(int mode, int trackCount)
{
    // 首 timing 同时提供 BPM、拍长和统一效果字段，避免依赖默认推断。
    MMM::BeatMap bm;
    MMM::Timing  t;
    t.m_timestamp             = 0.0;
    t.m_bpm                   = 120.0;
    t.m_beat_length           = 500.0;
    t.m_timingEffect          = MMM::TimingEffect::BPM;
    t.m_timingEffectParameter = 120.0;
    bm.m_timings.push_back(t);

    // 基础文本字段让导出 meta 完整有效，路径只作为测试资源标识使用。
    bm.m_baseMapMetadata.track_count     = trackCount;
    bm.m_baseMapMetadata.preference_bpm  = 120.0;
    bm.m_baseMapMetadata.title           = "EdgeCaseTest";
    bm.m_baseMapMetadata.artist          = "Test";
    bm.m_baseMapMetadata.author          = "Test";
    bm.m_baseMapMetadata.version         = "Test";
    bm.m_baseMapMetadata.song_file_hint  = fs::path("audio.ogg");
    bm.m_baseMapMetadata.main_cover_path = fs::path("cover.jpg");
    bm.m_baseMapMetadata.map_path        = fs::path("/tmp/test_edge.mc");
    bm.m_baseMapMetadata.bgm_track_count = 1;

    // 格式专属 mode 决定 Key 与 Slide 的序列化字段集合。
    bm.m_metadata.map_properties[MMM::MapMetadataType::MALODY]["mode"] =
        std::to_string(mode);
    bm.m_metadata.map_properties[MMM::MapMetadataType::MALODY]["id"] = "0";

    // 主音频显式建模为自动采样，不依赖 song_file_hint 合成播放对象。
    MMM::AudioSampleEvent sample;
    sample.m_timestamp       = 0.0;
    sample.m_track           = static_cast<uint32_t>(trackCount);
    sample.m_audioResourceId = "audio.ogg";
    bm.m_audioSamples.push_back(sample);

    // 建立 allNotes 等派生索引，统一后续测试的初始状态。
    bm.sync();
    return bm;
}

/**
 * @brief 通过正式 MC 保存与加载入口返回往返后的 BeatMap。
 *
 * 用于只关心统一模型结果的折线退化场景。需要检查原始 JSON 字段的测试
 * 不使用此 helper，而是自行打开导出文件。保存失败会记录路径并返回空模型，
 * 后续断言据此给出场景级失败，不抛出异常。
 *
 * @param bm 待保存的统一谱面快照。
 * @param tag 用于隔离临时文件名的场景标识。
 * @return 从导出 MC 重新加载并 sync 后的谱面；保存失败时返回空谱面。
 */
static MMM::BeatMap saveAndReload(const MMM::BeatMap& bm,
                                  const std::string&  tag)
{
    // 每个 tag 对应固定临时路径，方便失败后直接检查生成的 MC。
    fs::path outPath =
        std::filesystem::temp_directory_path() / ("edge_" + tag + ".mc");

    if ( !bm.saveToFile(outPath) ) {
        XERROR("Failed to save {}", outPath.string());
        return {};
    }

    // 重载后同步通用索引，以编辑器实际访问方式检查物件分类。
    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outPath);
    reloaded.sync();
    return reloaded;
}

/**
 * @brief 验证零长度 Hold 后接 Flick 时折线退化为独立 Flick。
 *
 * Slide 折线首节点没有持续时间，唯一可见语义来自同拍的方向变化。保存器
 * 应删除无效 Hold，同时把 Flick 提升出折线；若保留空 seg，Malody 客户端
 * 会得到不可见或无法编辑的折线结构。
 *
 * 场景检查对象数量、位移和 sub-note 标志，确保退化不是简单复制出额外物件。
 */
void test_zero_length_hold_degrade_to_flick()
{
    XINFO("=== Test: Zero-length Hold + Flick → Flick (dir mode) ===");

    auto bm = makeMinimalBeatMap(7 /*Slide*/, 4);

    // 折线根与两个子节点位于同一时刻，排除拍号换算对结果的干扰。
    // 构建 Polyline: [Hold(dur=0, track=0), Flick(dtrack=+1, track=0)]
    MMM::Polyline& poly = bm.m_noteData.polylines.emplace_back();
    poly.m_type         = MMM::NoteType::POLYLINE;
    poly.m_timestamp    = 1000.0;
    poly.m_track        = 0;

    MMM::Hold& h  = bm.m_noteData.holds.emplace_back();
    h.m_type      = MMM::NoteType::HOLD;
    h.m_timestamp = 1000.0;
    h.m_track     = 0;
    h.m_duration  = 0.0;  // 零长度
    h.m_isSubNote = true;
    // 同时登记通用子节点和具体 Hold 引用，模拟编辑器中的完整折线状态。
    poly.m_subNotes.push_back(h);
    poly.m_subHolds.push_back(h);

    MMM::Flick& f = bm.m_noteData.flicks.emplace_back();
    f.m_type      = MMM::NoteType::FLICK;
    f.m_timestamp = 1000.0;
    f.m_track     = 0;
    f.m_dtrack    = 1;
    f.m_isSubNote = true;
    // dtrack=1 是退化后必须保留的唯一可见方向信息。
    poly.m_subNotes.push_back(f);
    poly.m_subFlicks.push_back(f);

    // 保存前同步，使新增折线替换 helper 中的初始派生索引。
    bm.sync();
    auto reloaded = saveAndReload(bm, "zero_hold_flick");

    // 先验证总体仍有可玩物件，再检查具体容器的唯一性。
    TEST_ASSERT(!reloaded.m_allNotes.empty(), "reloaded map should have notes");
    TEST_ASSERT(reloaded.m_noteData.flicks.size() == 1,
                "should have exactly 1 Flick");
    TEST_ASSERT(reloaded.m_noteData.polylines.empty(),
                "should have no Polylines");
    TEST_ASSERT(reloaded.m_noteData.holds.empty(), "should have no Holds");

    const auto& f2 = reloaded.m_noteData.flicks.front();
    // 提升后的 Flick 必须成为顶层对象，否则下一次保存仍会按子节点处理。
    TEST_ASSERT(f2.m_dtrack == 1, "Flick dtrack should be 1");
    TEST_ASSERT(!f2.m_isSubNote, "Flick should not be a sub-note");

    XINFO("PASS: Zero-length Hold+Flick degraded to single Flick with dir");
}

/**
 * @brief 验证多组零 Hold 与同向 Flick 合并为一个累积位移 Flick。
 *
 * 三组节点都发生在同一时刻，表示没有持续轨迹、只有连续横移。清理空 Hold
 * 后，三个 +1 方向应合并为一个 +3，且中间折线和子节点全部消失。
 *
 * 该场景锁定方向合并顺序，避免只保留最后一次 Flick 或生成三个重叠物件。
 */
void test_multiple_zero_holds_same_flicks_merge()
{
    XINFO(
        "=== Test: Hold(0)+Flick(1)+Hold(0)+Flick(1)+Hold(0)+Flick(1) → "
        "single Flick(3) ===");

    auto bm = makeMinimalBeatMap(7 /*Slide*/, 4);

    MMM::Polyline& poly = bm.m_noteData.polylines.emplace_back();
    poly.m_type         = MMM::NoteType::POLYLINE;
    poly.m_timestamp    = 1000.0;
    poly.m_track        = 0;

    // 三个零长度 Hold + 三个同向 Flick；track 随累计位移逐步变化。
    for ( int i = 0; i < 3; i++ ) {
        MMM::Hold& h  = bm.m_noteData.holds.emplace_back();
        h.m_type      = MMM::NoteType::HOLD;
        h.m_timestamp = 1000.0;
        h.m_track     = i;  // track changes with each flick (after previous)
        h.m_duration  = 0.0;
        h.m_isSubNote = true;
        poly.m_subNotes.push_back(h);
        poly.m_subHolds.push_back(h);

        // 每段固定右移一轨，预期总位移是循环次数之和。
        MMM::Flick& f = bm.m_noteData.flicks.emplace_back();
        f.m_type      = MMM::NoteType::FLICK;
        f.m_timestamp = 1000.0;
        f.m_track     = i;
        f.m_dtrack    = 1;
        f.m_isSubNote = true;
        poly.m_subNotes.push_back(f);
        poly.m_subFlicks.push_back(f);
    }

    bm.sync();
    // 通过磁盘往返触发保存器清理与加载器重建，而非直接检查源对象。
    auto reloaded = saveAndReload(bm, "multi_zero_hold_flick");

    TEST_ASSERT(!reloaded.m_allNotes.empty(), "reloaded map should have notes");
    TEST_ASSERT(reloaded.m_noteData.flicks.size() == 1,
                "should have exactly 1 Flick");
    TEST_ASSERT(reloaded.m_noteData.polylines.empty(),
                "should have no Polylines");
    TEST_ASSERT(reloaded.m_noteData.holds.empty(), "should have no Holds");

    const auto& f2 = reloaded.m_noteData.flicks.front();
    // +3 同时证明三段都参与合并且方向符号没有丢失。
    TEST_ASSERT(f2.m_dtrack == 3,
                "Flick dtrack should be 3 (merged from 3x+1)");
    TEST_ASSERT(!f2.m_isSubNote, "Flick should not be a sub-note");

    XINFO("PASS: Multiple zero-hold+flick merged into single Flick(3)");
}

/**
 * @brief 验证折线所有子节点清理后退化为根位置的普通 Note。
 *
 * 夹具只包含三个同拍、同轨、零长度 Hold，没有 Flick 或有效停留段。整条
 * 折线因此没有可输出的 seg，但根点击仍代表一个有效玩家输入，不能整条删除。
 *
 * 往返后只允许存在一个普通 Note，且 Hold、Flick、Polyline 容器均为空。
 */
void test_polyline_all_cleaned_degrade_to_note()
{
    XINFO("=== Test: All sub-notes cleaned → degrade to Note ===");

    auto bm = makeMinimalBeatMap(7 /*Slide*/, 4);

    MMM::Polyline& poly = bm.m_noteData.polylines.emplace_back();
    poly.m_type         = MMM::NoteType::POLYLINE;
    poly.m_timestamp    = 1000.0;
    poly.m_track        = 0;

    // 只有零长度 Hold（无 Flick），每一项都应在序列化前被过滤掉。
    for ( int i = 0; i < 3; i++ ) {
        MMM::Hold& h  = bm.m_noteData.holds.emplace_back();
        h.m_type      = MMM::NoteType::HOLD;
        h.m_timestamp = 1000.0;
        h.m_track     = 0;
        h.m_duration  = 0.0;
        h.m_isSubNote = true;
        poly.m_subNotes.push_back(h);
        poly.m_subHolds.push_back(h);
    }

    bm.sync();
    // 退化结果由保存器生成，源模型本身仍保持原始折线用于编辑撤销语义。
    auto reloaded = saveAndReload(bm, "all_cleaned_note");

    TEST_ASSERT(!reloaded.m_allNotes.empty(), "reloaded map should have notes");
    // 唯一普通 Note 承接折线根的点击，不允许产生多个同拍重叠 Note。
    TEST_ASSERT(reloaded.m_noteData.notes.size() == 1,
                "should have exactly 1 Note (degraded from empty polyline)");
    TEST_ASSERT(reloaded.m_noteData.polylines.empty(),
                "should have no Polylines");
    TEST_ASSERT(reloaded.m_noteData.holds.empty(), "should have no Holds");
    TEST_ASSERT(reloaded.m_noteData.flicks.empty(), "should have no Flicks");

    XINFO("PASS: Empty polyline degraded to Note");
}

/**
 * @brief 验证分拍编辑与 IMD 整毫秒混合后仍按 RM 规则导出阶梯段落。
 *
 * RM 的一个 seg 表示先在前轨长按，再在段尾横移。显式写两个同拍端点
 * 会产生额外虚拟段落。整毫秒 Hold 尾与分拍 Flick 的误差则可能导致
 * 导出拍号倒退，必须在同轨连续连接处折叠，并以 Flick 的时间为准。
 *
 * 测试经过 MMM 保存重开，直接检查端点数量、时间顺序与每次转向。
 * 回到根轨时偏移为零，同样必须保留这一段，避免下一段从错误轨道开始。
 *
 * @par RM 外部消费规则
 * 皮肤的第一个长按从根轨开始，后续长按从上一 seg 的目标轨开始。
 * 每段的终轨取 root.x + seg.x；横移跨度取两个连续端点的差。
 * 因此四次转向应有四个 seg，而不是八个几何角点。
 * 双端点会使皮肤创建零时长长按，不能通过“同拍即无害”放宽断言。
 *
 * @par 时间精度边界
 * 125 ms 对应 120 BPM 的四分之一拍，预期公开拍号完全确定。
 * 误差分别放在 Hold 的持续时间中，Flick 的转向时间保持不变。
 * 略晚的尾部可触发拍号倒退，略早的尾部可生成额外极短长按。
 * 两种方向都必须折叠为相同的标准输出，不能仅修复倒退的情况。
 */
void testQuantizedStaircaseExportsRmSegments()
{
    auto  map            = makeMinimalBeatMap(7, 4);
    auto& polyline       = map.m_noteData.polylines.emplace_back();
    polyline.m_timestamp = 1000.0;
    polyline.m_track     = 3;
    constexpr std::array<int, 5> tracks{ 3, 2, 1, 2, 3 };
    // 同时覆盖精确接点、尾部略晚与略早的整毫秒量化误差。
    constexpr std::array<double, 4> errors{ 0.0, 1.0 / 3, -1.0 / 3, 0.0 };
    for ( size_t i = 0; i < errors.size(); ++i ) {
        auto& hold       = map.m_noteData.holds.emplace_back();
        hold.m_timestamp = 1000.0 + 125.0 * i;
        hold.m_track     = tracks[i];
        hold.m_duration  = 125.0 + errors[i];
        hold.m_isSubNote = true;
        polyline.m_subNotes.push_back(hold);
        polyline.m_subHolds.push_back(hold);
        // Flick 是实际转向时刻；误差只在导出投影中消除，不修改源 Hold。
        auto& flick       = map.m_noteData.flicks.emplace_back();
        flick.m_timestamp = 1000.0 + 125.0 * (i + 1);
        flick.m_track     = tracks[i];
        flick.m_dtrack    = tracks[i + 1] - tracks[i];
        flick.m_isSubNote = true;
        polyline.m_subNotes.push_back(flick);
        polyline.m_subFlicks.push_back(flick);
    }
    // 与用户操作一致：先同步、持久化，再从磁盘建立新的对象关系。
    // 这样检查的是另存后打包入口，而不是刚绘制完的内存引用状态。
    map.sync();
    const auto mmmPath = fs::temp_directory_path() / "edge_rm_staircase.mmm";
    const auto mcPath  = fs::temp_directory_path() / "edge_rm_staircase.mc";
    TEST_ASSERT(map.saveToFile(mmmPath), "staircase should save to MMM");
    auto saved = MMM::BeatMap::loadFromFile(mmmPath);
    saved.sync();
    TEST_ASSERT(saved.saveToFile(mcPath), "saved MMM should export to MC");
    std::ifstream input(mcPath);
    const auto    document = json::parse(input, nullptr, false);
    TEST_ASSERT(!document.is_discarded(), "MC should contain valid JSON");
    // 自动采样也位于 note 数组，不能假定玩家折线固定排在第一项。
    const auto node =
        std::find_if(document["note"].begin(),
                     document["note"].end(),
                     [](const json& value) { return value.contains("seg"); });
    TEST_ASSERT(node != document["note"].end(), "staircase root should exist");
    const auto& segments = (*node)["seg"];
    // 先限定段落数量，再逐段索引，避免缺失节点变成无关的 JSON 断言。
    // 多出一段即代表游戏会建立额外虚拟长按，不能只比较最终落轨。
    TEST_ASSERT(segments.size() == 4, "each RM corner requires one segment");
    for ( size_t i = 0; i < tracks.size() - 1; ++i ) {
        const json expectedBeat = i == 3   ? json::array({ 1, 0, 1 })
                                  : i == 1 ? json::array({ 0, 1, 2 })
                                           : json::array({ 0, i + 1, 4 });
        TEST_ASSERT(segments[i]["beat"] == expectedBeat,
                    "segment time must follow the actual Flick endpoint");
        TEST_ASSERT(segments[i].value("x", 0) == 64 * (tracks[i + 1] - 3),
                    "RM segment must end on the next lane relative to root");
        // 按 RM 皮肤的读取规则核对长按起轨与横移终轨，而非本项目往返。
        const int previousX = i == 0 ? 0 : segments[i - 1].value("x", 0);
        TEST_ASSERT(previousX == 64 * (tracks[i] - 3),
                    "RM Hold must start on the previous segment lane");
    }
    // 导出容差属于目标格式投影，原谱面保留自身的精确编辑结果。
    // 该断言同时防止保存操作偷偷改动后续撤销或其它格式的输出。
    TEST_ASSERT(std::abs(map.m_noteData.holds[1].m_duration -
                         (125.0 + errors[1])) < 1e-7,
                "export cleanup must not mutate the source duration");
}

/**
 * @brief 验证尾部容差只用于同轨相接的 Hold/Flick，不跨越真实间隔。
 *
 * 不同轨的邻接节点不应按时间误差折叠；超过 IMD 整毫秒精度的间隔
 * 也必须保留。此场景绕过 MMM 的合并流程，专门限定保存器的清理边界。
 *
 * 两个反例均由单个 Hold 与单个 Flick 构成，排除同类型合并的干扰。
 * 时间反例保留 2 ms 间隔，明确超过 1 ms 的格式量化范围。
 * 轨道反例在完全相同的端点时间发生，但起轨不同，不能视为一个角点。
 * 此测试只限定输出段落数，不要求加载器修复这些不连续输入的语义。
 */
void testRmCornerCleanupRespectsContinuity()
{
    for ( const bool wrongTrack : { false, true } ) {
        auto  map         = makeMinimalBeatMap(7, 4);
        auto& poly        = map.m_noteData.polylines.emplace_back();
        poly.m_timestamp  = 1000.0;
        poly.m_track      = 0;
        auto& hold        = map.m_noteData.holds.emplace_back();
        hold.m_timestamp  = 1000.0;
        hold.m_duration   = 125.0;
        hold.m_track      = 0;
        hold.m_isSubNote  = true;
        auto& flick       = map.m_noteData.flicks.emplace_back();
        flick.m_timestamp = wrongTrack ? 1125.0 : 1127.0;
        flick.m_track     = wrongTrack ? 1 : 0;
        flick.m_dtrack    = 1;
        flick.m_isSubNote = true;
        // 两个场景分别隔离轨道不连续和时间不连续，不能被容差扩大吞掉。
        poly.m_subNotes.push_back(hold);
        poly.m_subNotes.push_back(flick);
        poly.m_subHolds.push_back(hold);
        poly.m_subFlicks.push_back(flick);
        map.sync();
        const auto path = fs::temp_directory_path() / "edge_rm_separate.mc";
        TEST_ASSERT(map.saveToFile(path), "separate segments should export");
        std::ifstream input(path);
        const auto    document = json::parse(input, nullptr, false);
        TEST_ASSERT(!document.is_discarded(), "separate MC should be valid");
        const auto node = std::find_if(
            document["note"].begin(),
            document["note"].end(),
            [](const json& value) { return value.contains("seg"); });
        TEST_ASSERT(
            node != document["note"].end() && (*node)["seg"].size() == 2,
            "discontinuous segments must not be collapsed");
    }
}

/**
 * @brief 验证 Key Hold 使用 column 与绝对 endbeat，而不是 Slide seg。
 *
 * 500ms 持续时间在 120 BPM 下正好是一拍，适合检查结束拍号字段是否存在。
 * 测试直接读取 JSON，避免加载器把错误的 Slide 表示再次容错成 Hold。
 */
void test_key_mode_hold_uses_endbeat()
{
    XINFO("=== Test: Key mode Hold uses endbeat ===");

    auto bm = makeMinimalBeatMap(0 /*Key*/, 4);

    // 顶层 Hold 不属于折线，Key 保存器应直接输出一个游戏节点。
    MMM::Hold& h  = bm.m_noteData.holds.emplace_back();
    h.m_type      = MMM::NoteType::HOLD;
    h.m_timestamp = 1000.0;
    h.m_track     = 0;
    h.m_duration  = 500.0;
    h.m_isSubNote = false;

    bm.sync();
    // 使用独立路径保留原始 JSON 供字段级断言。
    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_key_hold_endbeat.mc";
    bm.saveToFile(outPath);

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("meta") && j["meta"].value("mode", -1) == 0,
                "mode should be 0 (Key)");
    TEST_ASSERT(j["meta"].value("free", -1) == 0,
                "free should be 0 in Key mode");

    // 排除 helper 自动加入的主 SOUND，只统计可玩节点。
    auto gameNotes = json::array();
    for ( const auto& n : j["note"] ) {
        if ( isSoundNode(n) ) continue;
        gameNotes.push_back(n);
    }
    TEST_ASSERT(gameNotes.size() == 1, "should have 1 game note");
    // column/endbeat 是 Key Hold 的必要字段，seg/x 则属于 Slide 协议。
    TEST_ASSERT(gameNotes[0].contains("column"), "should have column");
    TEST_ASSERT(gameNotes[0].contains("endbeat"), "should have endbeat");
    TEST_ASSERT(!gameNotes[0].contains("seg"), "should NOT have seg");
    TEST_ASSERT(!gameNotes[0].contains("x"), "should NOT have x");

    XINFO("PASS: Key mode Hold correctly uses column + endbeat");
}

/**
 * @brief 验证 Slide 普通 Note 使用皮肤坐标 x 与宽度 w。
 *
 * Slide 没有离散 column 字段，保存器需按轨道和皮肤布局换算中心坐标与宽度。
 * 普通 Note 没有持续时间，因此也不应出现 Key endbeat 或 Slide seg。
 */
void test_slide_mode_saves_xw()
{
    XINFO("=== Test: Slide mode note uses x + w ===");

    auto bm = makeMinimalBeatMap(7 /*Slide*/, 4);

    // 选择轨道 1，确保坐标不是边界轨道的特殊值。
    MMM::Note& n  = bm.m_noteData.notes.emplace_back();
    n.m_type      = MMM::NoteType::NOTE;
    n.m_timestamp = 1000.0;
    n.m_track     = 1;

    bm.sync();
    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_slide_xw.mc";
    bm.saveToFile(outPath);

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("meta") && j["meta"].value("mode", -1) == 7,
                "mode should be 7 (Slide)");
    TEST_ASSERT(j["meta"].value("free", -1) == 1,
                "free should be 1 in Slide mode");

    // 主 SOUND 与玩家 Note 共用 note[]，字段检查前必须先分类。
    auto gameNotes = json::array();
    for ( const auto& n2 : j["note"] ) {
        if ( isSoundNode(n2) ) continue;
        gameNotes.push_back(n2);
    }
    TEST_ASSERT(gameNotes.size() == 1, "should have 1 game note");
    // x/w 成对出现；任何 Key 字段都说明模式路由错误。
    TEST_ASSERT(gameNotes[0].contains("x"), "should have x");
    TEST_ASSERT(gameNotes[0].contains("w"), "should have w");
    TEST_ASSERT(!gameNotes[0].contains("column"), "should NOT have column");
    TEST_ASSERT(!gameNotes[0].contains("endbeat"), "should NOT have endbeat");

    XINFO("PASS: Slide mode note correctly uses x + w");
}

/**
 * @brief 确认 7K、8K Slide 写出遵循 Rhythm Master 皮肤的分轨与宽度规则。
 *
 * 7K 与 8K 并非把四轨坐标线性放大；皮肤对普通键宽、Flick 跨轨宽度、
 * 根节点中心和 seg 横向偏移分别有固定约定。表驱动场景同时创建普通 Note、
 * 跨全宽 Flick 和单段 Polyline，覆盖三类换算入口。
 *
 * 原始 JSON 检查中心和宽度，重载模型再检查 Flick 的离散 dtrack，证明像素
 * 坐标可以无歧义恢复为轨道位移。每个 case 结束后删除临时文件。
 */
void test_slide_mode_7k_8k_uses_skin_compatible_layout()
{
    XINFO("=== Test: Slide 7K/8K uses skin-compatible layout ===");

    // 期望值来自 Rhythm Master 皮肤的有效键区，而非均分整个坐标范围。
    struct SlideLayoutCase {
        int trackCount;
        int noteWidth;
        int flickWidth;
        int noteX;
        int flickX;
        int segmentOffset;
    };

    // 7K 与 8K 使用不同键宽和中心，分别锁定避免共享错误常量。
    constexpr std::array<SlideLayoutCase, 2> cases{
        SlideLayoutCase{ 7, 30, 36, 55, 18, 182 },
        SlideLayoutCase{ 8, 20, 27, 48, 16, 192 },
    };

    for ( const auto& testCase : cases ) {
        // 每个轨数都从相同最小 Slide 谱面开始，只有布局参数不同。
        auto bm = makeMinimalBeatMap(7 /*Slide*/, testCase.trackCount);

        // 普通 Note 检查单轨中心与基础宽度。
        MMM::Note& note  = bm.m_noteData.notes.emplace_back();
        note.m_type      = MMM::NoteType::NOTE;
        note.m_timestamp = 1000.0;
        note.m_track     = 1;

        // Flick 从最左轨跨到最右轨，宽度包含完整横向覆盖距离。
        MMM::Flick& flick = bm.m_noteData.flicks.emplace_back();
        flick.m_type      = MMM::NoteType::FLICK;
        flick.m_timestamp = 1250.0;
        flick.m_track     = 0;
        flick.m_dtrack    = testCase.trackCount - 1;

        // Polyline 根使用普通键宽，子 Flick 的 seg offset 指向目标轨中心。
        MMM::Polyline& polyline = bm.m_noteData.polylines.emplace_back();
        polyline.m_type         = MMM::NoteType::POLYLINE;
        polyline.m_timestamp    = 1500.0;
        polyline.m_track        = 1;

        MMM::Flick& subFlick = bm.m_noteData.flicks.emplace_back();
        subFlick.m_type      = MMM::NoteType::FLICK;
        subFlick.m_timestamp = 1750.0;
        subFlick.m_track     = 1;
        subFlick.m_dtrack    = testCase.trackCount - 2;
        subFlick.m_isSubNote = true;
        polyline.m_subNotes.push_back(subFlick);
        polyline.m_subFlicks.push_back(subFlick);

        bm.sync();
        const fs::path outPath =
            std::filesystem::temp_directory_path() /
            ("edge_slide_" + std::to_string(testCase.trackCount) + "k.mc");
        TEST_ASSERT(bm.saveToFile(outPath), "7K/8K Slide map should save");

        std::ifstream ifs(outPath);
        json          document;
        ifs >> document;

        // 根据互斥字段分类三类玩家对象，避免依赖 JSON 数组输出顺序。
        const json* noteNode     = nullptr;
        const json* flickNode    = nullptr;
        const json* polylineNode = nullptr;
        for ( const auto& node : document["note"] ) {
            if ( isSoundNode(node) ) continue;
            if ( node.contains("dir") ) {
                flickNode = &node;
            } else if ( node.contains("seg") ) {
                polylineNode = &node;
            } else {
                noteNode = &node;
            }
        }

        // 三类对象必须各自存在，缺项时不继续解引用对应节点。
        TEST_ASSERT(noteNode != nullptr && flickNode != nullptr &&
                        polylineNode != nullptr,
                    "7K/8K Slide output should keep all note forms");
        TEST_ASSERT(
            noteNode->value("x", -1) == testCase.noteX &&
                noteNode->value("w", -1) == testCase.noteWidth,
            "plain note should use the skin-compatible center and width");
        TEST_ASSERT(
            flickNode->value("x", -1) == testCase.flickX &&
                flickNode->value("w", -1) == testCase.flickWidth,
            "Flick width should preserve its encoded cross-lane distance");
        TEST_ASSERT(
            polylineNode->value("x", -1) == testCase.noteX &&
                polylineNode->value("w", -1) == testCase.noteWidth,
            "Polyline root should stay inside the skin key-width range");
        TEST_ASSERT((*polylineNode)["seg"].size() == 1 &&
                        (*polylineNode)["seg"][0].value("x", 0) ==
                            testCase.segmentOffset,
                    "Polyline segment should land on the target lane center");

        // 像素字段正确之后，再证明导入能还原原始离散 Flick 位移。
        const auto reloaded     = MMM::BeatMap::loadFromFile(outPath);
        const bool decodedFlick = std::any_of(
            reloaded.m_noteData.flicks.begin(),
            reloaded.m_noteData.flicks.end(),
            [&](const MMM::Flick& loadedFlick) {
                return !loadedFlick.m_isSubNote &&
                       std::abs(loadedFlick.m_timestamp - 1250.0) < 1e-5 &&
                       loadedFlick.m_dtrack == testCase.trackCount - 1;
            });
        TEST_ASSERT(decodedFlick,
                    "7K/8K Flick distance should survive MC round trip");

        // 清理成功产物，避免系统临时目录积累多轨夹具。
        std::error_code removeError;
        std::filesystem::remove(outPath, removeError);
    }

    XINFO("PASS: Slide 7K/8K layout matches the skin rules");
}

/**
 * @brief 确认不支持的 Malody mode 会阻止导出。
 *
 * 当前保存器只实现 Key 与 Slide 的字段映射。Catch mode 4 若被勉强写出，
 * 统一模型中的轨道语义无法正确映射，形成表面成功但不可用的文件。
 *
 * 该场景只检查返回值；目标模式能力应在正式序列化前完成判断。
 */
void test_unsupported_malody_mode_rejected()
{
    XINFO("=== Test: Unsupported Malody mode rejected ===");

    // 使用结构上有效但协议未实现的 mode，区分模式拒绝与谱面损坏。
    auto bm = makeMinimalBeatMap(4 /*Catch*/, 4);

    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_unsupported_mode.mc";

    // 成功返回会让调用方误以为 Catch 语义已被无损保存。
    TEST_ASSERT(!bm.saveToFile(outPath),
                "mode 4 should be rejected by Malody exporter");

    XINFO("PASS: Unsupported Malody mode rejected");
}

/**
 * @brief 确认 Key 模式导出折线时只保留可表示的 Hold 子物件。
 *
 * 统一模型允许折线中混合 Hold 与 Flick，Key MC 却没有 seg/dir 表示。
 * 保存器应展平有效 Hold 为 column/endbeat，并忽略只服务 Slide 手势的 Flick，
 * 而不是把整个折线误写成 Slide 节点。
 *
 * JSON 中最终只允许一个游戏节点，并要求所有 Slide 专属字段缺失。
 */
void test_key_mode_polyline_exports_key_fields()
{
    XINFO("=== Test: Key mode polyline exports key fields ===");

    auto bm = makeMinimalBeatMap(0 /*Key*/, 4);

    // 折线根只承担统一模型容器职责，不应在 Key 输出中单独成为节点。
    MMM::Polyline& poly = bm.m_noteData.polylines.emplace_back();
    poly.m_type         = MMM::NoteType::POLYLINE;
    poly.m_timestamp    = 1000.0;
    poly.m_track        = 0;

    // 一拍 Hold 有明确 Key 等价表示，应保留为带 endbeat 的游戏节点。
    MMM::Hold& h  = bm.m_noteData.holds.emplace_back();
    h.m_type      = MMM::NoteType::HOLD;
    h.m_timestamp = 1000.0;
    h.m_track     = 0;
    h.m_duration  = 500.0;
    h.m_isSubNote = true;
    poly.m_subNotes.push_back(h);
    poly.m_subHolds.push_back(h);

    // 后续 Flick 只有 Slide dir 语义，Key 输出不能虚构等价方向字段。
    MMM::Flick& f = bm.m_noteData.flicks.emplace_back();
    f.m_type      = MMM::NoteType::FLICK;
    f.m_timestamp = 1750.0;
    f.m_track     = 2;
    f.m_dtrack    = 1;
    f.m_isSubNote = true;
    poly.m_subNotes.push_back(f);
    poly.m_subFlicks.push_back(f);

    bm.sync();
    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_key_polyline.mc";
    TEST_ASSERT(bm.saveToFile(outPath), "key polyline map should save");

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("meta") && j["meta"].value("mode", -1) == 0,
                "mode should be 0 (Key)");

    // 排除主音频后，折线展平只应留下可表示的 Hold。
    auto gameNotes = json::array();
    for ( const auto& n : j["note"] ) {
        if ( isSoundNode(n) ) continue;
        gameNotes.push_back(n);
    }

    TEST_ASSERT(gameNotes.size() == 1,
                "key polyline should only export subHold notes");

    // 逐字段证明节点遵守 Key 协议，并确认 endbeat 没在展平时丢失。
    bool hasHold = false;
    for ( const auto& note : gameNotes ) {
        TEST_ASSERT(note.contains("column"), "key note should have column");
        TEST_ASSERT(!note.contains("x"), "key note should not have x");
        TEST_ASSERT(!note.contains("w"), "key note should not have w");
        TEST_ASSERT(!note.contains("seg"), "key note should not have seg");
        TEST_ASSERT(!note.contains("dir"), "key note should not have dir");
        if ( note.contains("endbeat") ) {
            hasHold = true;
        }
    }

    TEST_ASSERT(hasHold, "flattened key polyline should keep hold endbeat");

    XINFO("PASS: Key mode polyline exports only key hold fields");
}

/**
 * @brief 确认 Key 模式导出普通 Flick 时降级为单个 Note。
 *
 * Key MC 无法表达 dtrack 手势，但 Flick 的起点仍代表一次有效击打。导出器
 * 选择保留起点为 column Note，并明确不写 dir、seg、x、w 或 endbeat。
 *
 * 该降级只产生一个节点，避免同时输出原 Flick 与补偿 Note。
 */
void test_key_mode_flick_exports_single_note()
{
    XINFO("=== Test: Key mode Flick exports single Note ===");

    auto bm = makeMinimalBeatMap(0 /*Key*/, 4);

    // 非零 dtrack 证明方向确实存在，只是目标格式主动降级该语义。
    MMM::Flick& flick = bm.m_noteData.flicks.emplace_back();
    flick.m_type      = MMM::NoteType::FLICK;
    flick.m_timestamp = 1000.0;
    flick.m_track     = 1;
    flick.m_dtrack    = 2;

    bm.sync();
    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_key_flick.mc";
    TEST_ASSERT(bm.saveToFile(outPath), "key flick map should save");

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    // SOUND 不参与玩家节点数量，先从共享 note[] 中过滤。
    auto gameNotes = json::array();
    for ( const auto& n : j["note"] ) {
        if ( isSoundNode(n) ) continue;
        gameNotes.push_back(n);
    }

    // column 是降级后唯一位置字段，其余两种模式专属字段必须全部缺失。
    TEST_ASSERT(gameNotes.size() == 1, "key flick should export one note");
    TEST_ASSERT(gameNotes[0].contains("column"),
                "key flick should have column");
    TEST_ASSERT(!gameNotes[0].contains("dir"), "key flick should not have dir");
    TEST_ASSERT(!gameNotes[0].contains("x"), "key flick should not have x");
    TEST_ASSERT(!gameNotes[0].contains("w"), "key flick should not have w");
    TEST_ASSERT(!gameNotes[0].contains("seg"), "key flick should not have seg");
    TEST_ASSERT(!gameNotes[0].contains("endbeat"),
                "key flick should not have endbeat");

    XINFO("PASS: Key mode Flick exports as single Note");
}

/**
 * @brief 确认 Malody Key 自动采样使用数值 type=1 和绝对 BGM 轨道。
 *
 * helper 创建的主音频位于四条玩家轨之后，因此导出 x 必须为 4。Key 格式
 * 使用数值类型标记自动采样，不能使用玩家 column，也不能把单位音量写成
 * 100；Malody 增益零才对应统一模型音量 1.0。
 */
void testKeyAudioNodeUsesNumericType()
{
    XINFO("=== Test: Key audio node uses numeric type ===");

    auto bm = makeMinimalBeatMap(0 /*Key*/, 4);

    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_audio_type.mc";
    TEST_ASSERT(bm.saveToFile(outPath), "canonical audio sample should save");

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("note") && !j["note"].empty(),
                "note array should not be empty");
    // 遍历而不是假设主音频在数组首位，同时确认没有重复生成采样。
    const json* audioNode = nullptr;
    for ( const auto& node : j["note"] ) {
        if ( isSoundNode(node) ) {
            TEST_ASSERT(audioNode == nullptr,
                        "minimal map should export exactly one audio sample");
            audioNode = &node;
        }
    }
    TEST_ASSERT(audioNode != nullptr, "audio sample should be present");
    // 类型形态本身属于兼容协议，数值 1 与字符串 SOUND 不能混用。
    TEST_ASSERT((*audioNode)["type"].is_number_integer(),
                "audio sample type should be numeric");
    TEST_ASSERT((*audioNode)["type"].get<int>() == 1,
                "audio sample type should be 1");
    TEST_ASSERT(audioNode->value("sound", "") == "audio.ogg",
                "audio sample should keep its resource id");
    // x=4 是绝对 BGM 轨，而非四轨 Key 中越界的玩家 column。
    TEST_ASSERT(audioNode->value("x", -1) == 4,
                "first BGM track should immediately follow four key tracks");
    TEST_ASSERT(audioNode->value("offset", -1.0) == 0.0,
                "audio sample should keep zero offset");
    TEST_ASSERT(audioNode->value("vol", -1.0) == 0.0,
                "unit volume should serialize as neutral gain 0");
    TEST_ASSERT(!audioNode->contains("column"),
                "audio sample must not use playable column");

    XINFO("PASS: Key audio node uses numeric type");
}

/**
 * @brief 确认 Malody Slide 主音轨使用游戏可识别的字符串 SOUND 类型。
 *
 * Slide 客户端识别字符串 SOUND，并且自动采样没有玩家物件的 x/w 坐标。
 * 场景与 Key 数值 type 测试成对存在，防止统一序列化分支错误复用协议。
 */
void testSlideAudioNodeUsesSoundType()
{
    XINFO("=== Test: Slide audio node uses SOUND type ===");

    auto bm = makeMinimalBeatMap(7 /*Slide*/, 4);

    const fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_slide_audio_type.mc";
    TEST_ASSERT(bm.saveToFile(outPath), "slide audio sample should save");

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("note") && !j["note"].empty(),
                "slide note array should not be empty");
    // 使用公共判定器兼容读取两种 SOUND 类型，再单独断言 Slide 的字符串形态。
    const auto audioNode =
        std::find_if(j["note"].begin(), j["note"].end(), isSoundNode);
    TEST_ASSERT(audioNode != j["note"].end(),
                "slide audio sample should be present");
    // 类型和值都检查，避免任意字符串被误认为有效 SOUND。
    TEST_ASSERT((*audioNode)["type"].is_string(),
                "slide audio sample type should be a string");
    TEST_ASSERT((*audioNode)["type"].get<std::string>() == "SOUND",
                "slide audio sample type should be SOUND");
    TEST_ASSERT(audioNode->value("sound", "") == "audio.ogg",
                "slide audio sample should keep its resource id");
    TEST_ASSERT(!audioNode->contains("x"),
                "slide audio sample should not export x");

    XINFO("PASS: Slide audio node uses SOUND type");
}

/**
 * @brief 确认内部兼容 offset 元数据不会导出到 Malody meta。
 *
 * initialDelay 与 audioOffset 是加载/编辑阶段使用的历史辅助键，不属于公开
 * Malody meta 协议。显式自动采样的 m_offsetMs 才应写入该节点的 offset。
 *
 * 测试同时证明过滤内部键不会误删真正的逐采样偏移。
 */
void test_internal_offset_metadata_not_exported()
{
    XINFO("=== Test: Internal offset metadata not exported ===");

    // 把两个内部键放入格式专属 map_properties，模拟旧加载结果残留。
    auto  bm    = makeMinimalBeatMap(7 /*Slide*/, 4);
    auto& props = bm.m_metadata.map_properties[MMM::MapMetadataType::MALODY];
    props["initialDelay"]                       = "123";
    props["audioOffset"]                        = "456";
    bm.m_audioSamples.front().m_audioResourceId = "effect.ogg";
    bm.m_audioSamples.front().m_offsetMs        = -75;

    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_no_internal_meta.mc";
    TEST_ASSERT(bm.saveToFile(outPath),
                "map with internal metadata should save");

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("meta"), "output should contain meta");
    // 内部键不得进入 meta，但 sample.offset 必须从正式模型字段写出。
    TEST_ASSERT(!j["meta"].contains("initialDelay"),
                "initialDelay should not be exported to Malody meta");
    TEST_ASSERT(!j["meta"].contains("audioOffset"),
                "audioOffset should not be exported to Malody meta");
    const auto sampleIt =
        std::find_if(j["note"].begin(), j["note"].end(), isSoundNode);
    TEST_ASSERT(sampleIt != j["note"].end(),
                "explicit audio sample should remain present");
    TEST_ASSERT(sampleIt->value("offset", 0.0) == -75.0,
                "per-sample offset should not use legacy global metadata");

    XINFO("PASS: Internal offset metadata hidden from Malody meta");
}

/**
 * @brief 确认空难度名导出 MC 时会写出可显示的默认 version。
 *
 * Malody meta.version 是界面识别难度的必要文本。统一模型允许新建过程中
 * 暂时为空，保存器应使用 default 占位，同时继续保留 song 标题等元数据。
 */
void test_empty_version_exports_default_metadata()
{
    XINFO("=== Test: Empty Malody version exports default metadata ===");

    // 只清空 version，不改变其它必需字段，精确触发缺省名称分支。
    auto bm                      = makeMinimalBeatMap(7 /*Slide*/, 4);
    bm.m_baseMapMetadata.version = "";
    bm.m_baseMapMetadata.title   = "EmptyVersionTitle";
    bm.m_baseMapMetadata.artist  = "EmptyVersionArtist";
    fs::path outPath             = std::filesystem::temp_directory_path() /
                                   "edge_empty_version_metadata.mc";
    TEST_ASSERT(bm.saveToFile(outPath), "empty version map should save");

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("meta"), "empty version output should contain meta");
    TEST_ASSERT(j["meta"].contains("version"),
                "empty version output should contain meta.version");
    // 默认难度名和歌曲信息分别检查，避免填充 version 时覆盖整个 meta。
    TEST_ASSERT(j["meta"].value("version", "") == "default",
                "empty version should export as default");
    TEST_ASSERT(j["meta"].contains("song"),
                "empty version output should keep song metadata");
    TEST_ASSERT(j["meta"]["song"].value("title", "") == "EmptyVersionTitle",
                "empty version output should keep title metadata");

    XINFO("PASS: Empty Malody version exports default metadata");
}

/**
 * @brief 确认 BGM 自动采样对象的 x 不参与玩家 key 数量推断。
 *
 * 输入明确声明四条 Key 轨，但 SOUND 使用绝对 x=8。玩家轨仍应保持 4，
 * BGM 轨数则需要覆盖绝对轨道 4..8，共五条。若先扫描所有 note[] 的 x
 * 再推断 key 数量，自动采样会错误把谱面扩成九键。
 *
 * 场景还验证 SOUND 进入 m_audioSamples 而非 m_allNotes，确保轨道推断与
 * 对象分类使用相同的类型边界。
 */
void test_sound_track_does_not_expand_key_count()
{
    XINFO("=== Test: SOUND track does not expand key count ===");

    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_sound_track.mc";

    // 手工构造输入 JSON，绕过保存器对合法轨道的规范化。
    json  fileData;
    auto& meta         = fileData["meta"];
    meta["id"]         = 0;
    meta["creator"]    = "Test";
    meta["background"] = "";
    meta["cover"]      = "";
    meta["version"]    = "4K";
    meta["preview"]    = 0;
    meta["mode"]       = 0;
    meta["aimode"]     = "";

    auto& song        = meta["song"];
    song["title"]     = "SoundColumn";
    song["artist"]    = "Test";
    song["titleorg"]  = "";
    song["artistorg"] = "";
    song["file"]      = "audio.ogg";
    song["bpm"]       = 120.0;

    // column 是玩家轨权威来源，bar_begin 不参与本场景的对象分类。
    meta["mode_ext"]["column"]    = 4;
    meta["mode_ext"]["bar_begin"] = 0;

    json timing;
    timing["beat"]   = json::array({ 0, 0, 1 });
    timing["bpm"]    = 120.0;
    timing["delay"]  = 0.0;
    fileData["time"] = json::array({ timing });

    // 玩家节点落在最后一条 Key 轨，证明四轨声明确实被正常消费。
    json gameNote;
    gameNote["beat"]   = json::array({ 1, 0, 1 });
    gameNote["column"] = 3;

    // 自动采样使用远端绝对轨道，但不能反向扩大玩家轨区域。
    json soundNote;
    soundNote["beat"]   = json::array({ 0, 0, 1 });
    soundNote["x"]      = 8;
    soundNote["type"]   = 1;
    soundNote["sound"]  = "audio.ogg";
    soundNote["offset"] = 0;
    soundNote["vol"]    = 0;

    fileData["note"] = json::array({ gameNote, soundNote });

    // 输入位于系统临时目录，不污染共享测试资源。
    std::ofstream ofs(outPath);
    TEST_ASSERT(ofs.good(), "should open temp Malody file");
    ofs << fileData.dump();
    ofs.close();

    // 加载并同步后分别检查玩家区与 BGM 区的派生计数。
    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outPath);
    reloaded.sync();

    // 玩家轨、BGM 轨、玩家对象和采样对象四项共同锁定分类结果。
    TEST_ASSERT(reloaded.m_baseMapMetadata.track_count == 4,
                "SOUND x should not expand key count");
    TEST_ASSERT(reloaded.m_baseMapMetadata.bgm_track_count == 5,
                "x=8 after four key tracks should require five BGM tracks");
    TEST_ASSERT(reloaded.m_allNotes.size() == 1,
                "SOUND object should not become a playable note");
    TEST_ASSERT(reloaded.m_audioSamples.size() == 1,
                "SOUND object should become one automatic sample");
    TEST_ASSERT(reloaded.m_audioSamples.front().m_track == 8,
                "automatic sample should keep its absolute track");

    XINFO("PASS: SOUND track is separate from key count");
}

/**
 * @brief 验证多个自动采样独立保留轨道、音量和有符号偏移。
 *
 * 输入同时包含一个玩家命中采样、三个自动 SOUND、两个 BGM 轨，以及
 * 同拍同轨的重叠自动采样。每个 SOUND 使用不同资源、偏移和增益，专门
 * 防止加载器按 beat 或 x 去重，也防止把某个 offset 提升为全局时间平移。
 *
 * 测试分三层断言：首次加载后的统一模型、规范导出的 JSON 字段，以及
 * 规范文件再次加载后的采样数量。玩家绑定始终独立于自动采样集合。
 *
 * 音量换算覆盖 -20、-65 和 +16 三种 Malody gain，对应内部 0.8、0.35
 * 与 1.16；这也证明正增益不会被错误夹到单位音量。
 */
void test_multiple_sound_objects_round_trip_without_global_shift()
{
    XINFO("=== Test: Multiple SOUND objects round trip independently ===");

    const fs::path sourcePath =
        std::filesystem::temp_directory_path() / "edge_multiple_sound.mc";
    const fs::path exportPath = std::filesystem::temp_directory_path() /
                                "edge_multiple_sound_export.mc";

    // 使用 Key 模式以便同时验证 column 玩家节点和 x 自动采样节点。
    json  fileData;
    auto& meta         = fileData["meta"];
    meta["id"]         = 0;
    meta["creator"]    = "Test";
    meta["background"] = "";
    meta["cover"]      = "";
    meta["version"]    = "4K";
    meta["preview"]    = 0;
    meta["mode"]       = 0;
    meta["aimode"]     = "";
    meta["mode_ext"]   = { { "column", 4 }, { "bar_begin", 0 } };
    meta["song"]       = { { "title", "MultipleSound" }, { "artist", "Test" },
                           { "titleorg", "" },           { "artistorg", "" },
                           { "file", "stem.ogg" },       { "bpm", 120.0 } };
    fileData["time"]   = json::array({ { { "beat", json::array({ 0, 0, 1 }) },
                                         { "bpm", 120.0 },
                                         { "delay", 0.0 } } });

    // 玩家 Note 虽含 sound/vol，但缺少自动采样 type，应保留为命中绑定。
    json playableNote{ { "beat", json::array({ 4, 0, 1 }) },
                       { "column", 2 },
                       { "sound", "hit.wav" },
                       { "vol", -35 } };
    // 第一条 SOUND 使用浮点 1.0，覆盖历史 JSON 数字形态。
    json earlyStem{ { "beat", json::array({ 1, 0, 1 }) },
                    { "type", 1.0 },
                    { "sound", "stem.ogg" },
                    { "offset", -125 },
                    { "x", 4 },
                    { "vol", -20 } };
    // 第二条使用字符串 SOUND、正 offset 和第二条 BGM 轨。
    json delayedEffect{ { "beat", json::array({ 2, 0, 1 }) },
                        { "type", "SOUND" },
                        { "sound", "effect.wav" },
                        { "offset", 250 },
                        { "x", 5 },
                        { "vol", -65 } };
    // 第三条与第二条同拍同轨，只靠资源和增益区分，禁止错误去重。
    json sameBeatLayer{ { "beat", json::array({ 2, 0, 1 }) },
                        { "type", 1 },
                        { "sound", "layer.wav" },
                        { "offset", 0 },
                        { "x", 5 },
                        { "vol", 16 } };
    fileData["note"] =
        json::array({ playableNote, earlyStem, delayedEffect, sameBeatLayer });

    std::ofstream source(sourcePath);
    TEST_ASSERT(source.good(), "should open multiple SOUND input");
    source << fileData.dump();
    source.close();

    // 首次加载检查历史混合形态能否全部进入当前统一模型。
    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(sourcePath);
    loaded.sync();
    TEST_ASSERT(loaded.m_timings.size() == 1,
                "multiple SOUND map should keep its timing");
    // SOUND offset 属于单个采样，绝不能移动全局 timing 或玩家 Note。
    TEST_ASSERT(std::abs(loaded.m_timings.front().m_timestamp) < 1e-6,
                "sample offset must not shift the timing timeline");
    TEST_ASSERT(loaded.m_allNotes.size() == 1,
                "automatic samples must not enter playable note list");
    TEST_ASSERT(
        std::abs(loaded.m_allNotes.front().get().m_timestamp - 2000.0) < 1e-6,
        "sample offset must not shift playable note timestamps");
    // 从玩家对象读取嵌套绑定，确认含 sound 不会改变对象分类。
    const auto playableBinding =
        loaded.m_allNotes.front().get().getSampleBinding();
    TEST_ASSERT(playableBinding.has_value(),
                "playable note should keep its hit sample binding");
    TEST_ASSERT(playableBinding->m_audioResourceId == "hit.wav",
                "playable note should keep its hit sample resource");
    TEST_ASSERT(std::abs(playableBinding->m_volume - 0.65F) < 1e-6F,
                "playable note should keep its hit sample volume");
    // 三条自动采样全保留，同时只推导出 x=4、5 两条 BGM 轨。
    TEST_ASSERT(loaded.m_audioSamples.size() == 3,
                "all automatic samples should load independently");
    TEST_ASSERT(loaded.m_baseMapMetadata.track_count == 4,
                "automatic samples must not expand playable track count");
    TEST_ASSERT(loaded.m_baseMapMetadata.bgm_track_count == 2,
                "x=4 and x=5 should create two BGM tracks");

    /// @brief 按音频资源标识查找已加载的自动采样对象。
    /// @param resourceId 输入 JSON 中唯一的 sound 路径。
    /// @return 对应采样地址；未找到时返回 nullptr 供断言报告。
    auto findSample =
        [&](const std::string& resourceId) -> const MMM::AudioSampleEvent* {
        const auto it =
            std::find_if(loaded.m_audioSamples.begin(),
                         loaded.m_audioSamples.end(),
                         [&](const MMM::AudioSampleEvent& sample) {
                             return sample.m_audioResourceId == resourceId;
                         });
        return it == loaded.m_audioSamples.end() ? nullptr : &*it;
    };

    // 负 offset 只提前 stem 的有效播放时刻，beat 锚点仍在 500ms。
    const MMM::AudioSampleEvent* stem = findSample("stem.ogg");
    TEST_ASSERT(stem != nullptr, "main stem sample should load");
    TEST_ASSERT(std::abs(stem->m_timestamp - 500.0) < 1e-6,
                "main stem beat should remain its anchor timestamp");
    TEST_ASSERT(stem->m_offsetMs == -125,
                "negative sample offset should be retained");
    TEST_ASSERT(std::abs(stem->effectiveTimestamp() - 375.0) < 1e-6,
                "negative offset should advance only that sample");
    TEST_ASSERT(stem->m_track == 4,
                "main stem should remain on first BGM track");
    TEST_ASSERT(std::abs(stem->m_volume - 0.8F) < 1e-6F,
                "main stem volume should be normalized");

    // 正 offset 只延后 effect，有效时间等于锚点加 250ms。
    const MMM::AudioSampleEvent* effect = findSample("effect.wav");
    TEST_ASSERT(effect != nullptr, "effect sample should load");
    TEST_ASSERT(std::abs(effect->m_timestamp - 1000.0) < 1e-6,
                "effect beat should remain its anchor timestamp");
    TEST_ASSERT(effect->m_offsetMs == 250,
                "positive sample offset should be retained");
    TEST_ASSERT(std::abs(effect->effectiveTimestamp() - 1250.0) < 1e-6,
                "positive offset should delay only that sample");
    TEST_ASSERT(effect->m_track == 5,
                "effect should remain on second BGM track");
    TEST_ASSERT(std::abs(effect->m_volume - 0.35F) < 1e-6F,
                "effect volume should be normalized");
    // 同拍 layer 的存在性证明对象没有按 timestamp 或 track 被合并。
    const MMM::AudioSampleEvent* layer = findSample("layer.wav");
    TEST_ASSERT(layer != nullptr, "same-beat sample must not be deduplicated");
    TEST_ASSERT(std::abs(layer->m_volume - 1.16F) < 1e-6F,
                "positive Malody gain should increase internal volume");

    // 规范回写应统一成数值 type=1，但保留每个采样的独立载荷。
    TEST_ASSERT(loaded.saveToFile(exportPath),
                "multiple automatic samples should export");
    std::ifstream exportedFile(exportPath);
    json          exported;
    exportedFile >> exported;
    size_t canonicalSampleCount = 0;
    // 逐个 SOUND 校验协议字段和增益反向换算，不依赖数组顺序。
    for ( const auto& node : exported["note"] ) {
        if ( !isSoundNode(node) ) continue;
        ++canonicalSampleCount;
        TEST_ASSERT(
            node["type"].is_number_integer() && node["type"].get<int>() == 1,
            "all exported automatic samples should use numeric type 1");
        TEST_ASSERT(node.contains("x"),
                    "all exported automatic samples should keep BGM track x");
        TEST_ASSERT(!node.contains("column"),
                    "automatic samples must not use playable column");
        const std::string sound = node.value("sound", "");
        if ( sound == "stem.ogg" ) {
            TEST_ASSERT(node.value("vol", 0) == -20,
                        "80% internal volume should export as -20 gain");
        } else if ( sound == "effect.wav" ) {
            TEST_ASSERT(node.value("vol", 0) == -65,
                        "35% internal volume should export as -65 gain");
        } else if ( sound == "layer.wav" ) {
            TEST_ASSERT(node.value("vol", 0) == 16,
                        "116% internal volume should export as +16 gain");
        }
    }
    TEST_ASSERT(canonicalSampleCount == 3,
                "export should preserve every automatic sample");
    // 玩家命中绑定仍是普通 Note，独立验证其 -35 gain。
    const auto playableOutput = std::find_if(
        exported["note"].begin(), exported["note"].end(), [](const json& node) {
            return node.value("sound", "") == "hit.wav";
        });
    TEST_ASSERT(playableOutput != exported["note"].end() &&
                    playableOutput->value("vol", 0) == -35,
                "65% bound-note volume should export as -35 gain");

    // 最后一层重载确保规范 type=1 输出仍能恢复全部自动采样。
    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(exportPath);
    TEST_ASSERT(reloaded.m_audioSamples.size() == 3,
                "canonical Malody output should reload all samples");

    XINFO("PASS: Multiple SOUND objects round trip independently");
}

/**
 * @brief 验证未与主采样配对的 delay 与逐采样 offset 独立往返。
 *
 * 输入包含三个 BPM timing，其中两个位于同一 beat 并按来源顺序累积 delay；
 * 两枚玩家 Note 分处延迟 timing 前后，一条自动采样与后者同拍但另有 -75ms
 * offset。该组合区分拍轴锚点变化与单对象播放偏移。
 *
 * 首次加载验证绝对毫秒值；随后删除导入时缓存的 beat，迫使保存器从当前
 * timestamp 和 delay 逆算拍号；最后再次加载，确认两种偏移没有互相吸收。
 */
void test_timing_delay_and_sample_offset_round_trip_independently()
{
    XINFO("=== Test: Timing delay and sample offset round trip ===");

    const fs::path sourcePath =
        std::filesystem::temp_directory_path() / "edge_timing_delay_source.mc";
    const fs::path exportPath =
        std::filesystem::temp_directory_path() / "edge_timing_delay_export.mc";

    // 手工输入保留同拍 timing 的来源顺序，覆盖顺序相关的 delay 累积。
    json fileData;
    fileData["meta"] = { { "id", 0 },
                         { "creator", "Test" },
                         { "version", "4K" },
                         { "mode", 0 },
                         { "mode_ext",
                           { { "column", 4 }, { "bar_begin", 0 } } },
                         { "song",
                           { { "title", "TimingDelay" },
                             { "artist", "Test" },
                             { "file", "stem.ogg" },
                             { "bpm", 120.0 } } } };
    // beat 4 上连续 200ms 与 50ms delay，第二锚点应比第一锚点晚 50ms。
    fileData["time"] = json::array({ { { "beat", json::array({ 0, 0, 1 }) },
                                       { "bpm", 120.0 },
                                       { "delay", 100.0 } },
                                     { { "beat", json::array({ 4, 0, 1 }) },
                                       { "bpm", 60.0 },
                                       { "delay", 200.0 } },
                                     { { "beat", json::array({ 4, 0, 1 }) },
                                       { "bpm", 60.0 },
                                       { "delay", 50.0 } } });
    // 玩家对象前后跨越延迟锚点，SOUND 与后一对象同拍但有独立负 offset。
    fileData["note"] =
        json::array({ { { "beat", json::array({ 3, 0, 1 }) }, { "column", 0 } },
                      { { "beat", json::array({ 5, 0, 1 }) }, { "column", 1 } },
                      { { "beat", json::array({ 5, 0, 1 }) },
                        { "type", 1 },
                        { "sound", "effect.wav" },
                        { "offset", -75 },
                        { "x", 4 },
                        { "vol", 80 } } });

    std::ofstream source(sourcePath);
    TEST_ASSERT(source.good(), "should open timing delay input");
    source << fileData.dump();
    source.close();

    // 首次加载按来源 beat/delay 建立绝对时间轴。
    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(sourcePath);
    loaded.sync();
    TEST_ASSERT(loaded.m_timings.size() == 3,
                "timing delay map should keep three BPM timings");
    TEST_ASSERT(std::abs(loaded.m_timings[0].m_timestamp - 100.0) < 1e-6,
                "unpaired first timing should keep its positive delay");
    TEST_ASSERT(std::abs(loaded.m_timings[1].m_timestamp - 2300.0) < 1e-6,
                "second timing should apply its 200ms delay once");
    TEST_ASSERT(std::abs(loaded.m_timings[2].m_timestamp - 2350.0) < 1e-6,
                "same-beat timings should accumulate delay in source order");
    TEST_ASSERT(loaded.m_noteData.notes.size() == 2,
                "timing delay map should keep two playable notes");

    // 按轨道查找两枚 Note，避免数组重排影响场景判断。
    const auto beforeDelay =
        std::find_if(loaded.m_noteData.notes.begin(),
                     loaded.m_noteData.notes.end(),
                     [](const MMM::Note& note) { return note.m_track == 0; });
    const auto afterDelay =
        std::find_if(loaded.m_noteData.notes.begin(),
                     loaded.m_noteData.notes.end(),
                     [](const MMM::Note& note) { return note.m_track == 1; });
    TEST_ASSERT(beforeDelay != loaded.m_noteData.notes.end(),
                "note before delayed timing should load");
    TEST_ASSERT(afterDelay != loaded.m_noteData.notes.end(),
                "note after delayed timing should load");
    // 延迟 timing 之前的 Note 不回溯移动，之后的 Note 使用新锚点。
    TEST_ASSERT(std::abs(beforeDelay->m_timestamp - 1600.0) < 1e-6,
                "later timing delay must not shift earlier notes");
    TEST_ASSERT(std::abs(afterDelay->m_timestamp - 3350.0) < 1e-6,
                "later notes should use the delayed BPM anchor");

    TEST_ASSERT(loaded.m_audioSamples.size() == 1,
                "timing delay map should keep one automatic sample");
    // sample.m_timestamp 是拍轴锚点，effectiveTimestamp 才包含 -75ms。
    const auto& sample = loaded.m_audioSamples.front();
    TEST_ASSERT(std::abs(sample.m_timestamp - 3350.0) < 1e-6,
                "sample anchor should follow only the timing delay");
    TEST_ASSERT(sample.m_offsetMs == -75,
                "sample should keep its independent signed offset");
    TEST_ASSERT(std::abs(sample.effectiveTimestamp() - 3275.0) < 1e-6,
                "sample offset should affect only effective playback time");

    // 强制导出器从内部时间戳重算 beat，覆盖 delay 的逆向换算路径。
    // 若不清除来源 beat，错误算法可能原样复制输入而让测试虚假通过。
    for ( auto& timing : loaded.m_timings ) {
        timing.m_metadata.timing_properties[MMM::TimingMetadataType::MALODY]
            .erase("beat");
    }
    for ( auto& note : loaded.m_noteData.notes ) {
        note.m_metadata.note_properties[MMM::NoteMetadataType::MALODY].erase(
            "beat");
    }
    loaded.m_audioSamples.front()
        .m_metadata.sample_properties[MMM::SampleMetadataType::MALODY]
        .erase("beat");

    TEST_ASSERT(loaded.saveToFile(exportPath),
                "timing delay map should export");
    std::ifstream exportedFile(exportPath);
    json          exported;
    exportedFile >> exported;

    // 初始化红线位于零点，原来的三个延迟红线保持来源顺序。
    TEST_ASSERT(exported["time"].size() == 4 &&
                    exported["time"][0]["beat"] == json::array({ 0, 0, 1 }) &&
                    exported["time"][1]["beat"] == json::array({ 0, 1, 5 }) &&
                    exported["time"][1].value("delay", 0.0) == 0.0 &&
                    exported["time"][2]["beat"] == json::array({ 4, 1, 5 }) &&
                    exported["time"][2].value("delay", 0.0) == 200.0 &&
                    exported["time"][3]["beat"] == json::array({ 4, 1, 5 }) &&
                    exported["time"][3].value("delay", 0.0) == 50.0,
                "initial reference anchor must preserve later timing delays");

    // 自动采样的 beat 回到 5，而 -75 仍留在节点 offset。
    const auto exportedSample = std::find_if(
        exported["note"].begin(), exported["note"].end(), isSoundNode);
    TEST_ASSERT(exportedSample != exported["note"].end(),
                "export should keep the automatic sample");
    TEST_ASSERT((*exportedSample)["beat"] == json::array({ 5, 1, 5 }),
                "sample anchor should convert back to beat 5");
    TEST_ASSERT(exportedSample->value("offset", 0) == -75,
                "sample offset should survive export independently");

    // 规范输出再次加载后，绝对时间应与首次加载完全一致。
    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(exportPath);
    reloaded.sync();
    TEST_ASSERT(reloaded.m_timings.size() == 5,
                "round trip should keep three timings");
    TEST_ASSERT(std::abs(reloaded.m_timings[2].m_timestamp - 100.0) < 1e-6,
                "round trip should keep the unpaired first timing");
    TEST_ASSERT(std::abs(reloaded.m_timings[3].m_timestamp - 2300.0) < 1e-6,
                "round trip should keep delayed timing timestamp");
    TEST_ASSERT(std::abs(reloaded.m_timings[4].m_timestamp - 2350.0) < 1e-6,
                "round trip should keep same-beat timing order");
    TEST_ASSERT(reloaded.m_noteData.notes.size() == 2,
                "round trip should keep both playable notes");
    const auto reloadedBeforeDelay =
        std::find_if(reloaded.m_noteData.notes.begin(),
                     reloaded.m_noteData.notes.end(),
                     [](const MMM::Note& note) { return note.m_track == 0; });
    const auto reloadedAfterDelay =
        std::find_if(reloaded.m_noteData.notes.begin(),
                     reloaded.m_noteData.notes.end(),
                     [](const MMM::Note& note) { return note.m_track == 1; });
    TEST_ASSERT(reloadedBeforeDelay != reloaded.m_noteData.notes.end() &&
                    std::abs(reloadedBeforeDelay->m_timestamp - 1600.0) < 1e-6,
                "round trip must not shift the note before delayed timing");
    TEST_ASSERT(reloadedAfterDelay != reloaded.m_noteData.notes.end() &&
                    std::abs(reloadedAfterDelay->m_timestamp - 3350.0) < 1e-6,
                "round trip should keep the note after delayed timing");
    TEST_ASSERT(reloaded.m_audioSamples.size() == 1,
                "round trip should keep one automatic sample");
    TEST_ASSERT(
        std::abs(reloaded.m_audioSamples.front().m_timestamp - 3350.0) < 1e-6,
        "round trip should keep sample anchor separate from its offset");
    TEST_ASSERT(reloaded.m_audioSamples.front().m_offsetMs == -75,
                "round trip should keep signed sample offset");

    XINFO("PASS: Timing delay and sample offset round trip independently");
}

/**
 * @brief 验证非 Malody 来源的非零首红线保留原位并补参考零点。
 *
 * 首红线位于 237ms，主音频从零点播放。该相位不需要向前回卷，新的
 * 初始化红绿线位于绝对零点，原红线和同刻玩家 Note 仍位于 237ms。
 * 主 SOUND 不再借用旧首红线 delay，锚点和 offset 均保持零点。
 *
 * Key 和 Slide 执行同一规则，只改变玩家字段。来源模型没有 Malody
 * beat 缓存，因此结果只能来自当前毫秒时间线，而不是照抄历史 JSON。
 * 拍位采用有限分母拟合，原红线残余 delay 保留精确毫秒相位。
 */
void test_non_malody_lead_in_exports_timing_origin_and_audio_compensation()
{
    XINFO("=== Test: Non-Malody lead-in exports paired note phase ===");

    // 237ms 位于首拍前半段，不回卷；仅补参考零点，不移动原红线。
    constexpr double LEAD_IN_MS = 237.0;
    for ( const int mode : { 0, 7 } ) {
        // 同一绝对模型分别经过 Key 与 Slide 保存分支。
        auto beatMap                          = makeMinimalBeatMap(mode, 4);
        beatMap.m_timings.front().m_timestamp = LEAD_IN_MS;

        // 玩家 Note 与首 timing 同时发生，用于观察 note[] 的整体相位补偿。
        MMM::Note& note  = beatMap.m_noteData.notes.emplace_back();
        note.m_type      = MMM::NoteType::NOTE;
        note.m_timestamp = LEAD_IN_MS;
        note.m_track     = 0;
        beatMap.sync();

        // 模式名只用于隔离产物，不参与格式逻辑。
        const std::string modeName = mode == 0 ? "key" : "slide";
        const fs::path    outputPath =
            std::filesystem::temp_directory_path() /
            ("edge_non_malody_lead_in_" + modeName + ".mc");
        TEST_ASSERT(beatMap.saveToFile(outputPath),
                    "non-Malody lead-in map should save");

        std::ifstream outputFile(outputPath);
        json          exported;
        outputFile >> exported;

        // 新增参考红线位于零点，原红线保留 237ms。
        TEST_ASSERT(exported["time"].size() == 2,
                    "lead-in export must keep original red and reference zero");
        TEST_ASSERT(exported["time"][0]["beat"] == json::array({ 0, 0, 1 }),
                    "paired first timing should remain at beat zero");
        TEST_ASSERT(
            std::abs(exported["time"][0].value("delay", 0.0) - 0.0) < 1e-6,
            "first timing should wrap the main-audio phase");

        // helper 的主音频应成为与首 timing 成对的 beat 0 SOUND。
        const auto sampleIt = std::find_if(
            exported["note"].begin(), exported["note"].end(), isSoundNode);
        TEST_ASSERT(sampleIt != exported["note"].end(),
                    "lead-in export should keep the main sample");
        TEST_ASSERT((*sampleIt)["beat"] == json::array({ 0, 0, 1 }),
                    "main sample should move back one beat");
        TEST_ASSERT(
            sampleIt->value("offset", -1) == 0,
            "first-half-beat red line should pair main offset with delay");

        // 玩家对象与原红线共用拍位，零点参考段不改变二者绝对时间。
        const auto noteIt =
            std::find_if(exported["note"].begin(),
                         exported["note"].end(),
                         [](const json& node) { return !isSoundNode(node); });
        TEST_ASSERT(noteIt != exported["note"].end(),
                    "lead-in export should keep the playable note");
        TEST_ASSERT((*noteIt)["beat"] == exported["time"][1]["beat"],
                    "playable note must remain aligned with original red");

        // 重载同时验证 timing、玩家 Note 和主音频有效播放时刻。
        MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outputPath);
        reloaded.sync();
        TEST_ASSERT(reloaded.m_timings.size() == 3 &&
                        std::abs(reloaded.m_timings.back().m_timestamp -
                                 LEAD_IN_MS) < 1e-6,
                    "round trip should keep the first timing timestamp");
        TEST_ASSERT(reloaded.m_noteData.notes.size() == 1 &&
                        std::abs(reloaded.m_noteData.notes.front().m_timestamp -
                                 LEAD_IN_MS) < 1e-6,
                    "round trip should keep the playable note timestamp");
        TEST_ASSERT(
            reloaded.m_audioSamples.size() == 1 &&
                std::abs(reloaded.m_audioSamples.front().effectiveTimestamp()) <
                    1e-6,
            "round trip should keep audio playback at time zero");
    }

    XINFO("PASS: Non-Malody lead-in uses timing delay and sample compensation");
}

/// @brief 验证晚到红线回卷后主音频仍补偿完整的首拍 delay。
/// @details 120 BPM 的 2649.5ms 红线对应 149.5ms 相位和 350.5ms delay。
///
/// 原时间包含五个完整拍与 149.5ms 拍内相位。导出器为了保留原红线，
/// 先生成 beat 0 BPM 锚点，再把原 BPM 放在拍轴的后续位置。
/// 这个合成锚点必须与主 SOUND 的 offset 成对，否则游戏中的音乐零点
/// 会被首拍 delay 推迟。输出 offset 只支持整数毫秒，因此 350.5ms
/// 按当前舍入规则写成 351ms，误差由加载器的半毫秒配对容差吸收。
/// 重载检查同时覆盖合成红线、原红线和规范化的歌曲零点，防止只改动
/// JSON 数值却使编辑器内的播放时间线发生整拍漂移。
void test_late_first_timing_keeps_paired_main_offset()
{
    // 使用最小 Slide 谱面保留主 SOUND 与首 BPM 的配对条件。
    auto beatMap                          = makeMinimalBeatMap(7, 4);
    beatMap.m_timings.front().m_timestamp = 2649.5;
    beatMap.sync();

    const fs::path outputPath = std::filesystem::temp_directory_path() /
                                "edge_late_paired_main_offset.mc";
    TEST_ASSERT(beatMap.saveToFile(outputPath),
                "late first timing should export");
    std::ifstream stream(outputPath);
    json          exported;
    stream >> exported;
    const auto sample = std::find_if(
        exported["note"].begin(), exported["note"].end(), isSoundNode);

    // 真实 BPM 位于合成锚点之后六拍：五拍前导加一拍相位进位。
    // 主 SOUND 的 offset 与 delay 仅有不可避免的半毫秒整数化误差。
    TEST_ASSERT(
        exported["time"].size() == 2 &&
            std::abs(exported["time"][0].value("delay", 0.0) - 350.5) < 1e-6 &&
            exported["time"][1]["beat"] == json::array({ 6, 0, 1 }) &&
            sample != exported["note"].end() &&
            sample->value("offset", 0) == 351,
        "late first timing should pair 350.5ms delay with 351ms SOUND offset");

    // 导入时去掉相位进位，两个红线分别落回 149.5ms 与 2649.5ms。
    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outputPath);
    reloaded.sync();
    TEST_ASSERT(
        reloaded.m_timings.size() == 3 &&
            std::abs(reloaded.m_timings[0].m_timestamp - 149.5) < 1e-6 &&
            std::abs(reloaded.m_timings[2].m_timestamp - 2649.5) < 1e-6 &&
            reloaded.m_audioSamples.size() == 1 &&
            reloaded.m_audioSamples.front().effectiveTimestamp() == 0.0,
        "paired offset should preserve the red lines and audio origin");
}

/**
 * @brief 验证半拍后相位统一补偿，且晚首红线通过合成锚点保留。
 *
 * 本场景包含两个子情况。第一条红线仍在首拍内但已越过半拍时，负相位必须
 * 由新增 timing 承载，原红线继续与同拍 Scroll 对齐；普通 Note 与 effect
 * 的 beat 整体前移。第二条红线位于第三拍以后时，beat 0 同样增加合成 BPM
 * 锚点，原红线、后续 BPM、effect 和 Note 均保留各自相对位置。
 *
 * @par 首拍后半段导出不变量
 *
 * - 新增锚点只承载负相位 delay，不代替原首红线；
 * - 原首红线与同时间 Scroll 必须导出到相同拍位；
 * - 主 SOUND 继续绑定新增锚点，并用 offset 抵消 delay；
 * - 普通内容统一接受整拍补偿，不能只移动其中一种对象；
 * - 重载后新增锚点、原 BPM 和同拍 Scroll 都必须保留。
 *
 * 合成锚点只承载配对 delay，不能吞掉原始第一条红线，也不能让后续红线
 * 重复携带 delay。重新加载后，模型中应同时存在合成 timing 与全部原 timing。
 *
 * 常量选用 195 BPM 和非整拍相位，避免整数拍长掩盖浮点换算错误。
 */
void test_late_first_timing_prepends_anchor_and_shifts_all_content()
{
    XINFO("=== Test: Late first timing prepends paired anchor ===");

    // 先计算真实拍长，再由三拍加相位构造晚于首拍的原红线。
    constexpr double BPM                = 195.0;
    constexpr double BEAT_LENGTH_MS     = 60000.0 / BPM;
    constexpr double PHASE_MS           = 256.3076923076919;
    constexpr double FIRST_TIME_MS      = 3.0 * BEAT_LENGTH_MS + PHASE_MS;
    constexpr double CONTENT_TIME_MS    = FIRST_TIME_MS + BEAT_LENGTH_MS;
    constexpr double SECOND_BPM_TIME_MS = FIRST_TIME_MS + 2.0 * BEAT_LENGTH_MS;
    constexpr double EXPECTED_DELAY_MS  = BEAT_LENGTH_MS - PHASE_MS;

    // 子情况一：首 timing 在第一拍后半段，新增锚点但不移动原红线。
    auto withinFirstBeat                             = makeMinimalBeatMap(0, 4);
    withinFirstBeat.m_baseMapMetadata.preference_bpm = BPM;
    auto& withinFirstTiming         = withinFirstBeat.m_timings.front();
    withinFirstTiming.m_timestamp   = PHASE_MS;
    withinFirstTiming.m_bpm         = BPM;
    withinFirstTiming.m_beat_length = BEAT_LENGTH_MS;
    withinFirstTiming.m_timingEffectParameter = BPM;

    // 与首红线同拍放置 Scroll，直接锁定游戏内红绿线必须保持重合。
    MMM::Timing& withinAlignedScroll = withinFirstBeat.m_timings.emplace_back();
    withinAlignedScroll.m_timestamp  = PHASE_MS;
    withinAlignedScroll.m_bpm        = BPM;
    withinAlignedScroll.m_beat_length           = 1.1;
    withinAlignedScroll.m_timingEffect          = MMM::TimingEffect::SCROLL;
    withinAlignedScroll.m_timingEffectParameter = 1.1;

    // 同时刻之后一拍放置 SCROLL，检查 effect[] 与 note[] 接受相同相位位移。
    MMM::Timing& withinScroll   = withinFirstBeat.m_timings.emplace_back();
    withinScroll.m_timestamp    = PHASE_MS + BEAT_LENGTH_MS;
    withinScroll.m_bpm          = BPM;
    withinScroll.m_beat_length  = 1.25;
    withinScroll.m_timingEffect = MMM::TimingEffect::SCROLL;
    withinScroll.m_timingEffectParameter = 1.25;

    // 玩家 Note 与 SCROLL 同时发生，便于直接比较两个数组的导出 beat。
    MMM::Note& withinNote  = withinFirstBeat.m_noteData.notes.emplace_back();
    withinNote.m_type      = MMM::NoteType::NOTE;
    withinNote.m_timestamp = PHASE_MS + BEAT_LENGTH_MS;
    withinNote.m_track     = 0;
    withinFirstBeat.sync();

    const fs::path withinOutputPath = std::filesystem::temp_directory_path() /
                                      "edge_after_half_beat_first_timing.mc";
    TEST_ASSERT(withinFirstBeat.saveToFile(withinOutputPath),
                "after-half-beat first timing map should export");
    std::ifstream withinOutputFile(withinOutputPath);
    json          withinExported;
    withinOutputFile >> withinExported;
    // 从共享 note[] 分别定位主 SOUND 和玩家 Note，不依赖输出排序。
    const auto withinMainSample = std::find_if(withinExported["note"].begin(),
                                               withinExported["note"].end(),
                                               isSoundNode);
    const auto withinPlayable =
        std::find_if(withinExported["note"].begin(),
                     withinExported["note"].end(),
                     [](const json& node) { return !isSoundNode(node); });
    // 合成锚点位于 beat 0，原红线和同拍 Scroll 必须一起留在 beat 1。
    TEST_ASSERT(
        withinExported["time"].size() == 2 &&
            withinExported["time"][0]["beat"] == json::array({ 0, 0, 1 }) &&
            std::abs(withinExported["time"][0].value("delay", 0.0) -
                     EXPECTED_DELAY_MS) < 1e-6 &&
            withinExported["time"][1]["beat"] == json::array({ 1, 0, 1 }) &&
            !withinExported["time"][1].contains("delay") &&
            withinMainSample != withinExported["note"].end() &&
            (*withinMainSample)["beat"] == json::array({ 0, 0, 1 }) &&
            withinMainSample->value("offset", -1) ==
                static_cast<std::int64_t>(std::llround(EXPECTED_DELAY_MS)) &&
            withinPlayable != withinExported["note"].end() &&
            (*withinPlayable)["beat"] == json::array({ 2, 0, 1 }) &&
            withinExported["effect"].size() == 3 &&
            withinExported["effect"][1]["beat"] == json::array({ 1, 0, 1 }) &&
            withinExported["effect"][2]["beat"] == json::array({ 2, 0, 1 }),
        "phase after half a beat should prepend an anchor without moving the "
        "original BPM away from its aligned scroll");

    // 重载后 Note 与 SCROLL 都应回到 PHASE+一拍的相同绝对时间。
    MMM::BeatMap withinReloaded = MMM::BeatMap::loadFromFile(withinOutputPath);
    withinReloaded.sync();
    TEST_ASSERT(
        withinReloaded.m_noteData.notes.size() == 1 &&
            std::abs(withinReloaded.m_noteData.notes.front().m_timestamp -
                     (PHASE_MS + BEAT_LENGTH_MS)) < 1e-6 &&
            withinReloaded.m_timings.size() == 5 &&
            std::any_of(
                withinReloaded.m_timings.begin(),
                withinReloaded.m_timings.end(),
                [&](const MMM::Timing& timing) {
                    return timing.m_timingEffect == MMM::TimingEffect::BPM &&
                           std::abs(timing.m_timestamp - PHASE_MS) < 1e-6;
                }) &&
            std::any_of(
                withinReloaded.m_timings.begin(),
                withinReloaded.m_timings.end(),
                [&](const MMM::Timing& timing) {
                    return timing.m_timingEffect == MMM::TimingEffect::SCROLL &&
                           std::abs(timing.m_timestamp - PHASE_MS) < 1e-6;
                }),
        "after-half-beat BPM should remain aligned with its scroll after "
        "reload");

    // 子情况二：原首红线晚于第一拍，必须额外插入 beat 0 合成锚点。
    auto beatMap                             = makeMinimalBeatMap(0, 4);
    beatMap.m_baseMapMetadata.preference_bpm = BPM;
    auto& firstTiming                        = beatMap.m_timings.front();
    firstTiming.m_timestamp                  = FIRST_TIME_MS;
    firstTiming.m_bpm                        = BPM;
    firstTiming.m_beat_length                = BEAT_LENGTH_MS;
    firstTiming.m_timingEffectParameter      = BPM;

    // SCROLL 与玩家 Note 位于原首红线后一拍，二者应保持同步相位。
    MMM::Timing& scroll            = beatMap.m_timings.emplace_back();
    scroll.m_timestamp             = CONTENT_TIME_MS;
    scroll.m_bpm                   = BPM;
    scroll.m_beat_length           = 1.25;
    scroll.m_timingEffect          = MMM::TimingEffect::SCROLL;
    scroll.m_timingEffectParameter = 1.25;

    // 再加入后续 BPM，验证合成锚点不会把所有红线折叠成一条。
    MMM::Timing& secondBpm            = beatMap.m_timings.emplace_back();
    secondBpm.m_timestamp             = SECOND_BPM_TIME_MS;
    secondBpm.m_bpm                   = 180.0;
    secondBpm.m_beat_length           = 60000.0 / secondBpm.m_bpm;
    secondBpm.m_timingEffect          = MMM::TimingEffect::BPM;
    secondBpm.m_timingEffectParameter = secondBpm.m_bpm;

    MMM::Note& note  = beatMap.m_noteData.notes.emplace_back();
    note.m_type      = MMM::NoteType::NOTE;
    note.m_timestamp = CONTENT_TIME_MS;
    note.m_track     = 0;
    beatMap.sync();

    const fs::path outputPath = std::filesystem::temp_directory_path() /
                                "edge_late_first_timing_anchor.mc";
    TEST_ASSERT(beatMap.saveToFile(outputPath),
                "late first timing map should export");

    std::ifstream outputFile(outputPath);
    json          exported;
    outputFile >> exported;
    // 三条输出红线依次为回卷锚点、原首红线和后续 BPM。
    TEST_ASSERT(
        exported["time"].size() == 3 &&
            exported["time"][0]["beat"] == json::array({ 0, 0, 1 }) &&
            exported["time"][1]["beat"] == json::array({ 4, 0, 1 }) &&
            exported["time"][2]["beat"] == json::array({ 6, 0, 1 }),
        "late first timing should keep its original red line after the anchor");
    // 配对 delay 只属于第一条合成锚点，原首红线保持普通 BPM 语义。
    TEST_ASSERT(
        std::abs(exported["time"][0].value("delay", 0.0) - EXPECTED_DELAY_MS) <
                1e-6 &&
            !exported["time"][1].contains("delay"),
        "only the synthetic first-beat anchor should carry the paired delay");

    // 主 SOUND 绑定合成锚点；玩家 Note 和 effect 绑定原时间线相位。
    const auto mainSample = std::find_if(
        exported["note"].begin(), exported["note"].end(), isSoundNode);
    const auto playable = std::find_if(
        exported["note"].begin(), exported["note"].end(), [](const json& node) {
            return !isSoundNode(node);
        });
    TEST_ASSERT(
        mainSample != exported["note"].end() &&
            (*mainSample)["beat"] == json::array({ 0, 0, 1 }) &&
            mainSample->value("offset", -1) ==
                static_cast<std::int64_t>(std::llround(EXPECTED_DELAY_MS)),
        "main audio should pair the synthetic anchor with negative-phase "
        "offset");
    TEST_ASSERT(
        playable != exported["note"].end() &&
            (*playable)["beat"] == json::array({ 5, 0, 1 }) &&
            exported["effect"].size() == 2 &&
            exported["effect"][1]["beat"] == json::array({ 5, 0, 1 }),
        "phase after half a beat must shift notes and effects together");

    // 按效果种类和绝对时间查找，避免合成 timing 改变容器顺序后误判。
    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outputPath);
    reloaded.sync();
    /// @brief 检查重载模型在指定绝对时间是否保留目标 timing 类型。
    /// @param effect BPM 或 SCROLL 类型。
    /// @param timestamp 期望的统一毫秒时间。
    /// @return 容差内存在匹配 timing 时返回 true。
    const auto hasTimingAt = [&](MMM::TimingEffect effect, double timestamp) {
        return std::any_of(reloaded.m_timings.begin(),
                           reloaded.m_timings.end(),
                           [&](const MMM::Timing& timing) {
                               return timing.m_timingEffect == effect &&
                                      std::abs(timing.m_timestamp - timestamp) <
                                          1e-6;
                           });
    };
    TEST_ASSERT(
        hasTimingAt(MMM::TimingEffect::BPM, PHASE_MS) &&
            hasTimingAt(MMM::TimingEffect::BPM, FIRST_TIME_MS) &&
            hasTimingAt(MMM::TimingEffect::SCROLL, CONTENT_TIME_MS) &&
            hasTimingAt(MMM::TimingEffect::BPM, SECOND_BPM_TIME_MS),
        "synthetic and original timings should round trip independently");
    TEST_ASSERT(
        reloaded.m_noteData.notes.size() == 1 &&
            std::abs(reloaded.m_noteData.notes.front().m_timestamp -
                     CONTENT_TIME_MS) < 1e-6,
        "late-first-timing playable note should keep its absolute time");

    XINFO("PASS: Late first timing keeps original red line and adds anchor");
}

/**
 * @brief 验证 osu! 来源仅在参考零点新增单位 Scroll，后续 BPM 不重置滚速。
 *
 * 用户零点包含 38 BPM 与 3.14x Scroll，参考 BPM 为 120。按新规则腾出
 * 参考一拍，在零点加入初始化配对，原红绿线一起后移，3.14x 数值保留。
 * 第二条红线不能再产生额外一倍速，防止初始化规则扩散到所有 BPM。
 * 此处是纯模型协议测试，真实音频补头另由 Logic 集成测试覆盖。
 */
void test_osu_source_bpm_does_not_inject_scroll_reset()
{
    XINFO("=== Test: OSU source BPM does not inject scroll reset ===");
    auto             beatMap        = makeMinimalBeatMap(7, 4);
    constexpr double BPM            = 38.0;
    constexpr double BEAT_LENGTH_MS = 60000.0 / BPM;
    constexpr double SCROLL         = 3.14;

    // 来源标记只描述导入来源，不能授权导出器创建不存在的速度事件。
    beatMap.m_metadata.map_properties[MMM::MapMetadataType::OSU]["source"] =
        "regression";
    auto& firstBpm                   = beatMap.m_timings.front();
    firstBpm.m_bpm                   = BPM;
    firstBpm.m_beat_length           = BEAT_LENGTH_MS;
    firstBpm.m_timingEffectParameter = BPM;

    // 真实 3.14x 与首红线同拍，必须成为该拍唯一的 Scroll 输出。
    MMM::Timing& scroll            = beatMap.m_timings.emplace_back();
    scroll.m_timestamp             = 0.0;
    scroll.m_bpm                   = BPM;
    scroll.m_beat_length           = SCROLL;
    scroll.m_timingEffect          = MMM::TimingEffect::SCROLL;
    scroll.m_timingEffectParameter = SCROLL;

    // 第二条红线覆盖“每遇到 BPM 都重置”的旧分支，不能产生尾随 1.0x。
    MMM::Timing& secondBpm            = beatMap.m_timings.emplace_back();
    secondBpm.m_timestamp             = BEAT_LENGTH_MS;
    secondBpm.m_bpm                   = BPM;
    secondBpm.m_beat_length           = BEAT_LENGTH_MS;
    secondBpm.m_timingEffect          = MMM::TimingEffect::BPM;
    secondBpm.m_timingEffectParameter = BPM;
    beatMap.sync();

    const fs::path outputPath = std::filesystem::temp_directory_path() /
                                "edge_osu_bpm_without_scroll_reset.mc";
    TEST_ASSERT(beatMap.saveToFile(outputPath),
                "OSU source BPM and scroll map should export");
    std::ifstream outputFile(outputPath);
    json          exported;
    outputFile >> exported;

    // 只允许零点初始化的一倍速，原有 3.14x 后移一拍，后续 BPM 不再重置。
    TEST_ASSERT(
        exported.contains("effect") && exported["effect"].size() == 2 &&
            exported["effect"][0].value("scroll", 0.0) == 1.0 &&
            exported["effect"][1]["beat"] == json::array({ 1, 0, 1 }) &&
            std::abs(exported["effect"][1].value("scroll", 0.0) - SCROLL) <
                1e-9,
        "only initial anchor may add scroll=1 before the designed 3.14x");
    XINFO("PASS: OSU source BPM keeps only explicit scroll effects");
}

/**
 * @brief 验证历史配对相位在新参考零点规则下仍可读取和无损转换。
 *
 * 历史输入用非负 delay 和整数 SOUND offset 包装首红线。新输出允许在
 * 零点增加初始化红绿线，因此不再把“输出只有一条红线”作为正确性条件。
 * 判断依据是原红绿线、玩家 Note、Hold 和独立采样的绝对时间及持续时间。
 *
 * 此处保留历史格式、非匹配 offset、非 Main 资源、亚毫秒编辑、整拍编辑、
 * 大 delay 取模与 Key/Slide 长条的覆盖；新初始化配对由独立矩阵测试验证。
 */
void test_first_timing_delay_unwraps_with_its_bpm()
{
    constexpr double BPM              = 210.0;
    constexpr double WRAPPED_DELAY_MS = 237.032272;
    constexpr double BEAT_LENGTH_MS   = 60000.0 / BPM;
    constexpr double EXPECTED_TIME_MS = BEAT_LENGTH_MS - WRAPPED_DELAY_MS;
    const auto       root             = std::filesystem::temp_directory_path();
    const auto       sourcePath = root / "edge_wrapped_first_timing_source.mc";
    json             source{
        { "meta",
          { { "mode", 7 },
            { "mode_ext", { { "bar_begin", 0 } } },
            { "song", { { "file", "music.ogg" }, { "bpm", BPM } } } } },
        { "time",
          json::array({ { { "beat", json::array({ 0, 0, 1 }) },
                          { "bpm", BPM },
                          { "delay", WRAPPED_DELAY_MS } } }) },
        { "note",
          json::array({ { { "beat", json::array({ 1, 0, 1 }) },
                          { "x", 31 },
                          { "w", 60 } },
                        { { "beat", json::array({ 0, 0, 1 }) },
                          { "type", 1 },
                          { "sound", "music.ogg" },
                          { "offset", 237 } } }) }
    };
    /// @brief 写入历史夹具；每个来源单独落盘以覆盖真实加载入口。
    const auto writeSource = [&](const json& data) {
        std::ofstream stream(sourcePath);
        stream << data.dump();
        return stream.good();
    };
    TEST_ASSERT(writeSource(source), "historical paired source should write");
    auto loaded = MMM::BeatMap::loadFromFile(sourcePath);
    loaded.sync();
    TEST_ASSERT(loaded.m_timings.size() == 1 &&
                    std::abs(loaded.m_timings.front().m_timestamp -
                             EXPECTED_TIME_MS) < 1e-6 &&
                    loaded.m_noteData.notes.size() == 1 &&
                    std::abs(loaded.m_noteData.notes.front().m_timestamp -
                             EXPECTED_TIME_MS) < 1e-6 &&
                    loaded.m_audioSamples.size() == 1 &&
                    loaded.m_audioSamples.front().m_timestamp == 0 &&
                    loaded.m_audioSamples.front().m_offsetMs == 0,
                "historical delay must unwrap with first BPM and normalize "
                "only paired Main");

    /// @brief 按绝对模型检查新输出，不依赖新增初始化事件改变后的数组索引。
    /// @param map 来源模型，只读比较；保存不得修改来源时序。
    /// @param tag 隔离输出文件名。
    /// @return 原有事件和自动采样在导出重载后仍保持各自时间时为真。
    const auto roundTrip = [&](MMM::BeatMap&      map,
                               const std::string& tag,
                               bool               canonicalMain = false) {
        map.sync();
        const auto path = root / ("edge_phase_" + tag + ".mc");
        if ( !map.saveToFile(path) ) return false;
        auto reloaded = MMM::BeatMap::loadFromFile(path);
        reloaded.sync();
        // 初始化允许增加红绿线，但原 timing 的类型、参数和时间均不得消失。
        for ( const auto& timing : map.m_timings ) {
            if ( !std::any_of(reloaded.m_timings.begin(),
                              reloaded.m_timings.end(),
                              [&](const MMM::Timing& candidate) {
                                  return candidate.m_timingEffect ==
                                             timing.m_timingEffect &&
                                         std::abs(
                                             candidate.m_timingEffectParameter -
                                             timing.m_timingEffectParameter) <
                                             1e-6 &&
                                         std::abs(candidate.m_timestamp -
                                                  timing.m_timestamp) < 1e-6;
                              }) )
                return false;
        }
        if ( map.m_noteData.notes.size() != reloaded.m_noteData.notes.size() ||
             map.m_noteData.holds.size() != reloaded.m_noteData.holds.size() ||
             map.m_audioSamples.size() != reloaded.m_audioSamples.size() )
            return false;
        // 按轨道找物件，排序变化不应掩盖 Note 或 Hold 时间漂移。
        for ( const auto& note : map.m_noteData.notes ) {
            if ( !std::any_of(reloaded.m_noteData.notes.begin(),
                              reloaded.m_noteData.notes.end(),
                              [&](const MMM::Note& candidate) {
                                  return candidate.m_track == note.m_track &&
                                         std::abs(candidate.m_timestamp -
                                                  note.m_timestamp) < .2;
                              }) )
                return false;
        }
        for ( const auto& hold : map.m_noteData.holds ) {
            if ( !std::any_of(reloaded.m_noteData.holds.begin(),
                              reloaded.m_noteData.holds.end(),
                              [&](const MMM::Hold& candidate) {
                                  return candidate.m_track == hold.m_track &&
                                         std::abs(candidate.m_timestamp -
                                                  hold.m_timestamp) < .2 &&
                                         std::abs(candidate.m_duration -
                                                  hold.m_duration) < .2;
                              }) )
                return false;
        }
        // 主音频与普通音效各自比较有效播放时刻，不把独立 offset 当成全局偏移。
        for ( const auto& sample : map.m_audioSamples ) {
            if ( !std::any_of(
                     reloaded.m_audioSamples.begin(),
                     reloaded.m_audioSamples.end(),
                     [&](const MMM::AudioSampleEvent& candidate) {
                         return candidate.m_audioResourceId ==
                                    sample.m_audioResourceId &&
                                std::abs(candidate.effectiveTimestamp() -
                                         (canonicalMain &&
                                                  sample.m_audioResourceId ==
                                                      "music.ogg"
                                              ? 0
                                              : sample.effectiveTimestamp())) <
                                    .51;
                     }) )
                return false;
        }
        return true;
    };
    TEST_ASSERT(
        roundTrip(loaded, "base"),
        "paired input must retain absolute time after reference prefix");
    // 较小 delay 在后半拍触发旧回卷，较大 delay 则要先取非负模数。
    for ( const double delay : { 100.0, 1083.54 } ) {
        auto data                = source;
        data["time"][0]["delay"] = delay;
        data["note"][1]["offset"] =
            static_cast<std::int64_t>(std::llround(delay));
        TEST_ASSERT(writeSource(data), "modulo source should write");
        auto         map      = MMM::BeatMap::loadFromFile(sourcePath);
        const double expected = std::fmod(
            BEAT_LENGTH_MS - std::fmod(delay, BEAT_LENGTH_MS), BEAT_LENGTH_MS);
        TEST_ASSERT(
            map.m_timings.size() == 1 &&
                std::abs(map.m_timings.front().m_timestamp - expected) < 1e-6 &&
                map.m_audioSamples.front().m_offsetMs == 0 &&
                roundTrip(map, "modulo_" + std::to_string(delay)),
            "paired delay must use Euclidean modulo and preserve round-trip "
            "timing");
    }
    // 相同文件名但 offset 不匹配，或 offset 相同但文件非 Main，均不得配对。
    for ( const bool wrongResource : { false, true } ) {
        auto data                 = source;
        data["note"][1]["offset"] = wrongResource ? 237 : 200;
        if ( wrongResource ) data["note"][1]["sound"] = "effect.ogg";
        TEST_ASSERT(writeSource(data), "unpaired source should write");
        auto map = MMM::BeatMap::loadFromFile(sourcePath);
        TEST_ASSERT(map.m_audioSamples.size() == 1 &&
                        map.m_audioSamples.front().m_offsetMs ==
                            (wrongResource ? 237 : 200),
                    "unpaired offset must remain independent of first timing");
    }
    TEST_ASSERT(writeSource(source), "base source should restore");
    // 首 timing 的小数编辑和整拍编辑都必须覆盖导入时缓存的 beat/delay。
    for ( const double delta : { .4, -BEAT_LENGTH_MS, BEAT_LENGTH_MS } ) {
        auto map = MMM::BeatMap::loadFromFile(sourcePath);
        map.m_timings.front().m_timestamp += delta;
        TEST_ASSERT(roundTrip(map, "timing_edit_" + std::to_string(delta)),
                    "timing edits must replace imported phase metadata");
    }
    // Main 离开零点之后按普通 SOUND 处理，不可继续复用配对 offset。
    for ( const double timestamp : { .4, 20.0 } ) {
        auto map = MMM::BeatMap::loadFromFile(sourcePath);
        map.m_audioSamples.front().m_timestamp = timestamp;
        TEST_ASSERT(roundTrip(map, "main_edit_" + std::to_string(timestamp)),
                    "nonzero Main must preserve ordinary sample timing");
    }
    // 历史内部 anchor+负 offset 形态的误差来自整数毫秒 SOUND offset。
    auto legacy = MMM::BeatMap::loadFromFile(sourcePath);
    legacy.m_audioSamples.front().m_timestamp = EXPECTED_TIME_MS;
    legacy.m_audioSamples.front().m_offsetMs =
        static_cast<std::int64_t>(std::llround(237.0 - BEAT_LENGTH_MS));
    TEST_ASSERT(roundTrip(legacy, "legacy"),
                "legacy Main shape must preserve effective playback");
    // 历史 anchor+负 offset 在整拍移动后仍表示配对 Main，重载规范化到零点。
    // 原红线则必须保留移动后的绝对时间，不能被初始化锚点替代。
    for ( const double delta : { -BEAT_LENGTH_MS, BEAT_LENGTH_MS } ) {
        auto map = MMM::BeatMap::loadFromFile(sourcePath);
        map.m_timings.front().m_timestamp += delta;
        map.m_audioSamples.front().m_timestamp =
            map.m_timings.front().m_timestamp;
        map.m_audioSamples.front().m_offsetMs =
            static_cast<std::int64_t>(std::llround(237.0 - BEAT_LENGTH_MS));
        TEST_ASSERT(
            roundTrip(map, "legacy_whole_" + std::to_string(delta), true),
            "legacy whole-beat shapes must retain red line and canonical Main");
    }
    // 历史近零点形态会按旧协议规范化到正相位，继续检查其红线与 Note 对齐。
    // 这与普通模型负时间的保留规则不同，不能用整谱统一平移掩盖加载兼容性。
    auto nearZero = MMM::BeatMap::loadFromFile(sourcePath);
    nearZero.m_timings.front().m_timestamp = -.1;
    nearZero.m_timings.front()
        .m_metadata
        .timing_properties[MMM::TimingMetadataType::MALODY]["delay"] =
        json(BEAT_LENGTH_MS - .1).dump();
    nearZero.m_audioSamples.front().m_timestamp   = -.1;
    nearZero.m_noteData.notes.front().m_timestamp = -.1;
    nearZero.sync();
    const auto nearZeroPath = root / "edge_legacy_wrapped_zero_boundary.mc";
    TEST_ASSERT(nearZero.saveToFile(nearZeroPath),
                "legacy near-zero shape must export");
    auto canonical = MMM::BeatMap::loadFromFile(nearZeroPath);
    TEST_ASSERT(canonical.m_timings.size() == 3 &&
                    canonical.m_noteData.notes.size() == 1 &&
                    canonical.m_timings.front().m_timestamp > 0 &&
                    std::abs(canonical.m_noteData.notes.front().m_timestamp -
                             canonical.m_timings.front().m_timestamp) < 1e-6,
                "legacy near-zero shape must align Note, original red and "
                "synthetic anchor");
    // 两种模式都覆盖不同参考 BPM、显式 Scroll、普通音效和长条两端。
    for ( const int mode : { 0, 7 } ) {
        auto map                             = makeMinimalBeatMap(mode, 4);
        map.m_baseMapMetadata.preference_bpm = 123.0;
        auto& timing                         = map.m_timings.front();
        timing.m_timestamp                   = EXPECTED_TIME_MS;
        timing.m_bpm = timing.m_timingEffectParameter = BPM;
        timing.m_beat_length                          = BEAT_LENGTH_MS;
        auto scroll                                   = timing;
        scroll.m_timingEffect          = MMM::TimingEffect::SCROLL;
        scroll.m_timingEffectParameter = scroll.m_beat_length = 1.25;
        map.m_timings.push_back(scroll);
        auto& note               = map.m_noteData.notes.emplace_back();
        note.m_type              = MMM::NoteType::NOTE;
        note.m_timestamp         = EXPECTED_TIME_MS;
        auto& hold               = map.m_noteData.holds.emplace_back();
        hold.m_type              = MMM::NoteType::HOLD;
        hold.m_track             = 1;
        hold.m_timestamp         = EXPECTED_TIME_MS + BEAT_LENGTH_MS;
        hold.m_duration          = BEAT_LENGTH_MS;
        auto& sample             = map.m_audioSamples.emplace_back();
        sample.m_timestamp       = EXPECTED_TIME_MS;
        sample.m_audioResourceId = "effect.ogg";
        sample.m_track           = 5;
        TEST_ASSERT(roundTrip(map, "generated_" + std::to_string(mode)),
                    "generated Key and Slide must preserve complete timeline");
    }
}

/**
 * @brief 验证 note[] 拍号补偿在首拍相位边界仍保持绝对时间。
 *
 * 表驱动 case 覆盖首 timing 为负时间、恰好位于整拍，以及低 BPM 下先回卷
 * 到一拍内的前后半拍。两侧都由主 SOUND 的整数 offset 抵消首拍 delay；
 * 后半拍回退一拍成为绝对值小于半拍的负相位。
 *
 * 每个 case 同时跑 Key 和 Slide，证明相位算法不依赖玩家坐标协议。导出层
 * 检查 beat/delay/offset，重载层再按绝对毫秒查找原首 BPM 与玩家 Note。
 *
 * 边界用例特别防止取模结果把整拍错误表示为一拍 delay、把负时间直接
 * 写成 Malody 不接受的负拍号，或在长拍长下把半拍两侧的 offset 规则写反。
 */
void test_malody_note_phase_shift_boundaries()
{
    XINFO("=== Test: Playable note phase shift boundaries ===");

    // 每项同时给出 BPM、导出拍号、首 delay 和主 SOUND offset 四个预期。
    struct TestCase {
        double       bpm;
        double       firstTimingMs;
        double       expectedBeat;
        double       expectedDelayMs;
        std::int64_t expectedMainOffsetMs;
        const char*  tag;
    };
    // 60 BPM 用 1000ms 拍长直接锁定回卷后位于半拍两侧的行为。
    const std::array<TestCase, 4> cases{ {
        { 120.0, -100.0, 0, 100.0, 100, "negative_timing" },
        { 120.0, 500.0, 1, 0.0, 0, "zero_phase" },
        { 60.0, 1400.0, 1.8, 0.0, 0, "low_bpm_before_half" },
        { 60.0, 1600.0, 2.2, 0.0, 0, "low_bpm_after_half" },
    } };

    for ( const auto& testCase : cases ) {
        for ( const int mode : { 0, 7 } ) {
            // 两种模式复用完全相同的 timing、主音频和玩家绝对时间。
            auto  beatMap                       = makeMinimalBeatMap(mode, 4);
            auto& firstTiming                   = beatMap.m_timings.front();
            firstTiming.m_timestamp             = testCase.firstTimingMs;
            firstTiming.m_bpm                   = testCase.bpm;
            firstTiming.m_beat_length           = 60000.0 / testCase.bpm;
            firstTiming.m_timingEffectParameter = testCase.bpm;
            beatMap.m_audioSamples.front().m_timestamp = 0.0;
            beatMap.m_audioSamples.front().m_offsetMs  = 0;

            // Note 与首 timing 同时发生，直接观察导出原点的相位选择。
            MMM::Note& note  = beatMap.m_noteData.notes.emplace_back();
            note.m_type      = MMM::NoteType::NOTE;
            note.m_timestamp = testCase.firstTimingMs;
            note.m_track     = 0;
            beatMap.sync();

            const fs::path outputPath =
                std::filesystem::temp_directory_path() /
                (std::string("edge_playable_phase_boundary_") + testCase.tag +
                 (mode == 0 ? "_key.mc" : "_slide.mc"));
            TEST_ASSERT(beatMap.saveToFile(outputPath),
                        "phase boundary map should export");
            std::ifstream outputFile(outputPath);
            json          exported;
            outputFile >> exported;
            // note[] 中分别定位玩家对象和主 SOUND，不假定序列化顺序。
            const auto exportedPlayable = std::find_if(
                exported["note"].begin(),
                exported["note"].end(),
                [](const json& node) { return !isSoundNode(node); });
            const auto exportedMainSample = std::find_if(
                exported["note"].begin(), exported["note"].end(), isSoundNode);
            // 玩家 beat 与首 timing 的 delay/SOUND offset 必须作为一组匹配。
            TEST_ASSERT(
                exportedPlayable != exported["note"].end() &&
                    std::abs((*exportedPlayable)["beat"][0].get<double>() +
                             (*exportedPlayable)["beat"][1].get<double>() /
                                 (*exportedPlayable)["beat"][2].get<double>() -
                             testCase.expectedBeat) < 1e-6,
                "phase boundary should preserve the playable absolute time");
            TEST_ASSERT(
                !exported["time"].empty() &&
                    std::abs(exported["time"][0].value("delay", -1.0) -
                             testCase.expectedDelayMs) < 1e-6 &&
                    exportedMainSample != exported["note"].end() &&
                    exportedMainSample->value("offset", -1) ==
                        testCase.expectedMainOffsetMs,
                "first timing should export the expected delay and paired "
                "main SOUND offset");

            // 重载后允许存在合成 timing，因此按类型与绝对时间查找原 BPM。
            MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outputPath);
            reloaded.sync();
            const bool keptFirstTiming = std::any_of(
                reloaded.m_timings.begin(),
                reloaded.m_timings.end(),
                [&](const MMM::Timing& timing) {
                    return timing.m_timingEffect == MMM::TimingEffect::BPM &&
                           std::abs(timing.m_timestamp -
                                    testCase.firstTimingMs) < 1e-6;
                });
            TEST_ASSERT(
                keptFirstTiming && reloaded.m_noteData.notes.size() == 1 &&
                    std::abs(reloaded.m_noteData.notes.front().m_timestamp -
                             testCase.firstTimingMs) < 1e-6,
                "phase boundary timing and playable note should round trip");
        }
    }

    XINFO("PASS: Playable note phase shift boundaries");
}

/**
 * @brief 验证参考前导段不改变原有多 BPM 时间线的绝对位置。
 *
 * 210 BPM 首红线位于非整数相位，随后切到 200 BPM，再切回 210 BPM。
 * 新参考零点解除历史配对包装，原始三条 BPM 的时间仍必须精确保留。
 * 第三条红线与 Scroll 同刻，玩家 Note 与第二条红线同刻。
 *
 * 再将原第二、第三红线及 Scroll/Note 后移首 BPM 的一拍后重新导出。
 * 此时来源零点已有初始化绿线，按新规则再腾出参考一拍，原编辑量和
 * 必要的前导量分别核对，避免导入元数据缓存覆盖最新编辑。
 * Key 与 Slide 均执行，排除坐标协议对拍轴积分的影响。
 */
void test_paired_first_delay_round_trips_variable_bpm()
{
    XINFO("=== Test: Paired first delay round trips variable BPM ===");

    // 两种非整数拍长组合用于暴露错误使用单一 BPM 累计所有拍号的问题。
    constexpr double FIRST_BPM          = 210.0;
    constexpr double SECOND_BPM         = 200.0;
    constexpr double WRAPPED_DELAY_MS   = 237.032272;
    constexpr double FIRST_BEAT_MS      = 60000.0 / FIRST_BPM;
    constexpr double SECOND_BEAT_MS     = 60000.0 / SECOND_BPM;
    constexpr double FIRST_TIMESTAMP_MS = FIRST_BEAT_MS - WRAPPED_DELAY_MS;
    // 第二红线跨 100 个首 BPM 拍，第三红线再跨 10 个第二 BPM 拍。
    constexpr double SECOND_TIMESTAMP_MS =
        FIRST_TIMESTAMP_MS + 100.0 * FIRST_BEAT_MS;
    constexpr double THIRD_TIMESTAMP_MS =
        SECOND_TIMESTAMP_MS + 10.0 * SECOND_BEAT_MS;

    for ( const int mode : { 0, 7 } ) {
        // 主音频资源与 song_file_hint 同名并固定时间零，满足首拍配对条件。
        auto beatMap                             = makeMinimalBeatMap(mode, 4);
        beatMap.m_baseMapMetadata.preference_bpm = FIRST_BPM;
        beatMap.m_baseMapMetadata.song_file_hint = "music.ogg";
        beatMap.m_audioSamples.front().m_audioResourceId = "music.ogg";
        beatMap.m_audioSamples.front().m_timestamp       = 0.0;
        beatMap.m_audioSamples.front().m_offsetMs        = 0;
        auto& firstTiming                                = beatMap.m_timings[0];
        firstTiming.m_timestamp                          = FIRST_TIMESTAMP_MS;
        firstTiming.m_bpm                                = FIRST_BPM;
        firstTiming.m_beat_length                        = FIRST_BEAT_MS;
        firstTiming.m_timingEffectParameter              = FIRST_BPM;

        // 第二红线切到新 BPM，后续拍号换算必须从此处使用 200 BPM。
        MMM::Timing& secondTiming            = beatMap.m_timings.emplace_back();
        secondTiming.m_timestamp             = SECOND_TIMESTAMP_MS;
        secondTiming.m_bpm                   = SECOND_BPM;
        secondTiming.m_beat_length           = SECOND_BEAT_MS;
        secondTiming.m_timingEffect          = MMM::TimingEffect::BPM;
        secondTiming.m_timingEffectParameter = SECOND_BPM;

        // 第三红线切回首 BPM，但其绝对位置由第二 BPM 的十拍区间决定。
        MMM::Timing& thirdTiming            = beatMap.m_timings.emplace_back();
        thirdTiming.m_timestamp             = THIRD_TIMESTAMP_MS;
        thirdTiming.m_bpm                   = FIRST_BPM;
        thirdTiming.m_beat_length           = FIRST_BEAT_MS;
        thirdTiming.m_timingEffect          = MMM::TimingEffect::BPM;
        thirdTiming.m_timingEffectParameter = FIRST_BPM;

        // 同刻 SCROLL 写入 effect[]，应和第三 BPM 共享导出 beat。
        MMM::Timing& scroll            = beatMap.m_timings.emplace_back();
        scroll.m_timestamp             = THIRD_TIMESTAMP_MS;
        scroll.m_bpm                   = FIRST_BPM;
        scroll.m_beat_length           = 1.25;
        scroll.m_timingEffect          = MMM::TimingEffect::SCROLL;
        scroll.m_timingEffectParameter = 1.25;

        // Note 与第二红线同刻，便于比较 time[] 与 note[] 的相位一致性。
        MMM::Note& note  = beatMap.m_noteData.notes.emplace_back();
        note.m_type      = MMM::NoteType::NOTE;
        note.m_timestamp = SECOND_TIMESTAMP_MS;
        note.m_track     = 0;
        beatMap.sync();

        const std::string modeName = mode == 0 ? "key" : "slide";
        const fs::path    outputPath =
            std::filesystem::temp_directory_path() /
            ("edge_paired_variable_bpm_" + modeName + ".mc");
        TEST_ASSERT(beatMap.saveToFile(outputPath),
                    "paired variable BPM map should export");

        std::ifstream outputFile(outputPath);
        json          exported;
        outputFile >> exported;
        // 除原三条 BPM 外，还应保留新参考零点红线。
        TEST_ASSERT(exported["time"].size() == 4,
                    "variable BPM export should keep three timings");
        // 新参考段在零点初始化，原红线以拟合拍位和残余 delay 保留精确相位。
        TEST_ASSERT(exported["time"][0]["beat"] == json::array({ 0, 0, 1 }) &&
                        exported["time"][0].value("bpm", 0.0) == FIRST_BPM &&
                        exported["effect"].size() == 2 &&
                        exported["effect"][0].value("scroll", 0.0) == 1.0,
                    "reference zero must initialize BPM and unit scroll");

        // 重载统一模型按绝对时间验证分段 BPM 换算，而不是只比较拍号文本。
        MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outputPath);
        reloaded.sync();
        TEST_ASSERT(
            reloaded.m_timings.size() == 6,
            "variable BPM round trip should keep BPM and effect timings");
        TEST_ASSERT(
            std::abs(reloaded.m_timings[2].m_timestamp - FIRST_TIMESTAMP_MS) <
                    1e-6 &&
                std::abs(reloaded.m_timings[3].m_timestamp -
                         SECOND_TIMESTAMP_MS) < 1e-6 &&
                std::abs(reloaded.m_timings[4].m_timestamp -
                         THIRD_TIMESTAMP_MS) < 1e-6,
            "variable BPM round trip should preserve every timing timestamp");
        TEST_ASSERT(
            reloaded.m_audioSamples.size() == 1 &&
                std::abs(reloaded.m_audioSamples.front().m_timestamp) < 1e-6 &&
                reloaded.m_audioSamples.front().m_offsetMs == 0,
            "variable BPM round trip should keep main audio normalized");

        // 编辑阶段将除首 timing 外的内容整体后移一个首 BPM 拍长。
        // 这不是原始 beat 的简单复制，保存器必须检测 timestamp 已发生变化。
        reloaded.m_timings[3].m_timestamp += FIRST_BEAT_MS;
        reloaded.m_timings[4].m_timestamp += FIRST_BEAT_MS;
        reloaded.m_timings[5].m_timestamp += FIRST_BEAT_MS;
        reloaded.m_noteData.notes.front().m_timestamp += FIRST_BEAT_MS;
        reloaded.sync();
        const fs::path editedOutputPath =
            std::filesystem::temp_directory_path() /
            ("edge_paired_variable_bpm_edited_" + modeName + ".mc");
        TEST_ASSERT(reloaded.saveToFile(editedOutputPath),
                    "edited imported variable BPM map should export");
        std::ifstream editedOutputFile(editedOutputPath);
        json          editedExported;
        editedOutputFile >> editedExported;
        // 重新导出可能再次建立独立初始化配对，原 BPM 的时间以重载模型验证。
        TEST_ASSERT(
            !editedExported["time"].empty() &&
                editedExported["time"][0].value("bpm", 0.0) == FIRST_BPM,
            "edited output must retain reference BPM initialization");

        // 最终重载确认编辑值经历第二次往返后仍落在新的绝对时间。
        MMM::BeatMap editedReloaded =
            MMM::BeatMap::loadFromFile(editedOutputPath);
        editedReloaded.sync();
        TEST_ASSERT(
            std::abs(editedReloaded.m_timings[5].m_timestamp -
                     (SECOND_TIMESTAMP_MS + 2 * FIRST_BEAT_MS)) < 1e-6 &&
                std::abs(editedReloaded.m_timings[6].m_timestamp -
                         (THIRD_TIMESTAMP_MS + 2 * FIRST_BEAT_MS)) < 1e-6,
            "edited variable BPM timings should round trip at new positions");
        TEST_ASSERT(
            std::abs(editedReloaded.m_timings[7].m_timestamp -
                     (THIRD_TIMESTAMP_MS + 2 * FIRST_BEAT_MS)) < 1e-6 &&
                std::abs(editedReloaded.m_noteData.notes.front().m_timestamp -
                         (SECOND_TIMESTAMP_MS + 2 * FIRST_BEAT_MS)) < 1e-6,
            "edited effect and playable note should round trip at new "
            "positions");
    }

    XINFO("PASS: Paired first delay round trips variable BPM");
}

/**
 * @brief 验证缺少 x 的旧版自动采样按 Malody Pro Editor 规则展开。
 *
 * 旧版编辑器允许 SOUND 不写 x。兼容规则不把它们塞入首条 BGM 轨，而是
 * 从绝对轨道 10 开始按同一触发时间横向展开；触发时间变化后重新从 10
 * 开始。显式 x=7 的节点不参与自动布局，应原样保留。
 *
 * 三个无 x 节点分别形成 10、11、10，连同显式 7 使 BGM 轨声明覆盖到
 * 绝对轨道 11。整个自动布局只生成一条聚合诊断，避免每个节点刷屏。
 *
 * 规范回写会给所有 SOUND 补上 x；再次加载后按资源名检查各轨道，证明
 * 推断结果已经稳定持久化。
 */
void test_legacy_samples_without_x_use_pro_editor_tracks()
{
    XINFO("=== Test: Legacy samples without x use Pro Editor tracks ===");

    const fs::path sourcePath =
        std::filesystem::temp_directory_path() / "edge_legacy_sample_tracks.mc";
    const fs::path exportPath = std::filesystem::temp_directory_path() /
                                "edge_legacy_sample_tracks_export.mc";

    // Key 玩家轨固定为四条，使绝对轨道 10 对应第七条 BGM 轨。
    json fileData;
    fileData["meta"] = { { "id", 0 },
                         { "creator", "Test" },
                         { "version", "4K" },
                         { "mode", 0 },
                         { "mode_ext",
                           { { "column", 4 }, { "bar_begin", 0 } } },
                         { "song",
                           { { "title", "LegacySamples" },
                             { "artist", "Test" },
                             { "bpm", 120.0 } } } };
    fileData["time"] = json::array(
        { { { "beat", json::array({ 0, 0, 1 }) }, { "bpm", 120.0 } } });
    // 前两条同拍无 x，第三条换拍无 x，第四条同拍但显式 x=7。
    fileData["note"] = json::array({ { { "beat", json::array({ 1, 0, 1 }) },
                                       { "type", 1 },
                                       { "sound", "first.wav" } },
                                     { { "beat", json::array({ 1, 0, 1 }) },
                                       { "type", 1 },
                                       { "sound", "second.wav" } },
                                     { { "beat", json::array({ 2, 0, 1 }) },
                                       { "type", 1 },
                                       { "sound", "third.wav" } },
                                     { { "beat", json::array({ 2, 0, 1 }) },
                                       { "type", 1 },
                                       { "sound", "explicit.wav" },
                                       { "x", 7 } } });

    std::ofstream source(sourcePath);
    TEST_ASSERT(source.good(), "should open legacy sample input");
    source << fileData.dump();
    source.close();

    // 首次加载执行兼容布局，并保留显式轨道。
    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(sourcePath);
    loaded.sync();
    TEST_ASSERT(loaded.m_baseMapMetadata.track_count == 4,
                "automatic samples must not change playable key count");
    TEST_ASSERT(loaded.m_audioSamples.size() == 4,
                "all legacy samples should load");
    // 同拍并发采样占用连续轨，新拍点则可复用起始轨道 10。
    TEST_ASSERT(loaded.m_audioSamples[0].m_track == 10 &&
                    loaded.m_audioSamples[1].m_track == 11,
                "simultaneous legacy samples should expand from track 10");
    TEST_ASSERT(loaded.m_audioSamples[2].m_track == 10,
                "a new trigger time should reuse legacy track 10");
    TEST_ASSERT(loaded.m_audioSamples[3].m_track == 7,
                "an explicit valid x should remain unchanged");
    TEST_ASSERT(loaded.m_baseMapMetadata.bgm_track_count == 8,
                "absolute track 11 after four keys should retain eight BGM "
                "tracks including the gap");

    // 诊断按一次历史布局操作聚合，而不是按三个缺 x 节点重复报告。
    const auto relocationCount = std::count_if(
        loaded.m_loadDiagnostics.begin(),
        loaded.m_loadDiagnostics.end(),
        [](const MMM::BeatmapLoadDiagnostic& diagnostic) {
            return diagnostic.m_code ==
                   MMM::BeatmapLoadDiagnosticCode::AUDIO_SAMPLE_TRACK_RELOCATED;
        });
    TEST_ASSERT(relocationCount == 1,
                "legacy auto-layout should emit one aggregate diagnostic");

    // 规范导出把推断轨道固化为显式 x，后续无需再次运行历史布局。
    TEST_ASSERT(loaded.saveToFile(exportPath),
                "legacy auto-layout map should export");
    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(exportPath);
    reloaded.sync();
    /// @brief 根据资源名读取规范重载后的绝对轨道。
    /// @param sound 唯一的测试资源名。
    /// @return 对应轨道；缺失时返回 uint32 最大值供断言失败。
    const auto trackForSound = [&](const std::string& sound) {
        const auto sample =
            std::find_if(reloaded.m_audioSamples.begin(),
                         reloaded.m_audioSamples.end(),
                         [&](const MMM::AudioSampleEvent& candidate) {
                             return candidate.m_audioResourceId == sound;
                         });
        return sample == reloaded.m_audioSamples.end()
                   ? std::numeric_limits<std::uint32_t>::max()
                   : sample->m_track;
    };
    TEST_ASSERT(reloaded.m_audioSamples.size() == 4 &&
                    trackForSound("first.wav") == 10 &&
                    trackForSound("second.wav") == 11 &&
                    trackForSound("third.wav") == 10 &&
                    trackForSound("explicit.wav") == 7,
                "canonical x values should preserve the inferred layout");

    XINFO("PASS: Legacy samples without x use Pro Editor tracks");
}

/**
 * @brief 验证非法采样轨道归一化，且 song.file 不夺走同名玩家音效语义。
 *
 * 输入的玩家 Note 绑定 audio/shared.wav，而 song.file 使用同一路径；另一个
 * 自动 SOUND 使用 x=2，错误落在四条 Key 玩家轨内部。加载器必须把同名
 * song.file 保持为提示、保留玩家绑定，并只把真正的 SOUND 搬到绝对轨道 4。
 *
 * 搬移需要在 MALODY 元数据中留下 original_x=2，同时生成关联源谱面路径的
 * warning 诊断。规范导出应保留一个玩家节点和一个 SOUND，不因资源提示
 * 合成额外主音频，也不因文件名相同把玩家绑定消费掉。
 */
void test_invalid_sample_track_and_song_hint_conflict()
{
    XINFO("=== Test: Invalid sample x and song hint conflict ===");

    const fs::path sourcePath =
        std::filesystem::temp_directory_path() / "edge_invalid_sample_track.mc";
    const fs::path exportPath = std::filesystem::temp_directory_path() /
                                "edge_invalid_sample_track_export.mc";

    // song.file 与玩家命中 sound 故意相同，用于验证身份不由路径猜测。
    json fileData;
    fileData["meta"] = { { "id", 0 },
                         { "creator", "Test" },
                         { "version", "4K" },
                         { "mode", 0 },
                         { "mode_ext",
                           { { "column", 4 }, { "bar_begin", 0 } } },
                         { "song",
                           { { "title", "HintConflict" },
                             { "artist", "Test" },
                             { "file", "audio/shared.wav" },
                             { "bpm", 120.0 } } } };
    fileData["time"] = json::array(
        { { { "beat", json::array({ 0, 0, 1 }) }, { "bpm", 120.0 } } });
    // 普通 Note 以 type=0 显式标记，非法 SOUND 则使用 type=1 与 x=2。
    fileData["note"] = json::array({ { { "beat", json::array({ 1, 0, 1 }) },
                                       { "column", 0 },
                                       { "type", 0 },
                                       { "sound", "audio/shared.wav" },
                                       { "vol", -45 } },
                                     { { "beat", json::array({ 2, 0, 1 }) },
                                       { "type", 1 },
                                       { "sound", "effect.wav" },
                                       { "offset", -20 },
                                       { "x", 2 },
                                       { "vol", -10 } } });

    std::ofstream source(sourcePath);
    TEST_ASSERT(source.good(), "should open invalid sample input");
    source << fileData.dump();
    source.close();

    // 加载后分别检查提示、玩家绑定和自动采样三个独立职责。
    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(sourcePath);
    loaded.sync();
    TEST_ASSERT(
        loaded.m_baseMapMetadata.song_file_hint == fs::path("audio/shared.wav"),
        "song.file should remain a non-scheduling hint");
    TEST_ASSERT(loaded.m_allNotes.size() == 1,
                "same-name song hint should not consume playable note");
    // 资源路径相同不影响玩家命中绑定的音量和对象归属。
    const auto binding = loaded.m_allNotes.front().get().getSampleBinding();
    TEST_ASSERT(binding.has_value() &&
                    binding->m_audioResourceId == "audio/shared.wav" &&
                    std::abs(binding->m_volume - 0.55F) < 1e-6F,
                "same-name playable sound should remain an effect binding");
    TEST_ASSERT(loaded.m_audioSamples.size() == 1,
                "song hint must not synthesize an extra automatic sample");
    // 非法 x 搬到第一条 BGM 轨，原值只保留作诊断和人工修复参考。
    const auto& sample = loaded.m_audioSamples.front();
    TEST_ASSERT(sample.m_track == 4,
                "invalid x inside key area should use first BGM track");
    TEST_ASSERT(sample.m_metadata.getValue<std::string>(
                    MMM::SampleMetadataType::MALODY, "original_x") == "2",
                "invalid source x should remain available for diagnostics");
    // 使用结构化 code/severity 查找，不依赖日志文本内容。
    const auto relocationDiagnostic = std::find_if(
        loaded.m_loadDiagnostics.begin(),
        loaded.m_loadDiagnostics.end(),
        [](const MMM::BeatmapLoadDiagnostic& diagnostic) {
            return diagnostic.m_code == MMM::BeatmapLoadDiagnosticCode::
                                            AUDIO_SAMPLE_TRACK_RELOCATED &&
                   diagnostic.m_severity ==
                       MMM::BeatmapLoadDiagnosticSeverity::
                           BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_WARNING;
        });
    TEST_ASSERT(relocationDiagnostic != loaded.m_loadDiagnostics.end(),
                "invalid sample x should emit a non-fatal diagnostic");
    TEST_ASSERT(relocationDiagnostic->m_relatedPath ==
                    loaded.m_baseMapMetadata.map_path,
                "sample track diagnostic should identify its source map");

    // 保存规范化模型，检查路径提示和两类 note[] 节点仍各自唯一。
    TEST_ASSERT(loaded.saveToFile(exportPath),
                "normalized sample map should export");
    std::ifstream exportedFile(exportPath);
    json          exported;
    exportedFile >> exported;
    TEST_ASSERT(
        exported["meta"]["song"].value("file", "") == "audio/shared.wav",
        "explicit song.file hint should retain its relative path");

    size_t soundCount    = 0;
    size_t playableCount = 0;
    // 通过 isSoundNode 分类，确保玩家绑定含 sound 时仍不会误计为 SOUND。
    for ( const auto& node : exported["note"] ) {
        if ( isSoundNode(node) ) {
            ++soundCount;
            TEST_ASSERT(node.value("x", -1) == 4,
                        "normalized sample should export first BGM track");
            continue;
        }
        ++playableCount;
        TEST_ASSERT(!node.contains("type"),
                    "canonical playable Note must not contain type");
        TEST_ASSERT(node.value("sound", "") == "audio/shared.wav",
                    "playable effect should survive song hint conflict");
    }
    TEST_ASSERT(soundCount == 1 && playableCount == 1,
                "hint conflict output should keep one sample and one Note");

    XINFO("PASS: Invalid sample x and song hint conflict");
}

/**
 * @brief 验证移动自动采样锚点后不会被导入时缓存的 beat 覆盖。
 *
 * 输入 SOUND 位于 beat 1，即 500ms。加载后直接把统一模型 timestamp 改为
 * 1500ms，对应 beat 3，但不手工清除来源 metadata。保存器必须检测编辑后的
 * 时间是权威值，而不是无条件复用导入 beat=1。
 *
 * 原始 JSON 断言 beat 已变成 3，重载模型再断言绝对时间为 1500ms，覆盖
 * 编辑、保存和重新解析完整路径。
 */
void testEditedSampleTimestampOverridesImportedBeat()
{
    XINFO("=== Test: Edited sample timestamp overrides imported beat ===");

    const fs::path sourcePath =
        std::filesystem::temp_directory_path() / "edge_sample_move_source.mc";
    const fs::path exportPath =
        std::filesystem::temp_directory_path() / "edge_sample_move_export.mc";

    // 单一 120 BPM timing 让 1500ms 精确对应第三拍，避免量化歧义。
    json fileData;
    fileData["meta"] = { { "creator", "Test" },
                         { "version", "4K" },
                         { "mode", 0 },
                         { "mode_ext", { { "column", 4 } } },
                         { "song",
                           { { "title", "MoveSample" },
                             { "artist", "Test" },
                             { "file", "stem.ogg" },
                             { "bpm", 120.0 } } } };
    fileData["time"] = json::array(
        { { { "beat", json::array({ 0, 0, 1 }) }, { "bpm", 120.0 } } });
    fileData["note"] = json::array({ { { "beat", json::array({ 1, 0, 1 }) },
                                       { "type", 1 },
                                       { "sound", "stem.ogg" },
                                       { "offset", 0 },
                                       { "x", 4 },
                                       { "vol", 0 } } });

    std::ofstream source(sourcePath);
    TEST_ASSERT(source.good(), "should open moved sample input");
    source << fileData.dump();
    source.close();

    // 先确认唯一采样，再只修改 timestamp，保留其它导入元数据作为冲突来源。
    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(sourcePath);
    TEST_ASSERT(loaded.m_audioSamples.size() == 1,
                "moved sample fixture should load one sample");
    // 该赋值模拟用户在时间线拖动采样后的模型状态。
    loaded.m_audioSamples.front().m_timestamp = 1500.0;

    TEST_ASSERT(loaded.saveToFile(exportPath),
                "map with moved sample should export");
    std::ifstream exportedFile(exportPath);
    json          exported;
    exportedFile >> exported;
    // 输出层直接检查拍号，防止重载容错再次隐藏旧 beat。
    const auto sampleIt = std::find_if(
        exported["note"].begin(), exported["note"].end(), isSoundNode);
    TEST_ASSERT(sampleIt != exported["note"].end(),
                "moved sample should remain in output");
    TEST_ASSERT((*sampleIt)["beat"] == json::array({ 3, 0, 1 }),
                "exported beat should follow the edited sample timestamp");

    // 重载后绝对时间是最终业务语义，应与编辑值精确一致。
    const MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(exportPath);
    TEST_ASSERT(reloaded.m_audioSamples.size() == 1 &&
                    std::abs(reloaded.m_audioSamples.front().m_timestamp -
                             1500.0) < 1e-6,
                "moved sample timestamp should survive round trip");

    XINFO("PASS: Edited sample timestamp overrides imported beat");
}

/**
 * @brief 确认近空 Malody 谱面中的字符串 BPM 可以按数值语义加载。
 *
 * 一些 MC 生成器把 time[].bpm 写成 JSON 字符串而不是数字。夹具只含一个
 * 字符串 "234" timing、一枚 Slide Note 和一个缺 vol 的 SOUND，最大程度
 * 减少其它字段对解析结果的影响。
 *
 * 加载器应把字符串 BPM 转为 234，并同步更新 preference_bpm；SOUND 不得
 * 进入玩家物件集合，缺失增益则使用内部单位音量 1.0。该兼容不能依赖
 * meta.song.bpm，因为夹具故意省略该字段。
 *
 * 场景的目标是兼容类型差异，不允许吞掉格式错误后静默回退 120 BPM。
 */
void testStringBpmInNearlyEmptyMapLoads()
{
    XINFO("=== Test: Nearly empty Malody map with string BPM loads ===");

    const fs::path outPath = std::filesystem::temp_directory_path() /
                             "edge_nearly_empty_string_bpm.mc";

    // meta 只提供识别 Slide 模式和基本歌曲信息所需的最小字段。
    json  fileData;
    auto& meta             = fileData["meta"];
    meta["$ver"]           = 0;
    meta["creator"]        = "Test";
    meta["background"]     = "background.png";
    meta["version"]        = "4K HD";
    meta["id"]             = 0;
    meta["mode"]           = 7;
    meta["mode_ext"]       = json::object();
    meta["song"]["title"]  = "NearlyEmpty";
    meta["song"]["artist"] = "Test";

    // bpm 明确使用字符串，锁定兼容转换入口而不是普通数字解析。
    json timing;
    timing["beat"]     = json::array({ 0, 0, 1 });
    timing["bpm"]      = "234";
    fileData["time"]   = json::array({ timing });
    fileData["effect"] = json::array();

    // 玩家 Note 与 SOUND 同放 note[]，用于同步验证对象分类。
    json playableNote;
    playableNote["beat"] = json::array({ 0, 0, 4 });
    playableNote["x"]    = 32;

    // SOUND 省略 vol，期望加载器使用单位音量缺省值。
    json soundNote;
    soundNote["beat"]  = json::array({ 0, 0, 1 });
    soundNote["sound"] = "audio.mp3";
    soundNote["type"]  = 1;
    fileData["note"]   = json::array({ playableNote, soundNote });

    std::ofstream ofs(outPath);
    TEST_ASSERT(ofs.good(), "should open string BPM temp Malody file");
    ofs << fileData.dump();
    ofs.close();

    // 直接从手工 JSON 加载，避免保存器预先规范化字符串类型。
    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outPath);

    TEST_ASSERT(reloaded.m_timings.size() == 1,
                "string BPM map should have one timing");
    // timing BPM 与 preference BPM 均应采用解析值，不能只更新其中一个。
    TEST_ASSERT(reloaded.m_timings.front().m_bpm == 234.0,
                "string BPM should parse as 234");
    TEST_ASSERT(reloaded.m_baseMapMetadata.preference_bpm == 234.0,
                "string BPM should become preferred BPM");
    TEST_ASSERT(reloaded.m_allNotes.size() == 1,
                "SOUND node should not become a playable note");
    // 玩家/自动对象数量与缺省音量共同验证近空 note[] 的处理。
    TEST_ASSERT(
        reloaded.m_audioSamples.size() == 1 &&
            std::abs(reloaded.m_audioSamples.front().m_volume - 1.0F) < 1e-6F,
        "missing Malody gain should default to unit volume");

    XINFO("PASS: Nearly empty Malody map with string BPM loaded");
}

/**
 * @brief 确认只有元数据且缺少 time、effect、note 段的 MC 可加载。
 *
 * 项目创建或损坏恢复过程中可能先得到只有 meta 的文档。加载器应把缺段视为
 * 空集合，而不是直接索引不存在的数组；同时建立一条 120 BPM 默认 timing，
 * 并在 Slide 未声明轨数时使用四条玩家轨。
 *
 * 测试不要求生成任何玩家物件或自动采样，避免默认修复越权创造内容。
 * 默认 timing 只为后续拍号换算提供有效锚点。
 */
void testMetadataOnlyMapLoadsWithDefaults()
{
    XINFO("=== Test: Metadata-only Malody map loads with defaults ===");

    const fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_metadata_only.mc";

    // mode_ext 保持空对象，明确覆盖“字段存在但没有轨道子字段”的情况。
    json  fileData;
    auto& meta             = fileData["meta"];
    meta["$ver"]           = 0;
    meta["creator"]        = "Test";
    meta["version"]        = "Empty";
    meta["mode"]           = 7;
    meta["mode_ext"]       = json::object();
    meta["song"]["title"]  = "MetadataOnly";
    meta["song"]["artist"] = "Test";

    // 不创建 time、effect、note 键，区别于存在空数组的普通文件。
    std::ofstream ofs(outPath);
    TEST_ASSERT(ofs.good(), "should open metadata-only temp Malody file");
    ofs << fileData.dump();
    ofs.close();

    // 缺段加载不得抛异常，返回模型应具备最小可编辑时间轴。
    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outPath);

    // 空内容、默认 timing、默认 BPM 和默认轨数分别断言。
    TEST_ASSERT(reloaded.m_allNotes.empty(),
                "metadata-only map should have no notes");
    TEST_ASSERT(reloaded.m_timings.size() == 1,
                "metadata-only map should receive a default timing");
    TEST_ASSERT(reloaded.m_timings.front().m_bpm == 120.0,
                "metadata-only map should use the default BPM");
    TEST_ASSERT(reloaded.m_baseMapMetadata.track_count == 4,
                "metadata-only map should use four default tracks");

    XINFO("PASS: Metadata-only Malody map loaded with defaults");
}

/**
 * @brief 验证内部 original_structure 标记不会泄漏到导出 JSON。
 *
 * 加载器和折线转换可能使用 original_structure 与
 * original_structure_flick 保存来源形态，但它们不是 Malody 公共协议字段。
 * 本场景生成带 Hold 的 Slide Polyline，然后递归检查顶层 note 和每个 seg。
 *
 * 顶层与 seg 都检查是必要的：内部元数据可能挂在折线根，也可能在构建相对
 * 节点时被复制。发现泄漏会记录截断 JSON，方便定位具体输出节点。
 */
void test_original_structure_not_leaked()
{
    XINFO("=== Test: original_structure key not leaked into output ===");

    auto bm = makeMinimalBeatMap(7 /*Slide*/, 4);

    // 使用真实 Polyline 转换路径，让保存器确实构造 seg 数组。
    MMM::Polyline& poly = bm.m_noteData.polylines.emplace_back();
    poly.m_type         = MMM::NoteType::POLYLINE;
    poly.m_timestamp    = 1000.0;
    poly.m_track        = 0;

    MMM::Hold& h  = bm.m_noteData.holds.emplace_back();
    h.m_type      = MMM::NoteType::HOLD;
    h.m_timestamp = 1000.0;
    h.m_track     = 0;
    h.m_duration  = 500.0;
    h.m_isSubNote = true;
    poly.m_subNotes.push_back(h);
    poly.m_subHolds.push_back(h);

    bm.sync();
    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_no_leak.mc";
    bm.saveToFile(outPath);

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    // 逐层扫描公开输出，不能只在 meta 或根对象搜索字符串。
    bool leaked = false;
    for ( const auto& n : j["note"] ) {
        // 顶层键泄漏通常来自通用 metadata 的无筛选复制。
        if ( n.contains("original_structure") ||
             n.contains("original_structure_flick") ) {
            leaked = true;
            XERROR("LEAK: original_structure found in item: {}",
                   n.dump().substr(0, 120));
        }
        // seg 泄漏则说明相对节点构造保留了仅供内部重建的来源标记。
        if ( n.contains("seg") ) {
            for ( const auto& s : n["seg"] ) {
                if ( s.contains("original_structure") ||
                     s.contains("original_structure_flick") ) {
                    leaked = true;
                    XERROR("LEAK: original_structure in seg: {}",
                           s.dump().substr(0, 120));
                }
            }
        }
    }
    // 任意层级出现一个内部键都判为失败。
    TEST_ASSERT(!leaked, "original_structure should not appear in output JSON");

    XINFO("PASS: No original_structure leaked");
}

/**
 * @brief 验证 Slide Hold 在折线头部原地停留仍生成有效 seg。
 *
 * 横向位置不变不代表节点无效：只要 duration 大于零，它就表示在根位置持续
 * 按住。该场景与零长度 Hold 清理测试成对存在，确保保存器按时间持续判断，
 * 而不是仅凭横向位移是否为零过滤节点。
 *
 * 输出应只有一个游戏折线根，且 seg 为非空数组。具体拍号由统一时间转换
 * 决定，本场景只锁定结构有效性，精度由专门的分拍测试覆盖。
 */
void test_hold_stay_at_head_creates_valid_seg()
{
    XINFO("=== Test: Hold staying at head position → valid seg ===");

    auto bm = makeMinimalBeatMap(7 /*Slide*/, 4);

    MMM::Polyline& poly = bm.m_noteData.polylines.emplace_back();
    poly.m_type         = MMM::NoteType::POLYLINE;
    poly.m_timestamp    = 1000.0;
    poly.m_track        = 1;

    // 单段 Hold 与根同轨同刻，但通过非零 duration 提供可见的持续语义。
    // 单段 Hold 在 head 位置停留 0.5 秒
    MMM::Hold& h  = bm.m_noteData.holds.emplace_back();
    h.m_type      = MMM::NoteType::HOLD;
    h.m_timestamp = 1000.0;
    h.m_track     = 1;
    h.m_duration  = 125.0;  // 0.5 beats at 120bpm = 250ms → ~0.5 beats
    h.m_isSubNote = true;
    poly.m_subNotes.push_back(h);
    poly.m_subHolds.push_back(h);

    // 同步后保存器从折线容器读取通用与具体子节点引用。
    bm.sync();
    fs::path outPath =
        std::filesystem::temp_directory_path() / "edge_hold_stay.mc";
    bm.saveToFile(outPath);

    std::ifstream ifs(outPath);
    json          j;
    ifs >> j;

    TEST_ASSERT(j.contains("meta") && j["meta"].value("mode", -1) == 7,
                "mode should be 7");

    // 排除主 SOUND，最终只应剩下一个带 seg 的折线根。
    auto gameNotes = json::array();
    for ( const auto& n : j["note"] ) {
        if ( isSoundNode(n) ) continue;
        gameNotes.push_back(n);
    }
    TEST_ASSERT(gameNotes.size() == 1, "should have 1 game note");
    TEST_ASSERT(gameNotes[0].contains("seg"), "should have seg");
    TEST_ASSERT(gameNotes[0]["seg"].is_array(), "seg should be array");
    // 非空 seg 是“原地停留”没有被零位移规则误删的核心证据。
    TEST_ASSERT(gameNotes[0]["seg"].size() > 0,
                "seg should not be empty (has a valid hold)");

    XINFO("PASS: Hold stay produces valid seg with correct beat");
}

/**
 * @brief 验证 Malody 导出拒绝无法放入 seg 的子节点采样绑定。
 *
 * 统一模型允许任意玩家物件绑定命中采样，但 Malody Slide 的 seg 子节点没有
 * sound/vol 字段。若保存器只输出几何，会静默丢失用户设置；若把采样移到
 * 折线根，又会改变触发时刻。因此唯一无损行为是拒绝整个导出。
 *
 * 测试先删除旧目标，再同时检查 saveToFile 返回 false 和文件不存在，保证
 * 能力验证发生在写入前，不会截断用户已有文件。
 */
void testPolylineSubnoteSampleBindingRejected()
{
    XINFO("=== Test: Polyline sub-note sample binding is rejected ===");

    auto           bm   = makeMinimalBeatMap(7 /*Slide*/, 4);
    MMM::Polyline& poly = bm.m_noteData.polylines.emplace_back();
    poly.m_type         = MMM::NoteType::POLYLINE;
    poly.m_timestamp    = 1000.0;
    poly.m_track        = 1;

    // 使用有效的一拍 Hold，确保拒绝原因只来自子节点采样绑定。
    MMM::Hold& hold  = bm.m_noteData.holds.emplace_back();
    hold.m_type      = MMM::NoteType::HOLD;
    hold.m_timestamp = 1000.0;
    hold.m_track     = 1;
    hold.m_duration  = 500.0;
    hold.m_isSubNote = true;
    // 非默认音量也需要保留，进一步证明不能安全降级为无采样 seg。
    hold.setSampleBinding({ "segment.wav", 0.45F });
    poly.m_subNotes.push_back(hold);
    poly.m_subHolds.push_back(hold);
    bm.sync();

    const fs::path outputPath = std::filesystem::temp_directory_path() /
                                "edge_polyline_bound_subnote.mc";
    // 清理历史成功产物，避免存在性断言被上次执行污染。
    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);

    // 返回值和文件状态共同约束拒绝路径的事务性。
    TEST_ASSERT(!bm.saveToFile(outputPath),
                "Malody saver should reject a bound Polyline sub-note");
    TEST_ASSERT(!std::filesystem::exists(outputPath),
                "rejected Polyline export should not leave a partial file");
    XINFO("PASS: Bound Polyline sub-note rejected without partial file");
}

/**
 * @brief 验证 MC 时间线与相对 seg 共用固定高精度分拍候选。
 *
 * SCROLL 位于 1+1919/1920 拍，接近下一整数拍；Polyline Hold 持续
 * 287/288 拍。两个分母都属于固定候选集合，但一个用于 effect 的绝对 beat，
 * 另一个用于 seg 相对根节点的 beat。
 *
 * 若只保留常见 1/4、1/8、1/16，两个时间都会被错误吸附；若绝对和相对
 * 转换使用不同算法，则只有一项通过。精确数组比较同时锁定分子和分母。
 */
void testMalodyTimelineUsesFixedHighPrecisionFractions()
{
    XINFO("=== Test: Malody timeline fixed high precision fractions ===");
    auto             beatMap        = makeMinimalBeatMap(7 /*Slide*/, 4);
    constexpr double BEAT_LENGTH_MS = 500.0;

    // 1919/1920 逼近下一拍，用于验证不会因浮点误差进位到整数拍。
    MMM::Timing& scroll            = beatMap.m_timings.emplace_back();
    scroll.m_timestamp             = BEAT_LENGTH_MS * (1.0 + 1919.0 / 1920.0);
    scroll.m_bpm                   = 120.0;
    scroll.m_beat_length           = 1.25;
    scroll.m_timingEffect          = MMM::TimingEffect::SCROLL;
    scroll.m_timingEffectParameter = 1.25;

    // 折线根固定在时间零，使 Hold duration 可直接解释为相对 seg 拍号。
    MMM::Polyline& polyline = beatMap.m_noteData.polylines.emplace_back();
    polyline.m_type         = MMM::NoteType::POLYLINE;
    polyline.m_timestamp    = 0.0;
    polyline.m_track        = 0;
    // 287/288 是另一高精度候选，验证分母选择不是为 1920 特判。
    MMM::Hold& hold  = beatMap.m_noteData.holds.emplace_back();
    hold.m_type      = MMM::NoteType::HOLD;
    hold.m_timestamp = 0.0;
    hold.m_duration  = BEAT_LENGTH_MS * (287.0 / 288.0);
    hold.m_track     = 0;
    hold.m_isSubNote = true;
    polyline.m_subNotes.push_back(hold);
    polyline.m_subHolds.push_back(hold);
    beatMap.sync();

    const fs::path outputPath =
        std::filesystem::temp_directory_path() / "edge_malody_fraction_1920.mc";
    TEST_ASSERT(beatMap.saveToFile(outputPath),
                "high precision Malody map should export");
    std::ifstream outputFile(outputPath);
    json          exported;
    outputFile >> exported;

    // effect[] 直接检查绝对 beat 三元组，不允许近似候选替代。
    TEST_ASSERT(
        exported.contains("effect") && exported["effect"].size() == 2 &&
            exported["effect"][1]["beat"] == json::array({ 1, 1919, 1920 }),
        "timeline effect should preserve 1919/1920");
    // 从 note[] 排除主 SOUND 后定位折线根，再检查首个相对 seg。
    const auto gameNote = std::find_if(
        exported["note"].begin(), exported["note"].end(), [](const json& node) {
            return !isSoundNode(node);
        });
    // 相对 seg 必须复用固定候选集合并保留 287/288。
    TEST_ASSERT(
        gameNote != exported["note"].end() && gameNote->contains("seg") &&
            (*gameNote)["seg"].size() == 1 &&
            (*gameNote)["seg"][0]["beat"] == json::array({ 0, 287, 288 }),
        "relative seg should use the same fixed denominator candidates");
    XINFO("PASS: Malody timeline preserves fixed 1920 precision fractions");
}

/**
 * @brief 依次执行全部 Malody 边界场景并汇总失败数量。
 *
 * TEST_ASSERT 只结束当前 void 场景，因此后续测试仍会继续运行。执行顺序从
 * 局部物件字段逐渐扩展到时间轴相位，再以缺字段、内部键和精度场景收尾，
 * 日志可以保留一次运行中的多个独立失败。
 *
 * main 不接收资源路径；所有夹具均由场景写入系统临时目录。最终退出码是
 * 自动化判断的权威结果，个别预期拒绝产生的 error 日志不代表测试失败。
 *
 * @return g_failed 为零时返回 0，否则返回 1。
 */
int main()
{
    // 统一标题便于从完整构建日志中定位本测试的开始位置。
    XINFO("========================================");
    XINFO("  Malody Edge Case Tests");
    XINFO("========================================");

    // 第一组：折线清理、模式协议与保存能力边界。
    test_zero_length_hold_degrade_to_flick();
    test_multiple_zero_holds_same_flicks_merge();
    test_polyline_all_cleaned_degrade_to_note();
    testQuantizedStaircaseExportsRmSegments();
    testRmCornerCleanupRespectsContinuity();
    test_key_mode_hold_uses_endbeat();
    test_slide_mode_saves_xw();
    test_slide_mode_7k_8k_uses_skin_compatible_layout();
    test_unsupported_malody_mode_rejected();
    test_key_mode_polyline_exports_key_fields();
    test_key_mode_flick_exports_single_note();
    // 第二组：SOUND 类型、轨道、增益和内部 offset 元数据。
    testKeyAudioNodeUsesNumericType();
    testSlideAudioNodeUsesSoundType();
    test_internal_offset_metadata_not_exported();
    test_empty_version_exports_default_metadata();
    test_sound_track_does_not_expand_key_count();
    test_multiple_sound_objects_round_trip_without_global_shift();
    // 第三组：普通 delay、配对首拍相位、边界和多 BPM 编辑。
    test_timing_delay_and_sample_offset_round_trip_independently();
    test_non_malody_lead_in_exports_timing_origin_and_audio_compensation();
    test_late_first_timing_keeps_paired_main_offset();
    test_late_first_timing_prepends_anchor_and_shifts_all_content();
    test_osu_source_bpm_does_not_inject_scroll_reset();
    test_first_timing_delay_unwraps_with_its_bpm();
    test_malody_note_phase_shift_boundaries();
    test_paired_first_delay_round_trips_variable_bpm();
    // 第四组：历史轨道推断、非法轨道迁移与编辑后缓存失效。
    test_legacy_samples_without_x_use_pro_editor_tracks();
    test_invalid_sample_track_and_song_hint_conflict();
    testEditedSampleTimestampOverridesImportedBeat();
    // 第五组：缺字段默认值、内部键过滤、拒绝事务性和分拍精度。
    testStringBpmInNearlyEmptyMapLoads();
    testMetadataOnlyMapLoadsWithDefaults();
    test_original_structure_not_leaked();
    test_hold_stay_at_head_creates_valid_seg();
    testPolylineSubnoteSampleBindingRejected();
    testMalodyTimelineUsesFixedHighPrecisionFractions();

    XINFO("========================================");
    // 只以累计断言结果决定进程状态，保持与 CTest 的退出码约定一致。
    if ( g_failed == 0 ) {
        XINFO("  ALL Malody Edge Case Tests PASSED");
        return 0;
    } else {
        XERROR("  {} Malody Edge Case Tests FAILED", g_failed);
        return 1;
    }
}
