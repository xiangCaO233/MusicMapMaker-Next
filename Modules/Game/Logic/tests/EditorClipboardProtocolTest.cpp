#include "logic/EditorClipboardProtocol.h"

#include "logic/EditorClipboard.h"
#include "logic/session/context/SessionContext.h"

#include "log/colorful-log.h"
#include <cmath>
#include <glm/glm.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace
{
// 直接验证文本协议与内存剪贴板隔离，不读取操作系统剪贴板或建立协作网络连接。
// 资源名和身份字符串都是测试数据，文件无需存在，也不在这里执行粘贴后的模型编辑。
using MMM::Logic::ClipboardItem;
using MMM::Logic::SampleClipboardItem;
using MMM::Logic::TimelineClipboardItem;

/// @brief 使用小容差比较有限浮点数。
/// @param lhs 解析后的时间、拍位或颜色通道。
/// @param rhs 原始夹具值或明确的兼容默认值。
/// @return 差值严格小于容差时返回 true，非有限差值不通过。
bool near(double lhs, double rhs)
{
    // float 字段提升后仍与原夹具比较，避免另写 double
    // 小数常量引入不同精度的基线。
    // 只比较数值往返精度，不把格式输出字符串的十进制写法固定成唯一表示。
    return std::abs(lhs - rhs) < 1e-9;
}

/// @brief 比较两个可选采样绑定的资源与物件音量。
/// @param lhs 解析后的绑定，可以为空。
/// @param rhs 期望绑定，可以为空。
/// @return 存在性一致，且有值时资源名和音量均一致。
bool sameBinding(const std::optional<MMM::AudioSampleBinding>& lhs,
                 const std::optional<MMM::AudioSampleBinding>& rhs)
{
    // 此比较不检查资源能否加载，非空但不存在的文件名仍可作为合法往返测试值。
    // 资源名逐字匹配，音量才使用数值容差；不能规范化路径后忽略协议字符串变化。
    if ( lhs.has_value() != rhs.has_value() ) {
        // 丢失绑定与保留一个空资源对象不同，先检查 optional 的状态。
        return false;
    }
    // 两边都为空直接成功，只有存在性一致且有值后才解引用右侧。
    return !lhs || (lhs->m_audioResourceId == rhs->m_audioResourceId &&
                    near(lhs->m_volume, rhs->m_volume));
}

/// @brief 以满足协议往返验证的精度比较可选颜色。
/// @param lhs 解析得到的颜色覆盖值。
/// @param rhs 原组件的颜色覆盖值。
/// @return 覆盖存在性和四个 RGBA 通道均匹配时返回 true。
bool sameColor(const std::optional<glm::vec4>& lhs,
               const std::optional<glm::vec4>& rhs)
{
    // 不把颜色夹到显示范围，协议数值失真应直接令比较失败而非被助手纠正。
    // 不从皮肤读取缺省颜色补齐覆盖，避免协议丢字段后仍取得视觉上接近的值。
    if ( lhs.has_value() != rhs.has_value() ) {
        return false;
    }
    if ( !lhs ) {
        // 没有覆盖表示继续使用皮肤色，不等同于存在一个透明颜色覆盖。
        return true;
    }
    // alpha 也属于协议载荷，不能只验证 RGB 而让透明度在复制时丢失。
    return near(lhs->r, rhs->r) && near(lhs->g, rhs->g) &&
           near(lhs->b, rhs->b) && near(lhs->a, rhs->a);
}

/// @brief 不抛异常地读取一个音符元数据值。
/// @param metadata 父物件或节点携带的格式扩展信息。
/// @param source 应保存该键的来源格式。
/// @param key 要检查的键，查询不创建缺失分组或条目。
/// @return 原始字符串副本，来源或键缺失时返回空 optional。
std::optional<std::string> noteMetadataValue(const MMM::NoteMetadata& metadata,
                                             MMM::NoteMetadataType    source,
                                             const std::string&       key)
{
    const auto sourceIt = metadata.note_properties.find(source);
    // 使用 find 保持原元数据不变，不把测试查询变成一次写入默认值的操作。
    // 按来源分组查找，不能从另一种格式的同名字段取后备值掩盖归属丢失。
    if ( sourceIt == metadata.note_properties.end() ) {
        return std::nullopt;
    }
    const auto valueIt = sourceIt->second.find(key);
    if ( valueIt == sourceIt->second.end() ) {
        return std::nullopt;
    }
    return valueIt->second;
}

/// @brief 不抛异常地读取一个时间线元数据值。
/// @param metadata 已解析时间点的格式扩展字段。
/// @param source 时间线元数据的来源类型，不使用音符元数据枚举替代。
/// @param key 来源分组内的字段名称。
/// @return 保留原内容的字符串副本，找不到时返回空。
std::optional<std::string> timingMetadataValue(
    const MMM::TimingMetadata& metadata, MMM::TimingMetadataType source,
    const std::string& key)
{
    const auto sourceIt = metadata.timing_properties.find(source);
    // "0" 等内容也可以是有效字段，是否存在由键查询决定，不按内容真假判断。
    if ( sourceIt == metadata.timing_properties.end() ) {
        return std::nullopt;
    }
    const auto valueIt = sourceIt->second.find(key);
    if ( valueIt == sourceIt->second.end() ) {
        return std::nullopt;
    }
    return valueIt->second;
}

/// @brief 不抛异常地读取一个自动采样元数据值。
/// @param metadata 自动采样专用元数据，不借用玩家音符属性表。
/// @param source 待验证的来源格式。
/// @param key 期望保留的资源扩展字段。
/// @return 有值表示键存在，空字符串本身仍属于一个存在的字段。
std::optional<std::string> sampleMetadataValue(
    const MMM::SampleMetadata& metadata, MMM::SampleMetadataType source,
    const std::string& key)
{
    const auto sourceIt = metadata.sample_properties.find(source);
    // 读取助手必须保持只读，不能通过 map 下标插入默认值而改变测试结果。
    if ( sourceIt == metadata.sample_properties.end() ) {
        return std::nullopt;
    }
    const auto valueIt = sourceIt->second.find(key);
    if ( valueIt == sourceIt->second.end() ) {
        return std::nullopt;
    }
    return valueIt->second;
}

/// @brief 验证音符剪贴板载荷的序列化和解析。
/// @return 公开身份过滤与显式保留身份的往返都满足断言时返回 true。
/// @note 只检查本场景列出的字段，子节点元数据等已构造字段并未全部逐项比较。
bool testNoteRoundTrip()
{
    // 身份检查只涉及文本值，不调用身份分配器或建立真实协作会话。
    // 协议保留载荷字段，本测试不验证折线几何是否满足全部编辑业务约束。
    // 先用同一父子结构测试公开导出，再显式保留身份测试完整往返，避免两套夹具漂移。
    ClipboardItem item;
    item.note.m_type            = MMM::NoteType::POLYLINE;
    item.note.m_timestamp       = 12.5;
    item.note.m_duration        = 1.25;
    item.note.m_trackIndex      = 2;
    item.note.m_dtrack          = 1;
    item.note.m_collaborationId = "root%identity";
    // 百分号、制表符与换行分别进入身份、资源名、批注和元数据，覆盖文本转义边界。
    // 这些字符不能被误认成记录分隔符或丢失为经过裁剪的展示文本。
    item.note.m_annotation = "整条折线\n待复核";
    item.note.m_sampleBinding =
        MMM::AudioSampleBinding{ "main\tbound.wav", 0.35F };
    item.note.m_metadata
        .note_properties[MMM::NoteMetadataType::MMM]["authorNote"] =
        "copy\tline\n%";
    item.note.m_customColors.tap = glm::vec4{ 0.1F, 0.2F, 0.3F, 1.0F };
    // 父级与两个节点分别设置 tap、head、flickArrow，检测类型专属覆盖槽位分派。

    // 父折线与两个不同类型节点各有独立字段，复制不能把父绑定套给所有节点。
    MMM::Logic::NoteComponent::SubNote hold;
    hold.type            = MMM::NoteType::HOLD;
    hold.timestamp       = 13.0;
    hold.duration        = 0.75;
    hold.trackIndex      = 3;
    hold.dtrack          = 0;
    hold.collaborationId = "hold-identity";
    hold.annotation      = "起段\t备注";
    // 子节点批注带分隔符，与父批注的换行形成独立转义输入，不能只处理顶层字符串。
    hold.sampleBinding = MMM::AudioSampleBinding{ "hold.wav", 0.45F };
    hold.metadata.note_properties[MMM::NoteMetadataType::OSU]["edge"] = "hold";
    // 三个绑定的资源名和音量不同，误复用父级或上一节点绑定都会改变比较结果。
    hold.customColors.head = glm::vec4{ 0.4F, 0.5F, 0.6F, 1.0F };

    // 第二节点使用负轨差，检查滑键方向符号不会在文本数值往返中消失。
    MMM::Logic::NoteComponent::SubNote flick;
    flick.type       = MMM::NoteType::FLICK;
    flick.timestamp  = 14.0;
    flick.duration   = 0.0;
    flick.trackIndex = 5;
    flick.dtrack     = -1;
    // Flick 不带持续时间，但仍有位移方向；HOLD 的非零时长不能泄漏到这一节点。
    flick.collaborationId = "flick-identity";
    flick.sampleBinding   = MMM::AudioSampleBinding{ "flick.wav", 0.55F };
    flick.metadata.note_properties[MMM::NoteMetadataType::MALODY]["sound"] =
        "snap";
    flick.customColors.flickArrow = glm::vec4{ 0.7F, 0.8F, 0.9F, 1.0F };

    item.note.m_subNotes = { hold, flick };
    // 节点按值复制进载荷，源 hold/flick 仍可作为独立期望对象参与比较。
    // 节点时间与拍位数组采用同一顺序，不能分别按类型排序而破坏下标对应关系。
    item.startBeat     = 24.0;
    item.endBeat       = 26.0;
    item.subStartBeats = { 25.0, 26.0 };
    item.subEndBeats   = { 25.5, 26.0 };
    // 25.5 保留分数拍，避免只用整数夹具而漏掉拍位被截断的问题。
    item.hasBeatPositions = true;
    // 拍位是复制时附带的数据；本场景不提供 BPM 环境，不应在解析时重新推导拍位。

    // 默认公开导出剥离协作身份，防止普通跨谱面粘贴复用原对象的逻辑身份。
    const auto publicText =
        MMM::Logic::EditorClipboardProtocol::serializeNotes({ item });
    const auto publicParsed =
        MMM::Logic::EditorClipboardProtocol::parse(publicText);
    // 默认调用策略单独验证，不能以两端都开启身份保留的结果证明公开导出安全。
    // 本断言具体检查父身份和首个节点身份为空，未逐一检查第二节点的公开身份字段。
    // 同时要求父物件与两个节点存在，不能以解析成空列表实现“身份已清空”。
    if ( !publicParsed || publicParsed->notes.size() != 1U ||
         publicParsed->notes.front().note.m_subNotes.size() != 2U ||
         !publicParsed->notes.front().note.m_collaborationId.empty() ||
         !publicParsed->notes.front()
              .note.m_subNotes.front()
              .collaborationId.empty() ) {
        XERROR("Public clipboard payload retained collaboration identities");
        return false;
    }

    const std::string text =
        MMM::Logic::EditorClipboardProtocol::serializeNotes({ item }, true);
    // 私有往返显式开启身份保留；解析端也要选择对应模式，不与公开路径混用。
    if ( !text.starts_with(MMM::Logic::EditorClipboardProtocol::MAGIC) ||
         text.find('{') != std::string::npos ||
         text.find("\"format\"") != std::string::npos ) {
        // 对本夹具检查协议标记和非 JSON
        // 外观，不把任意文本无花括号等同于合法协议。
        XERROR("Note clipboard protocol still looks like JSON");
        return false;
    }

    auto parsed = MMM::Logic::EditorClipboardProtocol::parse(text, true);
    // true 只控制协议字段保留，不是用户权限认证，本测试不验证协作授权。
    // text 在同步解析完成前保持有效，不把临时字符串视图交给跨帧消费者。
    // 音符类型载荷只能产生
    // notes；不能因公共解析器支持混合对象就多出空白其他对象。
    if ( !parsed || parsed->notes.size() != 1 || !parsed->samples.empty() ||
         !parsed->timelines.empty() ) {
        XERROR("Note clipboard protocol did not parse one note item");
        return false;
    }

    const auto& parsedItem = parsed->notes.front();
    // parsed 在所有引用使用期间保持存活，不把解析结果中的组件引用保存到场景外。
    const auto& note = parsedItem.note;
    // 解析出的组件独立拥有字符串和节点值，不通过原始 item 的引用验证自身字段。
    if ( note.m_type != MMM::NoteType::POLYLINE ||
         !near(note.m_timestamp, 12.5) || !near(note.m_duration, 1.25) ||
         note.m_trackIndex != 2 || note.m_dtrack != 1 ||
         note.m_collaborationId != "root%identity" ||
         note.m_annotation != "整条折线\n待复核" ||
         !sameBinding(note.m_sampleBinding, item.note.m_sampleBinding) ||
         note.m_subNotes.size() != 2 ) {
        XERROR("Note clipboard protocol changed core note fields");
        return false;
    }
    if ( !sameColor(note.m_customColors.tap, item.note.m_customColors.tap) ) {
        // 期望颜色来自源组件，保留原 float 精度，不要求固定的十进制文本写法。
        // 颜色覆盖丢失与解码为全零不同，两者都应由 optional
        // 存在性和通道比较识别。
        XERROR("Note clipboard protocol changed note color override");
        return false;
    }
    const auto noteMetadata = noteMetadataValue(
        note.m_metadata, MMM::NoteMetadataType::MMM, "authorNote");
    // 逐字检查带分隔符的内容，避免仅确认 metadata 非空而漏掉转义损坏。
    if ( !noteMetadata || *noteMetadata != "copy\tline\n%" ) {
        // 转义应还原原字符，保留百分号编码文本不能被视为相同元数据。
        XERROR("Note clipboard protocol changed note metadata");
        return false;
    }
    if ( note.m_subNotes[0].type != MMM::NoteType::HOLD ||
         // 第一节点有独立批注、绑定和头部颜色，不能被父折线属性覆盖。
         !near(note.m_subNotes[0].timestamp, 13.0) ||
         note.m_subNotes[0].collaborationId != "hold-identity" ||
         note.m_subNotes[0].annotation != "起段\t备注" ||
         !sameBinding(note.m_subNotes[0].sampleBinding, hold.sampleBinding) ||
         !sameColor(note.m_subNotes[0].customColors.head,
                    hold.customColors.head) ) {
        XERROR("Note clipboard protocol changed first sub note");
        // 本分支未逐项验证 HOLD
        // 时长与轨号，不能将通过结果表述为所有子字段均相等。
        return false;
    }
    if ( note.m_subNotes[1].type != MMM::NoteType::FLICK ||
         // 第二节点箭头颜色与第一节点头部颜色属于不同可选覆盖槽位。
         note.m_subNotes[1].dtrack != -1 ||
         note.m_subNotes[1].collaborationId != "flick-identity" ||
         !sameBinding(note.m_subNotes[1].sampleBinding, flick.sampleBinding) ||
         !sameColor(note.m_subNotes[1].customColors.flickArrow,
                    flick.customColors.flickArrow) ) {
        XERROR("Note clipboard protocol changed second sub note");
        // 类型和轨差是此节点的关键断言，其他未比较字段不由这条错误消息扩展覆盖。
        return false;
    }
    if ( !parsedItem.hasBeatPositions || !near(parsedItem.startBeat, 24.0) ||
         // beat 数值描述节奏定位，不可拿秒时间字段代替，即使时间也成功往返。
         !near(parsedItem.endBeat, 26.0) ||
         parsedItem.subStartBeats.size() != 2 ||
         !near(parsedItem.subStartBeats[1], 26.0) ||
         parsedItem.subEndBeats.size() != 2 ||
         !near(parsedItem.subEndBeats[0], 25.5) ) {
        XERROR("Note clipboard protocol changed beat offsets");
        // 拍位数组先检查长度再读指定下标，缺失数组应报告失败而不是越界。
        return false;
    }

    return true;
}

/// @brief 验证负轨道剪贴板物件会恢复为草稿域。
/// @return 单个负轨物件保留 -3 且恢复草稿标志时返回 true。
/// @note 直接写协议文本，避免序列化器提前补全草稿状态而掩盖解析兼容逻辑。
bool testNegativeTrackRestoresDraftDomain()
{
    // 单个物件数量断言也防止负号被误读后产生多余记录。
    // 只验证有效负轨地址，不覆盖草稿轨数量变化后的坐标重新投影。
    // -3 必须原样保存，不能只设草稿标志却把实际轨道夹到玩家零号轨。
    // 载荷没有显式构造运行时 m_isDraft，解析器需从协议轨道语义恢复区域。
    constexpr std::string_view payload =
        "MMM_CLIPBOARD_V4\tN\n"
        "N\tn\t1\t0\t-3\t0\t0\t-1\n";
    const auto parsed = MMM::Logic::EditorClipboardProtocol::parse(payload);
    if ( !parsed || parsed->notes.size() != 1 ||
         !parsed->notes.front().note.m_isDraft ||
         parsed->notes.front().note.m_trackIndex != -3 ) {
        XERROR("Negative clipboard track did not restore the draft domain");
        return false;
    }
    return true;
}

/// @brief 验证 V4 混合谱面物件载荷保留自动采样关键字段和相对 BGM 轨道。
/// @return 两种对象均被读出，且自动采样的指定字段保持不变时返回 true。
/// @note 混合测试对普通音符仅检查数量，其完整字段由音符专用往返场景覆盖。
bool testMixedChartObjectRoundTrip()
{
    // original_x 为 11，当前轨与相对轨为 7，元数据与轨号不可相互覆盖。
    // 自动采样作为独立对象保存，不应被转换成普通音符的绑定字段。
    // 本场景 track 与 bgmLane
    // 恰好同值，只检查二者保留，不验证不同值之间的换算。
    // 普通音符和采样的起点不同，混合协议不能把一种对象的时间复用于另一种对象。
    ClipboardItem note;
    note.note.m_timestamp  = 12.5;
    note.note.m_trackIndex = 2;
    note.startBeat         = 25.0;
    note.hasBeatPositions  = true;

    SampleClipboardItem sample;
    sample.sample.m_timestamp = 10.25;
    sample.sample.m_offsetMs  = -125;
    // 时间戳以秒保存、偏移以毫秒保存，负偏移不能被误写成负的绝对触发时间。
    sample.sample.m_track           = 7;
    sample.sample.m_audioResourceId = "stem\tlayer.ogg";
    // 制表符位于资源标识内部，往返后应仍是一个标识而非多出协议字段。
    sample.sample.m_volume = 0.65F;
    // original_x 是格式元数据，不应替代独立存储的当前轨号。
    sample.sample.m_metadata
        .sample_properties[MMM::SampleMetadataType::MALODY]["original_x"] =
        "11";
    sample.bgmLane = 7;
    // 组件轨号与剪贴板相对 BGM 轨字段分别存储，解析不得漏掉其中一种表示。
    sample.startBeat       = 20.5;
    sample.hasBeatPosition = true;
    // hasBeatPosition 参与判断是否可用拍位粘贴，不能只写出 beat
    // 数值而丢掉有效标志。

    const std::string text =
        MMM::Logic::EditorClipboardProtocol::serializeChartObjects({ note },
                                                                   { sample });
    if ( !text.starts_with("MMM_CLIPBOARD_V4\tC\n") ) {
        // 版本与载荷类别同时检查，不仅要求出现通用 MAGIC 前缀。
        // C 表示混合谱面对象载荷，不能继续输出只支持音符的 N 类型头。
        XERROR("Mixed chart-object clipboard did not emit a V4 payload");
        return false;
    }

    const auto parsed = MMM::Logic::EditorClipboardProtocol::parse(text);
    if ( !parsed || parsed->notes.size() != 1 || parsed->samples.size() != 1 ||
         !parsed->timelines.empty() ) {
        XERROR("Mixed chart-object clipboard did not parse both object kinds");
        // 同时限制 timelines 为空，防止混合类型行被错误路由到时间线列表。
        return false;
    }

    const auto& parsedSample = parsed->samples.front();
    // 数量检查在 front() 之前，失败载荷应返回测试失败而非导致助手越界。
    const auto originalX = sampleMetadataValue(parsedSample.sample.m_metadata,
                                               MMM::SampleMetadataType::MALODY,
                                               "original_x");
    if ( !near(parsedSample.sample.m_timestamp, 10.25) ||
         // 毫秒偏移是整数，精确比较避免对它套用浮点容差而忽略舍入变化。
         parsedSample.sample.m_offsetMs != -125 ||
         parsedSample.sample.m_track != 7 || parsedSample.bgmLane != 7 ||
         parsedSample.sample.m_audioResourceId != "stem\tlayer.ogg" ||
         !near(parsedSample.sample.m_volume, sample.sample.m_volume) ||
         !near(parsedSample.startBeat, 20.5) || !parsedSample.hasBeatPosition ||
         !originalX || *originalX != "11" ) {
        XERROR(
            "Mixed chart-object clipboard changed automatic sample fields: "
            "timestamp={}, offset={}, track={}, lane={}, resource='{}', "
            "volume={}, beat={}, hasBeat={}, originalX='{}'",
            parsedSample.sample.m_timestamp,
            parsedSample.sample.m_offsetMs,
            parsedSample.sample.m_track,
            parsedSample.bgmLane,
            parsedSample.sample.m_audioResourceId,
            parsedSample.sample.m_volume,
            parsedSample.startBeat,
            parsedSample.hasBeatPosition,
            originalX.value_or("<missing>"));
        // 缺失元数据用日志占位符展示，不把它写回解析结果或当作有效协议值。
        return false;
    }
    return true;
}

/// @brief 验证 V3 音符绑定音量载荷仍可读取。
/// @return V3 音符与时间线示例均能解析，指定字段符合预期时返回 true。
/// @note 使用固定历史载荷，而非修改当前序列化结果的版本头冒充旧协议。
bool testLegacyV3Payload()
{
    // 两种旧载荷都用默认解析入口，不要求调用方先识别版本再选择专用函数。
    // 历史兼容只调用读取入口，不要求当前序列化器继续输出 V3。
    // 明确音量 0.375 与默认 1 不同，可识别读旧列和忽略旧列两种行为。
    // 历史文本由字面量固定，每次升级当前写出格式时仍保持这一读取输入不变。
    // NS 行显式携带音量，V3 读取不能套用 V2 缺省单位音量的规则。
    constexpr std::string_view notePayload =
        "MMM_CLIPBOARD_V3\tN\n"
        "N\tn\t1\t0\t2\t0\t0\t-1\n"
        "NS\tlegacy-v3.wav\t0.375\n";

    const auto notes = MMM::Logic::EditorClipboardProtocol::parse(notePayload);
    // 检查绑定资源和非单位音量，避免读取器仅跳过 NS
    // 行仍保留一个普通音符而通过。
    if ( !notes || notes->notes.size() != 1 || !notes->samples.empty() ||
         !notes->notes.front().note.m_sampleBinding ||
         notes->notes.front().note.m_sampleBinding->m_audioResourceId !=
             "legacy-v3.wav" ||
         !near(notes->notes.front().note.m_sampleBinding->m_volume, 0.375) ) {
        XERROR("Legacy V3 note clipboard payload was rejected");
        return false;
    }

    constexpr std::string_view timelinePayload =
        // 同一旧版本的 T 类型也必须支持，不以音符分支通过替代时间线兼容。
        "MMM_CLIPBOARD_V3\tT\n"
        "T\t4\tb\t150\t0\t0\t1\n";
    const auto timelines =
        MMM::Logic::EditorClipboardProtocol::parse(timelinePayload);
    // 独立解析第二份载荷，不依赖上一份音符的当前对象或记录类型状态。
    if ( !timelines || timelines->timelines.size() != 1 ||
         !near(timelines->timelines.front().timeline.m_timestamp, 4.0) ||
         !near(timelines->timelines.front().timeline.m_value, 150.0) ) {
        XERROR("Legacy V3 timeline clipboard payload was rejected");
        // 时间与 BPM 数值采用不同值，可识别 T 行字段位置错读。
        return false;
    }
    return true;
}

/// @brief 验证 V2 采样资源行可读取并为缺失的物件音量补 1。
/// @return 父物件与子节点绑定均存在且音量为单位增益时返回 true。
/// @note 资源行省略音量列是旧版合法形状，不等同于当前版本损坏数值。
bool testLegacyV2SampleBindingDefaultsVolume()
{
    // 两条资源名不同，绑定归属不能仅靠父子音量都为 1 判断。
    // 父绑定后接子绑定，测试解析器切换当前节点时仍把后续记录附在正确对象上。
    // 旧载荷使用缺列而非空字符串表达缺少音量，不能改写成新版本的显式 1.0。
    // %09 表示资源名中的制表符，兼容路径也要保留旧版转义的解码行为。
    constexpr std::string_view payload =
        "MMM_CLIPBOARD_V2\tN\n"
        "N\tn\t1\t0\t2\t0\t0\t-1\n"
        "NS\tlegacy%09main.wav\n"
        "S\th\t2\t0.5\t3\t0\n"
        "SS\tlegacy-hold.wav\n";

    const auto parsed = MMM::Logic::EditorClipboardProtocol::parse(payload);
    if ( !parsed || parsed->notes.size() != 1 ) {
        XERROR("Legacy V2 note clipboard payload was rejected");
        return false;
    }

    const auto& note = parsed->notes.front().note;
    // 父绑定和节点绑定分别断言默认音量，不能只修正顶层兼容路径。
    // NS 属于父物件，SS 属于紧前的子节点，不能把两条绑定都附到父节点上。
    if ( !note.m_sampleBinding ||
         note.m_sampleBinding->m_audioResourceId != "legacy\tmain.wav" ||
         !near(note.m_sampleBinding->m_volume, 1.0) ||
         note.m_subNotes.size() != 1 ||
         !note.m_subNotes.front().sampleBinding ||
         note.m_subNotes.front().sampleBinding->m_audioResourceId !=
             "legacy-hold.wav" ||
         !near(note.m_subNotes.front().sampleBinding->m_volume, 1.0) ) {
        XERROR("Legacy V2 sample binding did not default volume to one");
        return false;
    }
    return true;
}

/// @brief 验证超出 float 范围的绑定音量不会产生无穷值。
/// @return 物件和子节点仍可解析，但两条越界音量绑定都被舍弃时返回 true。
/// @note 不是要求整份载荷失败；有效的主体结构应继续保留。
bool testOutOfRangeSampleBindingVolumeRejected()
{
    // 正溢出置于父绑定、负溢出置于子绑定，同时覆盖两种记录位置。
    // 1e100 是合法数字文本，测试 float 可表示范围，不是非数字字符串拒绝。
    // 绑定被拒绝不应导致节点数量变化，结构保留与数值拒绝必须同时成立。
    // 数值可由 double 表达但无法放入 float，覆盖窄化边界而非普通文本解析失败。
    // 正负两侧都设置越界值，不能只检查大正值而让负溢出生成无穷增益。
    constexpr std::string_view payload =
        "MMM_CLIPBOARD_V4\tN\n"
        "N\tn\t1\t0\t2\t0\t0\t-1\n"
        "NS\tmain.wav\t1e100\n"
        "S\th\t2\t0.5\t3\t0\n"
        "SS\thold.wav\t-1e100\n";

    const auto parsed = MMM::Logic::EditorClipboardProtocol::parse(payload);
    if ( !parsed || parsed->notes.size() != 1 ||
         parsed->notes.front().note.m_sampleBinding ||
         parsed->notes.front().note.m_subNotes.size() != 1 ||
         parsed->notes.front().note.m_subNotes.front().sampleBinding ) {
        XERROR("Clipboard protocol accepted an out-of-range binding volume");
        // 判断不存在绑定，而非只检测音量
        // isfinite，防止截断为任意有限值冒充合法输入。
        return false;
    }
    return true;
}

/// @brief 验证时间线剪贴板载荷的序列化和解析。
/// @return 单个 BPM 时间点、相对定位与指定元数据完整恢复时返回 true。
/// @note 相对秒时间和相对拍位是两套粘贴定位依据，需要独立保留。
bool testTimelineRoundTrip()
{
    // 明确断言 BPM 类型，数值 180 不能被当成 SV 倍率留在错误效果里。
    // inherited 保持来源格式的字符串，不在协议层解释成新的 TimingEffect。
    // 相对定位属于 ClipboardItem，不能仅移入 metadata 字符串而丢失结构化字段。
    // 时间点 metadata
    // 使用独立来源枚举与属性表，不能借音符元数据接口获得偶然匹配。 绝对时间
    // 48、相对时间 1.5 与相对拍位 3 故意不同，便于识别字段错位。
    TimelineClipboardItem item;
    item.timeline.m_timestamp = 48.0;
    item.timeline.m_effect    = MMM::TimingEffect::BPM;
    item.timeline.m_value     = 180.0;
    item.timeline.m_metadata
        .timing_properties[MMM::TimingMetadataType::OSU]["inherited"] = "0";
    item.relativeTime                                                 = 1.5;
    item.relativeBeat                                                 = 3.0;
    item.hasBeatPosition                                              = true;

    const std::string text =
        MMM::Logic::EditorClipboardProtocol::serializeTimelines({ item });
    if ( !text.starts_with(MMM::Logic::EditorClipboardProtocol::MAGIC) ||
         text.find('{') != std::string::npos ||
         text.find("\"format\"") != std::string::npos ) {
        XERROR("Timeline clipboard protocol still looks like JSON");
        return false;
    }

    auto parsed = MMM::Logic::EditorClipboardProtocol::parse(text);
    // 整个 parse 结果有值后才读取列表；无结果与一个空载荷是不同失败状态。
    if ( !parsed || parsed->timelines.size() != 1 || !parsed->notes.empty() ||
         !parsed->samples.empty() ) {
        XERROR("Timeline clipboard protocol did not parse one timeline item");
        return false;
    }

    const auto& parsedItem = parsed->timelines.front();
    // T 载荷不应混入音符或自动采样，先通过前面的类型分组断言再读取时间点。
    if ( !near(parsedItem.timeline.m_timestamp, 48.0) ||
         parsedItem.timeline.m_effect != MMM::TimingEffect::BPM ||
         !near(parsedItem.timeline.m_value, 180.0) ||
         !near(parsedItem.relativeTime, 1.5) ||
         !near(parsedItem.relativeBeat, 3.0) || !parsedItem.hasBeatPosition ) {
        XERROR("Timeline clipboard protocol changed timeline fields");
        return false;
    }
    const auto timingMetadata =
        timingMetadataValue(parsedItem.timeline.m_metadata,
                            MMM::TimingMetadataType::OSU,
                            "inherited");
    if ( !timingMetadata || *timingMetadata != "0" ) {
        // 期望键必须仍归属 OSU 来源，其他分组中的同名字段不能替代它。
        // 字符串 "0" 是有效的格式元数据，不应按布尔假值当作缺失字段删除。
        XERROR("Timeline clipboard protocol changed metadata");
        return false;
    }

    return true;
}

/// @brief 验证 MMM 剪贴板解析器会忽略普通文本。
/// @return 无协议标记的文本不产生解析结果时返回 true。
/// @note 这里只验证一条普通文本，不等同于所有损坏载荷的拒绝测试。
bool testPlainTextIgnored()
{
    // 拒绝结果留给调用方选择其他文本行为，测试不主动清空真实系统剪贴板。
    // 与文本编辑器共享系统剪贴板时，普通文字不应被误认成可粘贴的谱面对象。
    // 空 optional 表示不属于本协议，不能返回“成功但没有物件”的载荷对象。
    if ( MMM::Logic::EditorClipboardProtocol::parse("plain text") ) {
        XERROR("Clipboard protocol parser accepted plain text");
        return false;
    }
    return true;
}

/// @brief 验证协作剪贴板不会导出到系统或跨 Session 泄漏。
/// @return 普通会话可共享，协作会话仅源上下文可读且清理后不可读时返回 true。
/// @note 系统导出只检查待提交文本队列，不调用操作系统剪贴板 API。
bool testCollaborationClipboardIsolation()
{
    // 新建内存剪贴板不继承其他场景的待导出文本，队列状态只由本场景 set 决定。
    // 这里直接操作内存
    // Clipboard，待系统文本队列只是导出意图，不是真实系统写入。
    MMM::Logic::EditorClipboard clipboard;
    MMM::Logic::SessionContext  localSource;
    MMM::Logic::SessionContext  localTarget;
    ClipboardItem               item;
    item.note.m_timestamp = 12.5;

    clipboard.set({ item }, &localSource, false);
    // localSource 与 localTarget 是不同上下文，正向基线覆盖普通跨 Session
    // 读取。
    // 普通复制先作为共享正向基线，否则“所有读取都拒绝”也可能通过隔离断言。
    if ( clipboard.get(&localTarget).size() != 1U ||
         !clipboard.consumePendingSystemText() ) {
        // 这里只验证存在导出文本，其具体编码由前面的协议往返场景检查。
        XERROR("Local clipboard no longer crosses ordinary sessions");
        return false;
    }

    MMM::Logic::SessionContext collaborationSource;
    // 上一次公开文本已通过 consume 消费，后面应观察到隔离 set
    // 不再生成新导出文本。
    collaborationSource.collaborationClipboardIsolated = true;
    collaborationSource.collaborationClipboardScopeId  = 41U;
    MMM::Logic::SessionContext otherCollaborationSession;
    // 使用不同对象而非引用别名，即使 scopeId 相同也要验证上下文身份边界。
    otherCollaborationSession.collaborationClipboardIsolated = true;
    otherCollaborationSession.collaborationClipboardScopeId  = 41U;
    // 两个隔离会话使用相同
    // scopeId，仍不得共享；上下文身份也是隔离边界的一部分。
    clipboard.set({ item }, &collaborationSource, true);
    // 再次 set 覆盖上一份普通载荷，隔离检查观察的是新设定的源会话数据。
    // true 表示剪切，除了内容隔离还需保留源会话剪切身份并禁止跨会话取得剪切源。
    if ( clipboard.consumePendingSystemText() ||
         // 源会话仍须可读，不能以清空全部载荷的方式通过跨会话拒绝断言。
         clipboard.get(&collaborationSource).size() != 1U ||
         !clipboard.get(&localTarget).empty() ||
         !clipboard.get(&otherCollaborationSession).empty() ||
         !clipboard.isCutFrom(&collaborationSource) ||
         clipboard.getCrossSessionCutSource(&localTarget) ) {
        // 内容读取和剪切源访问分别隔离，防止拒绝内容后仍暴露跨会话源对象。
        XERROR("Collaboration clipboard escaped its source session");
        return false;
    }

    clipboard.clearForContext(&collaborationSource);
    // 保持源上下文对象仍存活以查询清理结果，不利用悬空指针判断载荷是否已失效。
    // 显式清理模拟源会话退出，不能让隔离载荷在该上下文清空后继续可读。
    if ( !clipboard.get(&collaborationSource).empty() ) {
        // 只验证清理已保存的源上下文，不覆盖清理无关会话时应保留数据的规则。
        // 清理后源会话自身也应无数据，不是仅关闭跨会话出口。
        XERROR("Collaboration clipboard survived session cleanup");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行当前协议往返、历史版本兼容及协作剪贴板隔离回归。
/// @return 全部场景通过返回 0，首个失败场景返回 1。
/// @note 输入均由本文件构造，不依赖系统剪贴板中已有的用户内容。
int main()
{
    // 每个测试返回值都参与最终结果，不以解析日志是否安静替代成功条件。
    // 退出零证明固定协议与内存隔离用例通过，不包含系统粘贴或粘贴后物件创建。
    // 一次运行覆盖全部固定用例，不生成随机载荷或按机器环境跳过某种协议版本。
    // 按场景独立执行并短路失败，不在后续测试中复用先前的解析结果。
    if ( !testNoteRoundTrip() ) return 1;
    if ( !testNegativeTrackRestoresDraftDomain() ) return 1;
    if ( !testMixedChartObjectRoundTrip() ) return 1;
    if ( !testLegacyV3Payload() ) return 1;
    if ( !testLegacyV2SampleBindingDefaultsVolume() ) return 1;
    if ( !testOutOfRangeSampleBindingVolumeRejected() ) return 1;
    if ( !testTimelineRoundTrip() ) return 1;
    if ( !testPlainTextIgnored() ) return 1;
    if ( !testCollaborationClipboardIsolation() ) return 1;
    return 0;
}
