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
 * 保存器随后需要恢复等价的非负 delay 和主 SOUND 偏移，保证 Malody 播放一致。
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
 * - 主 SOUND 与首 timing 建立配对相位；
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
 * - 玩家 Note 拍号按边界分别为 0 或 1；
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

    // 三个 timing 的 beat 与 delay 分开断言，保持同拍来源顺序。
    TEST_ASSERT(exported["time"].size() == 3,
                "export should keep three BPM timings");
    TEST_ASSERT(exported["time"][0]["beat"] == json::array({ 0, 0, 1 }),
                "first delayed timing should convert back to beat 0");
    TEST_ASSERT(exported["time"][0].value("delay", 0.0) == 100.0,
                "first timing delay should survive export");
    TEST_ASSERT(exported["time"][1]["beat"] == json::array({ 4, 0, 1 }),
                "delayed timing timestamp should convert back to beat 4");
    TEST_ASSERT(exported["time"][1].value("delay", 0.0) == 200.0,
                "timing delay should survive export");
    TEST_ASSERT(exported["time"][2]["beat"] == json::array({ 4, 0, 1 }),
                "same-beat timing should convert back to beat 4");
    TEST_ASSERT(exported["time"][2].value("delay", 0.0) == 50.0,
                "same-beat timing delay should survive export");

    // 自动采样的 beat 回到 5，而 -75 仍留在节点 offset。
    const auto exportedSample = std::find_if(
        exported["note"].begin(), exported["note"].end(), isSoundNode);
    TEST_ASSERT(exportedSample != exported["note"].end(),
                "export should keep the automatic sample");
    TEST_ASSERT((*exportedSample)["beat"] == json::array({ 5, 0, 1 }),
                "sample anchor should convert back to beat 5");
    TEST_ASSERT(exportedSample->value("offset", 0) == -75,
                "sample offset should survive export independently");

    // 规范输出再次加载后，绝对时间应与首次加载完全一致。
    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(exportPath);
    reloaded.sync();
    TEST_ASSERT(reloaded.m_timings.size() == 3,
                "round trip should keep three timings");
    TEST_ASSERT(std::abs(reloaded.m_timings[0].m_timestamp - 100.0) < 1e-6,
                "round trip should keep the unpaired first timing");
    TEST_ASSERT(std::abs(reloaded.m_timings[1].m_timestamp - 2300.0) < 1e-6,
                "round trip should keep delayed timing timestamp");
    TEST_ASSERT(std::abs(reloaded.m_timings[2].m_timestamp - 2350.0) < 1e-6,
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
 * @brief 验证非 Malody 谱面的前导时间导出为配对拍轴相位。
 *
 * 统一模型中首 timing 位于 237ms，而主音频有效播放仍在时间零。Malody
 * 无法直接使用负拍号表达这段前导，保存器需要把首 timing 写在 beat 0，
 * 以一拍减去 237ms 得到 263ms delay，并把主 SOUND 配到相同相位表示。
 *
 * 正相位还要求普通 Note 的导出 beat 前移一拍，但重新加载后其绝对时间仍
 * 必须回到 237ms。Key 和 Slide 都执行同一相位算法，只改变玩家字段协议。
 *
 * 该场景模拟从其它格式新建的模型，没有任何 Malody 来源 beat 元数据，
 * 因而可以证明结果来自当前时间戳计算，而不是复制历史 JSON。
 */
void test_non_malody_lead_in_exports_timing_origin_and_audio_compensation()
{
    XINFO("=== Test: Non-Malody lead-in exports paired note phase ===");

    // 120 BPM 下 237ms 位于前半拍，回卷后的非负 delay 为 500-237=263ms。
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

        // 首 timing 保持 beat 0，其 delay 使用一拍内的非负回卷值。
        TEST_ASSERT(exported["time"].size() == 1,
                    "lead-in export should keep one timing");
        TEST_ASSERT(exported["time"][0]["beat"] == json::array({ 0, 0, 1 }),
                    "paired first timing should remain at beat zero");
        TEST_ASSERT(
            std::abs(exported["time"][0].value("delay", 0.0) - 263.0) < 1e-6,
            "first timing should wrap the main-audio phase");

        // helper 的主音频应成为与首 timing 成对的 beat 0 SOUND。
        const auto sampleIt = std::find_if(
            exported["note"].begin(), exported["note"].end(), isSoundNode);
        TEST_ASSERT(sampleIt != exported["note"].end(),
                    "lead-in export should keep the main sample");
        TEST_ASSERT((*sampleIt)["beat"] == json::array({ 0, 0, 1 }),
                    "main sample should move back one beat");
        TEST_ASSERT(sampleIt->value("offset", -1) == 263,
                    "first-half-beat red line should keep wrapped main "
                    "offset");

        // 普通玩家对象需要前移一个导出拍，才能与回卷后的时间原点对齐。
        const auto noteIt =
            std::find_if(exported["note"].begin(),
                         exported["note"].end(),
                         [](const json& node) { return !isSoundNode(node); });
        TEST_ASSERT(noteIt != exported["note"].end(),
                    "lead-in export should keep the playable note");
        TEST_ASSERT((*noteIt)["beat"] == json::array({ 1, 0, 1 }),
                    "positive paired phase should move playable note one "
                    "Malody beat forward");

        // 重载同时验证 timing、玩家 Note 和主音频有效播放时刻。
        MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outputPath);
        reloaded.sync();
        TEST_ASSERT(reloaded.m_timings.size() == 1 &&
                        std::abs(reloaded.m_timings.front().m_timestamp -
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

/**
 * @brief 验证半拍后相位统一补偿，且晚首红线通过合成锚点保留。
 *
 * 本场景包含两个子情况。第一条红线仍在首拍内但已越过半拍时，不需要额外
 * timing；保存器只把普通 Note 与 effect 的 beat 整体前移，同时主 SOUND
 * 保持零 offset。第二条红线位于第三拍以后时，beat 0 必须增加合成 BPM
 * 锚点，原红线、后续 BPM、effect 和 Note 均保留各自相对位置。
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

    // 子情况一：首 timing 在第一拍后半段，不需要保留额外的原红线副本。
    auto withinFirstBeat                             = makeMinimalBeatMap(0, 4);
    withinFirstBeat.m_baseMapMetadata.preference_bpm = BPM;
    auto& withinFirstTiming         = withinFirstBeat.m_timings.front();
    withinFirstTiming.m_timestamp   = PHASE_MS;
    withinFirstTiming.m_bpm         = BPM;
    withinFirstTiming.m_beat_length = BEAT_LENGTH_MS;
    withinFirstTiming.m_timingEffectParameter = BPM;

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
    // 一组联合断言锁定“无合成 timing、零主偏移、普通内容前移一拍”。
    TEST_ASSERT(
        withinExported["time"].size() == 1 &&
            withinExported["time"][0]["beat"] == json::array({ 0, 0, 1 }) &&
            std::abs(withinExported["time"][0].value("delay", 0.0) -
                     EXPECTED_DELAY_MS) < 1e-6 &&
            withinMainSample != withinExported["note"].end() &&
            (*withinMainSample)["beat"] == json::array({ 0, 0, 1 }) &&
            withinMainSample->value("offset", -1) == 0 &&
            withinPlayable != withinExported["note"].end() &&
            (*withinPlayable)["beat"] == json::array({ 2, 0, 1 }) &&
            withinExported["effect"].size() == 1 &&
            withinExported["effect"][0]["beat"] == json::array({ 2, 0, 1 }),
        "phase after half a beat should keep zero main offset and shift "
        "ordinary content without adding a synthetic timing");

    // 重载后 Note 与 SCROLL 都应回到 PHASE+一拍的相同绝对时间。
    MMM::BeatMap withinReloaded = MMM::BeatMap::loadFromFile(withinOutputPath);
    withinReloaded.sync();
    TEST_ASSERT(
        withinReloaded.m_noteData.notes.size() == 1 &&
            std::abs(withinReloaded.m_noteData.notes.front().m_timestamp -
                     (PHASE_MS + BEAT_LENGTH_MS)) < 1e-6 &&
            withinReloaded.m_timings.size() == 2 &&
            std::abs(withinReloaded.m_timings[1].m_timestamp -
                     (PHASE_MS + BEAT_LENGTH_MS)) < 1e-6,
        "after-half-beat note and effect should round trip together");

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
    // 三条输出红线依次为合成锚点、原首红线和后续 BPM。
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
            mainSample->value("offset", -1) == 0,
        "main audio should use the synthetic anchor without duplicating delay");
    TEST_ASSERT(
        playable != exported["note"].end() &&
            (*playable)["beat"] == json::array({ 5, 0, 1 }) &&
            exported["effect"].size() == 1 &&
            exported["effect"][0]["beat"] == json::array({ 5, 0, 1 }),
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
 * @brief 验证 Malody 主音频与首 timing 使用同一非负回卷值。
 *
 * 这是首拍相位兼容的综合场景，覆盖历史输入、规范回写和编辑失效。基础输入
 * 在 210 BPM 下以 beat 0 delay=237.032ms 表示首 timing，并把同名主 SOUND
 * 放在 beat 0、offset=237。加载后首 timing 解回约 48.682ms，主音频则规范
 * 为时间零且 offset 清零；再次保存必须恢复与原播放等价的配对形态。
 *
 * 后续子场景依次验证：较小配对 delay、超过一拍的 Euclidean modulo、与
 * delay 不匹配的普通 offset、非主音频 offset、亚毫秒 timing 编辑、整拍
 * timing 编辑、主音频编辑、历史 anchor+offset 形态、近零边界、历史整拍
 * 位移、Key/Slide 新生成内容以及偏离时间零的主采样。
 *
 * 每个子场景都围绕同一不变量：只有“首 BPM timing + 时间零同名主音频”
 * 可以共享配对相位。任何身份、时间或偏移不满足条件的采样都按普通 SOUND
 * 处理，不能借用首 timing delay。
 */
void test_first_timing_delay_unwraps_with_its_bpm()
{
    XINFO("=== Test: First timing delay unwraps with first BPM ===");

    // 首 BPM 决定回卷模数，不能错误使用 meta.song.bpm 的旧缓存。
    constexpr double BPM              = 210.0;
    constexpr double WRAPPED_DELAY_MS = 237.032272;
    constexpr double BEAT_LENGTH_MS   = 60000.0 / BPM;
    constexpr double EXPECTED_TIME_MS = BEAT_LENGTH_MS - WRAPPED_DELAY_MS;

    const fs::path sourcePath = std::filesystem::temp_directory_path() /
                                "edge_wrapped_first_timing_source.mc";
    const fs::path exportPath = std::filesystem::temp_directory_path() /
                                "edge_wrapped_first_timing_export.mc";

    // 基础历史输入采用 Slide 模式，并显式保存配对 timing 与同名主 SOUND。
    json fileData;
    fileData["meta"] = { { "id", 0 },
                         { "creator", "Test" },
                         { "version", "Wrapped" },
                         { "mode", 7 },
                         { "mode_ext", { { "bar_begin", 0 } } },
                         { "song",
                           { { "title", "Wrapped" },
                             { "artist", "Test" },
                             { "file", "music.ogg" },
                             { "bpm", BPM } } } };
    // delay 小于一拍但位于前半拍，加载后使用“一拍减 delay”的正相位。
    fileData["time"] = json::array({ { { "beat", json::array({ 0, 0, 1 }) },
                                       { "bpm", BPM },
                                       { "delay", WRAPPED_DELAY_MS } } });
    // 玩家 Note 已前移到 beat 1；主 SOUND 在 beat 0 保存近似整数 offset。
    fileData["note"] = json::array(
        { { { "beat", json::array({ 1, 0, 1 }) }, { "x", 31 }, { "w", 60 } },
          { { "beat", json::array({ 0, 0, 1 }) },
            { "type", 1 },
            { "sound", "music.ogg" },
            { "offset", 237 } } });

    std::ofstream source(sourcePath);
    TEST_ASSERT(source.good(), "should open wrapped timing input");
    source << fileData.dump();
    source.close();

    // 基础加载验证配对识别：timing 解相位，主 SOUND 归一到时间零。
    MMM::BeatMap loaded = MMM::BeatMap::loadFromFile(sourcePath);
    loaded.sync();
    TEST_ASSERT(loaded.m_timings.size() == 1,
                "wrapped timing map should keep its first timing");
    TEST_ASSERT(std::abs(loaded.m_timings.front().m_timestamp -
                         EXPECTED_TIME_MS) < 1e-6,
                "237.032ms at 210 BPM should unwrap to about 48.682ms");
    // 玩家 Note 的导出拍位移应在加载时抵消，回到 timing 的绝对时间。
    TEST_ASSERT(loaded.m_noteData.notes.size() == 1 &&
                    std::abs(loaded.m_noteData.notes.front().m_timestamp -
                             EXPECTED_TIME_MS) < 1e-6,
                "shifted playable beat should use the unwrapped timing anchor");
    // 主音频内部锚点和 offset 都归零，effectiveTimestamp 仍接近零。
    TEST_ASSERT(
        loaded.m_audioSamples.size() == 1 &&
            std::abs(loaded.m_audioSamples.front().m_timestamp - 0.0) < 1e-6,
        "paired main sample should normalize its anchor to time zero");
    TEST_ASSERT(loaded.m_audioSamples.front().m_offsetMs == 0,
                "paired main sample should normalize its offset to zero");
    TEST_ASSERT(
        std::abs(loaded.m_audioSamples.front().effectiveTimestamp()) < 0.51,
        "paired main sample should still play at time zero");

    // 规范模型回写时重新构造等价配对字段，不直接复制输入 JSON。
    TEST_ASSERT(loaded.saveToFile(exportPath),
                "wrapped timing map should export");
    std::ifstream exportedFile(exportPath);
    json          exported;
    exportedFile >> exported;
    TEST_ASSERT(exported["time"][0]["beat"] == json::array({ 0, 0, 1 }),
                "wrapped first timing should keep beat zero");
    TEST_ASSERT(std::abs(exported["time"][0].value("delay", 0.0) -
                         WRAPPED_DELAY_MS) < 1e-6,
                "wrapped first timing should keep its non-negative delay");
    // 主 SOUND 应恢复 beat 0 与整数 offset=237。
    const auto exportedSample = std::find_if(
        exported["note"].begin(), exported["note"].end(), isSoundNode);
    TEST_ASSERT(exportedSample != exported["note"].end(),
                "wrapped map should keep its main sample");
    TEST_ASSERT(
        (*exportedSample)["beat"] == json::array({ 0, 0, 1 }) &&
            exportedSample->value("offset", -1) == 237,
        "first-half-beat wrapped main sample should keep its delay offset");
    // 玩家 Note 再次前移到 beat 1，与正相位 timing 保持绝对时间一致。
    const auto exportedPlayable = std::find_if(
        exported["note"].begin(), exported["note"].end(), [](const json& node) {
            return !isSoundNode(node);
        });
    TEST_ASSERT(exportedPlayable != exported["note"].end() &&
                    (*exportedPlayable)["beat"] == json::array({ 1, 0, 1 }),
                "wrapped playable note should retain its one-beat phase shift");

    // 子场景一：较小 100ms 配对 delay，验证规则不只针对 237ms 特例。
    json positiveOffsetData                = fileData;
    positiveOffsetData["time"][0]["delay"] = 100.0;
    auto& positiveSample                   = positiveOffsetData["note"][1];
    positiveSample["offset"]               = 100;
    const fs::path positivePath =
        std::filesystem::temp_directory_path() / "edge_positive_main_offset.mc";
    const fs::path positiveExportPath = std::filesystem::temp_directory_path() /
                                        "edge_positive_main_offset_export.mc";
    std::ofstream positiveFile(positivePath);
    TEST_ASSERT(positiveFile.good(), "should open positive offset input");
    positiveFile << positiveOffsetData.dump();
    positiveFile.close();

    // 小 delay 同样按一拍取模，但规范主音频不重复保存内部 offset。
    MMM::BeatMap positiveLoaded = MMM::BeatMap::loadFromFile(positivePath);
    positiveLoaded.sync();
    constexpr double EXPECTED_SMALL_DELAY_TIME_MS = BEAT_LENGTH_MS - 100.0;
    TEST_ASSERT(positiveLoaded.m_timings.size() == 1 &&
                    std::abs(positiveLoaded.m_timings.front().m_timestamp -
                             EXPECTED_SMALL_DELAY_TIME_MS) < 1e-6,
                "small paired delay should use the same modulo phase rule");
    TEST_ASSERT(
        positiveLoaded.m_audioSamples.size() == 1 &&
            std::abs(positiveLoaded.m_audioSamples.front().m_timestamp) <
                1e-6 &&
            positiveLoaded.m_audioSamples.front().m_offsetMs == 0,
        "paired small delay should normalize the main sample to time zero");
    TEST_ASSERT(positiveLoaded.saveToFile(positiveExportPath),
                "paired small delay map should export");
    std::ifstream positiveExportFile(positiveExportPath);
    json          positiveExported;
    positiveExportFile >> positiveExported;
    // 回写时 timing 保持 100ms delay，主 SOUND offset 归零避免双重补偿。
    const auto positiveExportedSample =
        std::find_if(positiveExported["note"].begin(),
                     positiveExported["note"].end(),
                     isSoundNode);
    TEST_ASSERT(
        std::abs(positiveExported["time"][0].value("delay", 0.0) - 100.0) <
                1e-6 &&
            positiveExportedSample != positiveExported["note"].end() &&
            positiveExportedSample->value("offset", -1) == 0,
        "small delay should round trip without duplicating main offset");

    // 子场景二：delay 超过三拍，必须使用非负 Euclidean modulo 而非直接截断。
    constexpr double LARGE_DELAY_BPM     = 212.0;
    constexpr double LARGE_DELAY_BEAT_MS = 60000.0 / LARGE_DELAY_BPM;
    constexpr double LARGE_DELAY_MS      = 1083.54;
    constexpr double LARGE_DELAY_PHASE_MS =
        4.0 * LARGE_DELAY_BEAT_MS - LARGE_DELAY_MS;
    json largeDelayData                   = fileData;
    largeDelayData["meta"]["song"]["bpm"] = LARGE_DELAY_BPM;
    largeDelayData["time"][0]["bpm"]      = LARGE_DELAY_BPM;
    largeDelayData["time"][0]["delay"]    = LARGE_DELAY_MS;
    largeDelayData["note"][1]["offset"]   = 1084;
    const fs::path largeDelayPath = std::filesystem::temp_directory_path() /
                                    "edge_large_paired_main_offset.mc";
    const fs::path largeDelayExportPath =
        std::filesystem::temp_directory_path() /
        "edge_large_paired_main_offset_export.mc";
    std::ofstream largeDelayFile(largeDelayPath);
    TEST_ASSERT(largeDelayFile.good(), "should open large paired delay input");
    largeDelayFile << largeDelayData.dump();
    largeDelayFile.close();

    // 加载后的 timing 只保留余数对应相位，主 SOUND 仍归一到时间零。
    MMM::BeatMap largeDelayLoaded = MMM::BeatMap::loadFromFile(largeDelayPath);
    largeDelayLoaded.sync();
    TEST_ASSERT(
        largeDelayLoaded.m_timings.size() == 1 &&
            std::abs(largeDelayLoaded.m_timings.front().m_timestamp -
                     LARGE_DELAY_PHASE_MS) < 1e-6,
        "paired delay larger than one beat should use Euclidean modulo");
    TEST_ASSERT(
        largeDelayLoaded.m_audioSamples.size() == 1 &&
            std::abs(largeDelayLoaded.m_audioSamples.front().m_timestamp) <
                1e-6 &&
            largeDelayLoaded.m_audioSamples.front().m_offsetMs == 0,
        "large paired delay should normalize the main sample to time zero");
    TEST_ASSERT(largeDelayLoaded.saveToFile(largeDelayExportPath),
                "large paired delay map should export");
    std::ifstream largeDelayExportFile(largeDelayExportPath);
    json          largeDelayExported;
    largeDelayExportFile >> largeDelayExported;
    // 规范回写只输出一拍内余数，防止历史多拍 delay 持续累积。
    const auto largeDelayExportedSample =
        std::find_if(largeDelayExported["note"].begin(),
                     largeDelayExported["note"].end(),
                     isSoundNode);
    TEST_ASSERT(
        std::abs(largeDelayExported["time"][0].value("delay", 0.0) -
                 std::fmod(LARGE_DELAY_MS, LARGE_DELAY_BEAT_MS)) < 1e-6 &&
            largeDelayExportedSample != largeDelayExported["note"].end() &&
            largeDelayExportedSample->value("offset", -1) ==
                static_cast<std::int64_t>(std::llround(
                    std::fmod(LARGE_DELAY_MS, LARGE_DELAY_BEAT_MS))),
        "large first-half-beat delay should keep its wrapped main offset");

    // 子场景三：主 SOUND offset 与 timing delay 不同，不满足配对条件。
    json  unmatchedOffsetData    = fileData;
    auto& unmatchedSample        = unmatchedOffsetData["note"][1];
    unmatchedSample["offset"]    = 200;
    const fs::path unmatchedPath = std::filesystem::temp_directory_path() /
                                   "edge_unmatched_main_offset.mc";
    std::ofstream unmatchedFile(unmatchedPath);
    TEST_ASSERT(unmatchedFile.good(), "should open unmatched offset input");
    unmatchedFile << unmatchedOffsetData.dump();
    unmatchedFile.close();

    // 未配对 offset 必须原样保留，不能因为资源名相同就被清零。
    MMM::BeatMap unmatchedLoaded = MMM::BeatMap::loadFromFile(unmatchedPath);
    unmatchedLoaded.sync();
    TEST_ASSERT(unmatchedLoaded.m_audioSamples.size() == 1 &&
                    unmatchedLoaded.m_audioSamples.front().m_offsetMs == 200,
                "offset not paired with first delay must remain positive");

    // 子场景四：offset 数值相同但资源不是 song.file，仍属于普通效果音。
    json effectOffsetData                = fileData;
    effectOffsetData["note"][1]["sound"] = "effect.ogg";
    const fs::path effectOffsetPath = std::filesystem::temp_directory_path() /
                                      "edge_effect_wrapped_offset.mc";
    std::ofstream effectOffsetFile(effectOffsetPath);
    TEST_ASSERT(effectOffsetFile.good(), "should open effect offset input");
    effectOffsetFile << effectOffsetData.dump();
    effectOffsetFile.close();

    // 非主音频永远不参与首拍配对，237ms 继续作为逐采样偏移。
    MMM::BeatMap effectOffsetLoaded =
        MMM::BeatMap::loadFromFile(effectOffsetPath);
    effectOffsetLoaded.sync();
    TEST_ASSERT(effectOffsetLoaded.m_audioSamples.size() == 1 &&
                    effectOffsetLoaded.m_audioSamples.front().m_offsetMs == 237,
                "non-main sample offset must never trigger paired wrapping");

    // 基础规范输出再重载，确认配对相位不会在多次保存后漂移。
    MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(exportPath);
    reloaded.sync();
    TEST_ASSERT(reloaded.m_timings.size() == 1 &&
                    std::abs(reloaded.m_timings.front().m_timestamp -
                             EXPECTED_TIME_MS) < 1e-6,
                "wrapped delay should remain stable after round trip");
    TEST_ASSERT(
        reloaded.m_audioSamples.size() == 1 &&
            std::abs(reloaded.m_audioSamples.front().m_timestamp) < 1e-6 &&
            reloaded.m_audioSamples.front().m_offsetMs == 0 &&
            std::abs(reloaded.m_audioSamples.front().effectiveTimestamp()) <
                0.51,
        "wrapped main sample should remain normalized at time zero");

    // 子场景五：只移动 timing 0.4ms，当前模型时间必须覆盖导入时的 phase 缓存。
    constexpr double SUB_MILLISECOND_EDIT_MS = 0.4;
    MMM::BeatMap     timingMoved = MMM::BeatMap::loadFromFile(sourcePath);
    timingMoved.m_timings.front().m_timestamp += SUB_MILLISECOND_EDIT_MS;
    timingMoved.sync();
    const fs::path timingMovedPath =
        std::filesystem::temp_directory_path() / "edge_wrapped_timing_moved.mc";
    TEST_ASSERT(timingMoved.saveToFile(timingMovedPath),
                "sub-millisecond timing edit should export");
    std::ifstream timingMovedFile(timingMovedPath);
    json          timingMovedJson;
    timingMovedFile >> timingMovedJson;
    // timing 后移会令等价 delay 减少相同数值，证明保存器重新计算相位。
    TEST_ASSERT(std::abs(timingMovedJson["time"][0].value("delay", 0.0) -
                         (WRAPPED_DELAY_MS - SUB_MILLISECOND_EDIT_MS)) < 1e-6,
                "sub-millisecond timing edit must replace the imported phase");
    MMM::BeatMap timingMovedReloaded =
        MMM::BeatMap::loadFromFile(timingMovedPath);
    timingMovedReloaded.sync();
    TEST_ASSERT(std::abs(timingMovedReloaded.m_timings.front().m_timestamp -
                         (EXPECTED_TIME_MS + SUB_MILLISECOND_EDIT_MS)) < 1e-6,
                "sub-millisecond timing edit should survive round trip");

    // 子场景六：原红线前后移动整拍后，合成锚点与原红线都应保留。
    for ( const int wholeBeatDelta : { -1, 1 } ) {
        MMM::BeatMap wholeBeatMoved = MMM::BeatMap::loadFromFile(sourcePath);
        wholeBeatMoved.m_timings.front().m_timestamp +=
            static_cast<double>(wholeBeatDelta) * BEAT_LENGTH_MS;
        wholeBeatMoved.sync();
        const fs::path wholeBeatMovedPath =
            std::filesystem::temp_directory_path() /
            (std::string("edge_wrapped_timing_whole_beat_") +
             (wholeBeatDelta < 0 ? "back.mc" : "forward.mc"));
        TEST_ASSERT(wholeBeatMoved.saveToFile(wholeBeatMovedPath),
                    "whole-beat timing edit should export");
        std::ifstream wholeBeatMovedFile(wholeBeatMovedPath);
        json          wholeBeatMovedJson;
        wholeBeatMovedFile >> wholeBeatMovedJson;
        // 主 SOUND 继续绑定首拍合成锚点，原 timing 独立写在移动后的拍号。
        const auto wholeBeatMovedSample =
            std::find_if(wholeBeatMovedJson["note"].begin(),
                         wholeBeatMovedJson["note"].end(),
                         isSoundNode);
        const json expectedOriginalBeat =
            json::array({ wholeBeatDelta < 0 ? 0 : 2, 0, 1 });
        TEST_ASSERT(
            wholeBeatMovedJson["time"].size() == 2 &&
                wholeBeatMovedJson["time"][0]["beat"] ==
                    json::array({ 0, 0, 1 }) &&
                std::abs(wholeBeatMovedJson["time"][0].value("delay", 0.0) -
                         WRAPPED_DELAY_MS) < 1e-6 &&
                wholeBeatMovedJson["time"][1]["beat"] == expectedOriginalBeat &&
                !wholeBeatMovedJson["time"][1].contains("delay") &&
                wholeBeatMovedSample != wholeBeatMovedJson["note"].end() &&
                (*wholeBeatMovedSample)["beat"] == json::array({ 0, 0, 1 }),
            "out-of-range first timing should keep a first-beat anchor and its "
            "original red line");
        MMM::BeatMap wholeBeatMovedReloaded =
            MMM::BeatMap::loadFromFile(wholeBeatMovedPath);
        wholeBeatMovedReloaded.sync();
        // 重载按绝对时间查找移动后的原红线，不依赖合成锚点排序。
        const double expectedOriginalTime =
            EXPECTED_TIME_MS +
            static_cast<double>(wholeBeatDelta) * BEAT_LENGTH_MS;
        TEST_ASSERT(
            wholeBeatMovedReloaded.m_timings.size() == 2 &&
                std::any_of(wholeBeatMovedReloaded.m_timings.begin(),
                            wholeBeatMovedReloaded.m_timings.end(),
                            [&](const MMM::Timing& timing) {
                                return std::abs(timing.m_timestamp -
                                                expectedOriginalTime) < 1e-6;
                            }),
            "whole-beat timing edit and synthetic anchor should round trip");
    }

    // 子场景七：移动主音频锚点 0.4ms 后，它不再代表严格的时间零配对 SOUND。
    MMM::BeatMap mainMoved = MMM::BeatMap::loadFromFile(sourcePath);
    mainMoved.m_audioSamples.front().m_timestamp = SUB_MILLISECOND_EDIT_MS;
    mainMoved.sync();
    const fs::path mainMovedPath =
        std::filesystem::temp_directory_path() / "edge_wrapped_main_moved.mc";
    TEST_ASSERT(mainMoved.saveToFile(mainMovedPath),
                "sub-millisecond main-audio edit should export");
    std::ifstream mainMovedFile(mainMovedPath);
    json          mainMovedJson;
    mainMovedFile >> mainMovedJson;
    // 普通采样时序应从新 timestamp 量化，不能继续输出旧 237ms 配对 offset。
    const auto mainMovedSample = std::find_if(mainMovedJson["note"].begin(),
                                              mainMovedJson["note"].end(),
                                              isSoundNode);
    TEST_ASSERT(
        std::abs(mainMovedJson["time"][0].value("delay", 0.0) -
                 EXPECTED_TIME_MS) < 1e-6 &&
            mainMovedSample != mainMovedJson["note"].end() &&
            mainMovedSample->value("offset", 0) == 0,
        "moved main audio must use ordinary sample timing instead of pairing");
    MMM::BeatMap mainMovedReloaded = MMM::BeatMap::loadFromFile(mainMovedPath);
    mainMovedReloaded.sync();
    TEST_ASSERT(
        std::abs(mainMovedReloaded.m_audioSamples.front().effectiveTimestamp() -
                 SUB_MILLISECOND_EDIT_MS) < 0.2,
        "sub-millisecond main-audio edit should survive beat quantization");

    // 子场景八：构造历史 anchor+负 offset 的内部形态，验证规范化输出兼容。
    MMM::BeatMap legacyShape = MMM::BeatMap::loadFromFile(sourcePath);
    legacyShape.m_audioSamples.front().m_timestamp = EXPECTED_TIME_MS;
    legacyShape.m_audioSamples.front().m_offsetMs =
        static_cast<std::int64_t>(std::llround(237.0 - BEAT_LENGTH_MS));
    legacyShape.sync();
    const fs::path legacyShapePath = std::filesystem::temp_directory_path() /
                                     "edge_legacy_wrapped_main_shape.mc";
    TEST_ASSERT(legacyShape.saveToFile(legacyShapePath),
                "legacy wrapped main-audio shape should export");
    std::ifstream legacyShapeFile(legacyShapePath);
    json          legacyShapeJson;
    legacyShapeFile >> legacyShapeJson;
    // 历史形态仍应收敛为当前 beat 0 + 正 offset 配对表示。
    const auto legacyShapeSample = std::find_if(legacyShapeJson["note"].begin(),
                                                legacyShapeJson["note"].end(),
                                                isSoundNode);
    TEST_ASSERT(std::abs(legacyShapeJson["time"][0].value("delay", 0.0) -
                         WRAPPED_DELAY_MS) < 1e-6 &&
                    legacyShapeSample != legacyShapeJson["note"].end() &&
                    legacyShapeSample->value("offset", -1) == 237,
                "legacy anchor-plus-offset shape should export canonically");

    // 子场景九：原 timing 略早于零，覆盖回卷相位的数值边界。
    constexpr double LEGACY_ZERO_BOUNDARY_MS = -0.1;
    const double     legacyNearBeatDelayMs =
        BEAT_LENGTH_MS + LEGACY_ZERO_BOUNDARY_MS;
    MMM::BeatMap legacyZeroBoundary = MMM::BeatMap::loadFromFile(sourcePath);
    legacyZeroBoundary.m_timings.front().m_timestamp = LEGACY_ZERO_BOUNDARY_MS;
    legacyZeroBoundary.m_timings.front()
        .m_metadata
        .timing_properties[MMM::TimingMetadataType::MALODY]["delay"] =
        json(legacyNearBeatDelayMs).dump();
    legacyZeroBoundary.m_audioSamples.front().m_timestamp =
        LEGACY_ZERO_BOUNDARY_MS;
    legacyZeroBoundary.m_audioSamples.front().m_offsetMs = 0;
    legacyZeroBoundary.m_noteData.notes.front().m_timestamp =
        LEGACY_ZERO_BOUNDARY_MS;
    legacyZeroBoundary.sync();
    const fs::path legacyZeroBoundaryPath =
        std::filesystem::temp_directory_path() /
        "edge_legacy_wrapped_zero_boundary.mc";
    TEST_ASSERT(legacyZeroBoundary.saveToFile(legacyZeroBoundaryPath),
                "legacy near-zero wrapped shape should export");
    std::ifstream legacyZeroBoundaryFile(legacyZeroBoundaryPath);
    json          legacyZeroBoundaryJson;
    legacyZeroBoundaryFile >> legacyZeroBoundaryJson;
    // 玩家 Note 的导出 beat 必须使用规范化后的 timing 位置进行相位偏移。
    const auto legacyZeroBoundaryPlayable =
        std::find_if(legacyZeroBoundaryJson["note"].begin(),
                     legacyZeroBoundaryJson["note"].end(),
                     [](const json& node) { return !isSoundNode(node); });
    TEST_ASSERT(
        legacyZeroBoundaryPlayable != legacyZeroBoundaryJson["note"].end() &&
            (*legacyZeroBoundaryPlayable)["beat"] == json::array({ 1, 0, 1 }),
        "legacy near-zero phase should use the exported timing position when "
        "shifting note-array beats");
    // 重载后合成锚点、原 timing 与 Note 保持对齐，不能出现负零附近漂移。
    MMM::BeatMap legacyZeroBoundaryReloaded =
        MMM::BeatMap::loadFromFile(legacyZeroBoundaryPath);
    legacyZeroBoundaryReloaded.sync();
    TEST_ASSERT(
        legacyZeroBoundaryReloaded.m_timings.size() == 2 &&
            legacyZeroBoundaryReloaded.m_noteData.notes.size() == 1 &&
            legacyZeroBoundaryReloaded.m_timings.front().m_timestamp > 0.0 &&
            std::abs(legacyZeroBoundaryReloaded.m_noteData.notes.front()
                         .m_timestamp -
                     legacyZeroBoundaryReloaded.m_timings.front().m_timestamp) <
                1e-6,
        "legacy near-zero phase must keep note, original timing, and synthetic "
        "anchor aligned after canonicalization");

    // 子场景十：历史内部形态整体前后移动一拍，仍需保留原红线。
    for ( const int wholeBeatDelta : { -1, 1 } ) {
        MMM::BeatMap legacyWholeBeat = MMM::BeatMap::loadFromFile(sourcePath);
        legacyWholeBeat.m_timings.front().m_timestamp =
            EXPECTED_TIME_MS +
            static_cast<double>(wholeBeatDelta) * BEAT_LENGTH_MS;
        legacyWholeBeat.m_audioSamples.front().m_timestamp =
            legacyWholeBeat.m_timings.front().m_timestamp;
        legacyWholeBeat.m_audioSamples.front().m_offsetMs =
            static_cast<std::int64_t>(std::llround(237.0 - BEAT_LENGTH_MS));
        legacyWholeBeat.sync();
        const fs::path legacyWholeBeatPath =
            std::filesystem::temp_directory_path() /
            (std::string("edge_legacy_wrapped_main_whole_beat_") +
             (wholeBeatDelta < 0 ? "back.mc" : "forward.mc"));
        TEST_ASSERT(legacyWholeBeat.saveToFile(legacyWholeBeatPath),
                    "legacy whole-beat wrapped shape should export");
        std::ifstream legacyWholeBeatFile(legacyWholeBeatPath);
        json          legacyWholeBeatJson;
        legacyWholeBeatFile >> legacyWholeBeatJson;
        const auto legacyWholeBeatSample =
            std::find_if(legacyWholeBeatJson["note"].begin(),
                         legacyWholeBeatJson["note"].end(),
                         isSoundNode);
        // 原红线根据移动方向落在 beat 0 或 2，合成锚点始终位于 beat 0。
        const json expectedOriginalBeat =
            json::array({ wholeBeatDelta < 0 ? 0 : 2, 0, 1 });
        TEST_ASSERT(
            legacyWholeBeatJson["time"].size() == 2 &&
                legacyWholeBeatJson["time"][0]["beat"] ==
                    json::array({ 0, 0, 1 }) &&
                std::abs(legacyWholeBeatJson["time"][0].value("delay", 0.0) -
                         WRAPPED_DELAY_MS) < 1e-6 &&
                legacyWholeBeatJson["time"][1]["beat"] ==
                    expectedOriginalBeat &&
                legacyWholeBeatSample != legacyWholeBeatJson["note"].end() &&
                (*legacyWholeBeatSample)["beat"] == json::array({ 0, 0, 1 }) &&
                legacyWholeBeatSample->value("offset", -1) == 237,
            "legacy wrapped shape should retain the original red line after "
            "the synthetic anchor");
        MMM::BeatMap legacyWholeBeatReloaded =
            MMM::BeatMap::loadFromFile(legacyWholeBeatPath);
        legacyWholeBeatReloaded.sync();
        // 绝对时间检查保证两个 beat 0 节点也不会在加载时错误去重。
        const double expectedLegacyOriginalTime =
            legacyWholeBeat.m_timings.front().m_timestamp;
        TEST_ASSERT(
            legacyWholeBeatReloaded.m_timings.size() == 2 &&
                std::any_of(legacyWholeBeatReloaded.m_timings.begin(),
                            legacyWholeBeatReloaded.m_timings.end(),
                            [&](const MMM::Timing& timing) {
                                return std::abs(timing.m_timestamp -
                                                expectedLegacyOriginalTime) <
                                       1e-6;
                            }) &&
                legacyWholeBeatReloaded.m_audioSamples.size() == 1 &&
                std::abs(legacyWholeBeatReloaded.m_audioSamples.front()
                             .m_timestamp) < 1e-6 &&
                legacyWholeBeatReloaded.m_audioSamples.front().m_offsetMs == 0,
            "legacy whole-beat shape and synthetic anchor should reload");
    }

    // 子场景十一：从统一模型生成 Key/Slide，覆盖无来源 metadata 的完整内容。
    for ( const int mode : { 0, 7 } ) {
        auto generated = makeMinimalBeatMap(mode, 4);
        generated.m_baseMapMetadata.preference_bpm = 123.0;
        auto& firstTiming                   = generated.m_timings.front();
        firstTiming.m_timestamp             = EXPECTED_TIME_MS;
        firstTiming.m_bpm                   = BPM;
        firstTiming.m_beat_length           = BEAT_LENGTH_MS;
        firstTiming.m_timingEffectParameter = BPM;
        generated.m_baseMapMetadata.song_file_hint         = "music.ogg";
        generated.m_audioSamples.front().m_audioResourceId = "music.ogg";
        generated.m_audioSamples.front().m_timestamp       = 0.0;
        generated.m_audioSamples.front().m_offsetMs        = 0;

        // 同时刻 SCROLL 用于验证 effect[] 接受与 note[] 相同的正相位补偿。
        MMM::Timing& scroll            = generated.m_timings.emplace_back();
        scroll.m_timestamp             = EXPECTED_TIME_MS;
        scroll.m_bpm                   = BPM;
        scroll.m_timingEffect          = MMM::TimingEffect::SCROLL;
        scroll.m_timingEffectParameter = 1.25;
        scroll.m_beat_length           = 1.25;

        // 普通 Note 位于首 timing，导出后应前移一拍、重载后恢复原时间。
        MMM::Note& note  = generated.m_noteData.notes.emplace_back();
        note.m_type      = MMM::NoteType::NOTE;
        note.m_timestamp = EXPECTED_TIME_MS;
        note.m_track     = 0;

        // Hold 跨一拍，分别覆盖 Key endbeat 与 Slide 相对 seg。
        MMM::Hold& hold  = generated.m_noteData.holds.emplace_back();
        hold.m_type      = MMM::NoteType::HOLD;
        hold.m_timestamp = EXPECTED_TIME_MS + BEAT_LENGTH_MS;
        hold.m_duration  = BEAT_LENGTH_MS;
        hold.m_track     = 1;

        // 第二条普通采样不与主音频配对，但也要接受 note[] 的拍号相位位移。
        MMM::AudioSampleEvent& ordinarySample =
            generated.m_audioSamples.emplace_back();
        ordinarySample.m_timestamp       = EXPECTED_TIME_MS;
        ordinarySample.m_track           = 5;
        ordinarySample.m_audioResourceId = "effect.ogg";
        generated.sync();

        const fs::path generatedPath =
            std::filesystem::temp_directory_path() /
            (std::string("edge_generated_wrapped_first_timing_") +
             (mode == 0 ? "key.mc" : "slide.mc"));
        TEST_ASSERT(generated.saveToFile(generatedPath),
                    "generated wrapped timing map should export");

        std::ifstream generatedFile(generatedPath);
        json          generatedJson;
        generatedFile >> generatedJson;
        TEST_ASSERT(
            generatedJson["time"][0]["beat"] == json::array({ 0, 0, 1 }),
            "paired first timing should export at beat zero");
        TEST_ASSERT(
            std::abs(generatedJson["time"][0].value("delay", 0.0) -
                     WRAPPED_DELAY_MS) < 1e-6,
            "paired first timing should export its wrapped phase delay");
        // 先定位主 SOUND，确认其配对 offset 与首 timing delay 一致。
        const auto generatedSample = std::find_if(generatedJson["note"].begin(),
                                                  generatedJson["note"].end(),
                                                  isSoundNode);
        TEST_ASSERT(
            generatedSample != generatedJson["note"].end() &&
                (*generatedSample)["beat"] == json::array({ 0, 0, 1 }) &&
                generatedSample->value("offset", -1) == 237,
            "first-half-beat main SOUND should keep the wrapped delay");
        // 普通 Note 的绝对时间依靠正相位拍号补偿保持。
        const auto generatedPlayable =
            std::find_if(generatedJson["note"].begin(),
                         generatedJson["note"].end(),
                         [](const json& node) { return !isSoundNode(node); });
        TEST_ASSERT(
            generatedPlayable != generatedJson["note"].end() &&
                (*generatedPlayable)["beat"] == json::array({ 1, 0, 1 }),
            "positive paired phase should shift generated playable note");
        // 按模式字段定位 Hold，避免 Key 与 Slide 共用错误的持续时间表示。
        const auto generatedHold =
            std::find_if(generatedJson["note"].begin(),
                         generatedJson["note"].end(),
                         [&](const json& node) {
                             return !isSoundNode(node) &&
                                    (mode == 0 ? node.contains("endbeat")
                                               : node.contains("seg"));
                         });
        TEST_ASSERT(generatedHold != generatedJson["note"].end() &&
                        (*generatedHold)["beat"] == json::array({ 2, 0, 1 }),
                    "positive paired phase should shift Hold root beat");
        // Key 结束拍是绝对值；Slide seg 拍号则相对折线根，不接受全局偏移。
        if ( mode == 0 ) {
            TEST_ASSERT(
                (*generatedHold)["endbeat"] == json::array({ 3, 0, 1 }),
                "Key Hold endbeat should receive the same absolute shift");
        } else {
            TEST_ASSERT(
                (*generatedHold)["seg"].size() == 1 &&
                    (*generatedHold)["seg"][0]["beat"] ==
                        json::array({ 1, 0, 1 }),
                "Slide Hold relative segment beat should remain unchanged");
        }
        // 普通自动采样与 effect 一样前移导出拍，但不共享主 SOUND offset。
        const auto generatedOrdinarySample = std::find_if(
            generatedJson["note"].begin(),
            generatedJson["note"].end(),
            [](const json& node) {
                return isSoundNode(node) &&
                       node.value("sound", std::string{}) == "effect.ogg";
            });
        TEST_ASSERT(
            generatedOrdinarySample != generatedJson["note"].end() &&
                (*generatedOrdinarySample)["beat"] == json::array({ 1, 0, 1 }),
            "ordinary automatic sample should receive the note-array phase "
            "shift");
        TEST_ASSERT(
            !generatedJson["effect"].empty() &&
                generatedJson["effect"][0]["beat"] == json::array({ 1, 0, 1 }),
            "effect at the paired first timing should share the phase shift");

        // 重载后所有导出相位补偿都应抵消，恢复统一模型的绝对时间。
        MMM::BeatMap generatedReloaded =
            MMM::BeatMap::loadFromFile(generatedPath);
        generatedReloaded.sync();
        TEST_ASSERT(generatedReloaded.m_timings.size() == 2,
                    "generated wrapped map should keep BPM and effect");
        TEST_ASSERT(
            std::abs(generatedReloaded.m_timings[0].m_timestamp -
                     EXPECTED_TIME_MS) < 1e-6 &&
                std::abs(generatedReloaded.m_timings[1].m_timestamp -
                         EXPECTED_TIME_MS) < 1e-6,
            "generated BPM and effect should round trip at the paired phase");
        TEST_ASSERT(std::abs(generatedReloaded.m_timings[1].m_bpm - BPM) < 1e-6,
                    "effect before a positive first beat should use first BPM");
        TEST_ASSERT(
            generatedReloaded.m_noteData.notes.size() == 1 &&
                std::abs(
                    generatedReloaded.m_noteData.notes.front().m_timestamp -
                    EXPECTED_TIME_MS) < 1e-6,
            "generated playable note should remove the phase shift on import");
        TEST_ASSERT(
            generatedReloaded.m_noteData.holds.size() == 1 &&
                std::abs(
                    generatedReloaded.m_noteData.holds.front().m_timestamp -
                    (EXPECTED_TIME_MS + BEAT_LENGTH_MS)) < 1e-6 &&
                std::abs(generatedReloaded.m_noteData.holds.front().m_duration -
                         BEAT_LENGTH_MS) < 1e-6,
            "generated Hold absolute beats should round trip");
        TEST_ASSERT(
            generatedReloaded.m_audioSamples.size() == 2,
            "generated samples should keep main and ordinary automatic audio");
        // 按资源 ID 区分主音频和普通采样，避免依赖自动采样数组顺序。
        const auto generatedReloadedMain =
            std::find_if(generatedReloaded.m_audioSamples.begin(),
                         generatedReloaded.m_audioSamples.end(),
                         [](const MMM::AudioSampleEvent& sample) {
                             return sample.m_audioResourceId == "music.ogg";
                         });
        const auto generatedReloadedOrdinary =
            std::find_if(generatedReloaded.m_audioSamples.begin(),
                         generatedReloaded.m_audioSamples.end(),
                         [](const MMM::AudioSampleEvent& sample) {
                             return sample.m_audioResourceId == "effect.ogg";
                         });
        TEST_ASSERT(
            generatedReloadedMain != generatedReloaded.m_audioSamples.end() &&
                std::abs(generatedReloadedMain->m_timestamp) < 1e-6 &&
                generatedReloadedMain->m_offsetMs == 0 &&
                generatedReloadedOrdinary !=
                    generatedReloaded.m_audioSamples.end() &&
                std::abs(generatedReloadedOrdinary->m_timestamp -
                         EXPECTED_TIME_MS) < 1e-6,
            "automatic samples should remove the note-array phase shift on "
            "import");
    }

    // 子场景十二：主资源名相同但采样锚点为 20ms，不满足时间零配对条件。
    auto  displacedMain                     = makeMinimalBeatMap(7, 4);
    auto& displacedTiming                   = displacedMain.m_timings.front();
    displacedTiming.m_timestamp             = EXPECTED_TIME_MS;
    displacedTiming.m_bpm                   = BPM;
    displacedTiming.m_beat_length           = BEAT_LENGTH_MS;
    displacedTiming.m_timingEffectParameter = BPM;
    displacedMain.m_baseMapMetadata.song_file_hint         = "music.ogg";
    displacedMain.m_audioSamples.front().m_audioResourceId = "music.ogg";
    displacedMain.m_audioSamples.front().m_timestamp       = 20.0;
    displacedMain.sync();

    const fs::path displacedPath = std::filesystem::temp_directory_path() /
                                   "edge_displaced_main_sample.mc";
    TEST_ASSERT(displacedMain.saveToFile(displacedPath),
                "non-zero main sample should export");
    std::ifstream displacedFile(displacedPath);
    json          displacedJson;
    displacedFile >> displacedJson;
    // timing 使用普通相位，采样不得错误恢复为 237ms 配对 offset。
    const auto displacedSample = std::find_if(displacedJson["note"].begin(),
                                              displacedJson["note"].end(),
                                              isSoundNode);
    TEST_ASSERT(std::abs(displacedJson["time"][0].value("delay", 0.0) -
                         EXPECTED_TIME_MS) < 1e-6 &&
                    displacedSample != displacedJson["note"].end() &&
                    displacedSample->value("offset", 0) != 237,
                "main sample away from time zero must not use paired wrapping");

    XINFO("PASS: First timing delay unwraps with first BPM");
}

/**
 * @brief 验证 note[] 拍号补偿在首拍相位边界仍保持绝对时间。
 *
 * 两个表驱动 case 分别覆盖首 timing 为负时间和首 timing 恰好位于一拍处。
 * 前者应写成 beat 0、delay=100、主 SOUND offset=100，玩家 Note 保持 beat 0；
 * 后者的相位恰好归零，玩家 Note 写在 beat 1，delay 与主 offset 都为零。
 *
 * 每个 case 同时跑 Key 和 Slide，证明相位算法不依赖玩家坐标协议。导出层
 * 检查 beat/delay/offset，重载层再按绝对毫秒查找原首 BPM 与玩家 Note。
 *
 * 边界用例特别防止取模结果把整拍错误表示为一拍 delay，或把负时间直接
 * 写成 Malody 不接受的负拍号。
 */
void test_malody_note_phase_shift_boundaries()
{
    XINFO("=== Test: Playable note phase shift boundaries ===");

    // 120 BPM 提供精确 500ms 拍长，使负 100ms 和整拍边界易于判读。
    constexpr double BPM            = 120.0;
    constexpr double BEAT_LENGTH_MS = 60000.0 / BPM;
    // 每项同时给出导出拍号、首 delay 和主 SOUND offset 三个预期。
    struct TestCase {
        double       firstTimingMs;
        int          expectedBeat;
        double       expectedDelayMs;
        std::int64_t expectedMainOffsetMs;
        const char*  tag;
    };
    // negative_timing 与 zero_phase 分别覆盖负相位和模拍为零。
    const std::array<TestCase, 2> cases{ {
        { -100.0, 0, 100.0, 100, "negative_timing" },
        { BEAT_LENGTH_MS, 1, 0.0, 0, "zero_phase" },
    } };

    for ( const auto& testCase : cases ) {
        for ( const int mode : { 0, 7 } ) {
            // 两种模式复用完全相同的 timing、主音频和玩家绝对时间。
            auto  beatMap                       = makeMinimalBeatMap(mode, 4);
            auto& firstTiming                   = beatMap.m_timings.front();
            firstTiming.m_timestamp             = testCase.firstTimingMs;
            firstTiming.m_bpm                   = BPM;
            firstTiming.m_beat_length           = BEAT_LENGTH_MS;
            firstTiming.m_timingEffectParameter = BPM;
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
                    (*exportedPlayable)["beat"] ==
                        json::array({ testCase.expectedBeat, 0, 1 }),
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
 * @brief 验证成对首拍相位统一补偿多 BPM 时间线和普通内容。
 *
 * 时间线从 210 BPM 的配对首 timing 开始，一百拍后切换到 200 BPM，再过
 * 十拍切回 210 BPM，并在第三红线同刻放置 SCROLL。普通 Note 位于第二红线，
 * 用于验证跨 BPM 的拍号累计与统一正相位补偿。
 *
 * 导出时三条 BPM 应位于 0、101、111 拍，只有首 timing 携带 wrapped delay；
 * Note、SCROLL 和普通内容共享相同的一拍相位前移。重载后四个 timing（含
 * SCROLL）和 Note 必须恢复原绝对时间，主音频保持时间零。
 *
 * 随后将第二、第三 timing、SCROLL 和 Note 各移动一个首 BPM 拍长，再次
 * 保存。新 beat 必须取代导入 metadata 缓存，证明编辑后的模型是权威来源。
 * Key 与 Slide 各执行一次，排除模式字段对拍轴算法的影响。
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
        // SCROLL 不在 time[] 中，因此这里应只有三条 BPM 红线。
        TEST_ASSERT(exported["time"].size() == 3,
                    "variable BPM export should keep three timings");
        TEST_ASSERT(
            exported["time"][0]["beat"] == json::array({ 0, 0, 1 }) &&
                exported["time"][1]["beat"] == json::array({ 101, 0, 1 }) &&
                exported["time"][2]["beat"] == json::array({ 111, 0, 1 }),
            "variable BPM timings should share the positive phase shift");
        // 配对 delay 只出现一次，后续红线不得重复改变拍轴相位。
        TEST_ASSERT(std::abs(exported["time"][0].value("delay", 0.0) -
                             WRAPPED_DELAY_MS) < 1e-6,
                    "only the first timing should carry the paired delay");
        TEST_ASSERT(
            (!exported["time"][1].contains("delay") ||
             std::abs(exported["time"][1].value("delay", 0.0)) < 1e-6) &&
                (!exported["time"][2].contains("delay") ||
                 std::abs(exported["time"][2].value("delay", 0.0)) < 1e-6),
            "later BPM timings should not repeat the paired delay");

        // 主 SOUND 的 wrapped offset 与首 timing delay 成对恢复。
        const auto exportedSample = std::find_if(
            exported["note"].begin(), exported["note"].end(), isSoundNode);
        TEST_ASSERT(
            exportedSample != exported["note"].end() &&
                (*exportedSample)["beat"] == json::array({ 0, 0, 1 }) &&
                exportedSample->value("offset", -1) == 237,
            "first-half-beat main SOUND should keep its wrapped offset");
        // Note 与 SCROLL 分属不同数组，但导出拍号分别对应 101 与 111。
        const auto exportedPlayable =
            std::find_if(exported["note"].begin(),
                         exported["note"].end(),
                         [](const json& node) { return !isSoundNode(node); });
        TEST_ASSERT(
            exportedPlayable != exported["note"].end() &&
                (*exportedPlayable)["beat"] == json::array({ 101, 0, 1 }) &&
                exported["effect"].size() == 1 &&
                exported["effect"][0]["beat"] == json::array({ 111, 0, 1 }),
            "all ordinary content should receive the positive phase shift");

        // 重载统一模型按绝对时间验证分段 BPM 换算，而不是只比较拍号文本。
        MMM::BeatMap reloaded = MMM::BeatMap::loadFromFile(outputPath);
        reloaded.sync();
        TEST_ASSERT(
            reloaded.m_timings.size() == 4,
            "variable BPM round trip should keep BPM and effect timings");
        TEST_ASSERT(
            std::abs(reloaded.m_timings[0].m_timestamp - FIRST_TIMESTAMP_MS) <
                    1e-6 &&
                std::abs(reloaded.m_timings[1].m_timestamp -
                         SECOND_TIMESTAMP_MS) < 1e-6 &&
                std::abs(reloaded.m_timings[2].m_timestamp -
                         THIRD_TIMESTAMP_MS) < 1e-6,
            "variable BPM round trip should preserve every timing timestamp");
        TEST_ASSERT(
            reloaded.m_audioSamples.size() == 1 &&
                std::abs(reloaded.m_audioSamples.front().m_timestamp) < 1e-6 &&
                reloaded.m_audioSamples.front().m_offsetMs == 0,
            "variable BPM round trip should keep main audio normalized");

        // 编辑阶段将除首 timing 外的内容整体后移一个首 BPM 拍长。
        // 这不是原始 beat 的简单复制，保存器必须检测 timestamp 已发生变化。
        reloaded.m_timings[1].m_timestamp += FIRST_BEAT_MS;
        reloaded.m_timings[2].m_timestamp += FIRST_BEAT_MS;
        reloaded.m_timings[3].m_timestamp += FIRST_BEAT_MS;
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
        // 编辑后红线拍号从 101/111 前移到 102/112，首红线仍维持 beat 0。
        TEST_ASSERT(
            editedExported["time"][0]["beat"] == json::array({ 0, 0, 1 }) &&
                editedExported["time"][1]["beat"] ==
                    json::array({ 102, 0, 1 }) &&
                editedExported["time"][2]["beat"] == json::array({ 112, 0, 1 }),
            "edited timings must replace imported beat metadata");
        // 玩家 Note 和 SCROLL 也必须使用编辑后的时间，而非各自缓存的导入 beat。
        const auto editedPlayable =
            std::find_if(editedExported["note"].begin(),
                         editedExported["note"].end(),
                         [](const json& node) { return !isSoundNode(node); });
        TEST_ASSERT(
            editedPlayable != editedExported["note"].end() &&
                (*editedPlayable)["beat"] == json::array({ 102, 0, 1 }) &&
                editedExported["effect"].size() == 1 &&
                editedExported["effect"][0]["beat"] ==
                    json::array({ 112, 0, 1 }),
            "edited objects must replace imported beat metadata");

        // 最终重载确认编辑值经历第二次往返后仍落在新的绝对时间。
        MMM::BeatMap editedReloaded =
            MMM::BeatMap::loadFromFile(editedOutputPath);
        editedReloaded.sync();
        TEST_ASSERT(
            std::abs(editedReloaded.m_timings[1].m_timestamp -
                     (SECOND_TIMESTAMP_MS + FIRST_BEAT_MS)) < 1e-6 &&
                std::abs(editedReloaded.m_timings[2].m_timestamp -
                         (THIRD_TIMESTAMP_MS + FIRST_BEAT_MS)) < 1e-6,
            "edited variable BPM timings should round trip at new positions");
        TEST_ASSERT(
            std::abs(editedReloaded.m_timings[3].m_timestamp -
                     (THIRD_TIMESTAMP_MS + FIRST_BEAT_MS)) < 1e-6 &&
                std::abs(editedReloaded.m_noteData.notes.front().m_timestamp -
                         (SECOND_TIMESTAMP_MS + FIRST_BEAT_MS)) < 1e-6,
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
        exported.contains("effect") && exported["effect"].size() == 1 &&
            exported["effect"][0]["beat"] == json::array({ 1, 1919, 1920 }),
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
    test_late_first_timing_prepends_anchor_and_shifts_all_content();
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
