#include "ui/walkthrough/ComposeLessonCatalog.h"
#include "config/Utf8Path.h"
#include "logic/EditorClipboardProtocol.h"
#include "ui/walkthrough/WalkthroughModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>

/// @file ComposeLessonCatalog.cpp
/// @brief 将示例谱面的教学标记与项目草稿载荷转换成可执行引导。
/// @details 文件承担一次性的资源装载与几何验收，不访问运行中的会话。
/// 教学描述来自 .mmm 的时间戳批注，草稿几何来自项目侧车数据。
/// 两份输入的职责不同：批注决定播放边界，草稿决定最终物件形态。
/// 任何一份缺失都会使教程成为占位主题，并把原因显示在欢迎页。
/// 这样用户不会在缺少参考的练习中等待一个永远无法满足的目标。
///
/// 目录生成遵循以下契约：
/// - 相邻两个时间戳标记圈定一个段落；
/// - 结束标记只要求结束后缀，不要求与起点描述完全相同；
/// - 进阶前缀首次出现后，后续段落持续属于进阶分支；
/// - 每个段落固定生成预览、练习、复播三个受业务验收的步骤；
/// - 分支内部保持线性依赖，两个分支各自从首段独立启动；
/// - 静态 JSON 只保存主题身份和说明，不再携带旧随机绘制流程。
///
/// 参考几何遵循以下契约：
/// - 项目草稿侧车用剪贴板协议保存非负的草稿相对轨道；
/// - 草稿与主轨道按紧邻边界的轨道对齐，多出的草稿轨道位于左侧；
/// - 只验收音符类型、时间、轨道、有效持续长度及折线全部子段；
/// - 折线父级的持续长度和横移缓存不定义路径，以子段几何为准；
/// - 配色、音效、协作 ID 和 ECS 实体身份不属于教程目标；
/// - 数量与一对一匹配同时检查，重叠物件不能重复抵扣参考。
///
/// 本文件的 JSON 与剪贴板解析都发生在服务构造时。
/// 运行中的 UI 仅持有已编译的步骤和参考值，不逐帧读取磁盘。
/// 比较函数仅在一次编辑事务后收到逻辑线程发布的查询结果时调用。
/// 资源损坏不会抛异常，也不会退回时间和轨道写死的旧教程。

namespace MMM::UI::Walkthrough
{
namespace
{
/// @brief 结束批注使用的中文后缀；只剥离末尾，标题本身可包含“结束”。
constexpr std::string_view END_SUFFIX = "结束";
/// @brief 作者在进阶第一段起点使用的分组标记。
constexpr std::string_view ADVANCED_PREFIX = "（进阶教学）";

/// @brief 起点或终点批注的最小字段集合。
struct Marker {
    std::string content;           ///< 教学说明原文。
    double      timestamp{ 0.0 };  ///< 所在时间，单位毫秒。
};

/// @brief 将草稿载荷或逻辑查询的组件映射到同一几何比较值。
/// @param note 已由剪贴板协议解析的草稿音符。
/// @param trackOffset 草稿比主轨道多出的左侧轨道数量。
/// @return 深拷贝的几何标量与子段，不保留载荷容器所有权。
/// @note 子段顺序就是折线连接顺序，不能在这里排序。
Logic::ComposeLessonNote geometryOf(const Logic::NoteComponent& note,
                                    int                         trackOffset)
{
    // 比较值保留 Flick 的水平延伸和 Hold 的纵向长度。
    // 不复制颜色、样本和批注，避免皮肤配置改变时误判谱面没有完成。
    // 草稿多出的列在主画布左侧，减去偏移后才是玩家轨道的列号。
    Logic::ComposeLessonNote result;
    result.type      = note.m_type;
    result.timestamp = note.m_timestamp;
    result.duration  = note.m_duration;
    result.track     = note.m_trackIndex - trackOffset;
    result.dtrack    = note.m_dtrack;
    // 参考在教程目录生命周期内只构造一次；提前保留子段容量。
    result.subNotes.reserve(note.m_subNotes.size());
    for ( const auto& sub : note.m_subNotes )
        result.subNotes.push_back({ sub.type,
                                    sub.timestamp,
                                    sub.duration,
                                    sub.trackIndex - trackOffset,
                                    sub.dtrack });
    return result;
}

/// @brief 比较一个根节点及全部子段，忽略不参与谱面几何的元数据。
/// @param left 来自持久化草稿的参考物件。
/// @param right 来自逻辑线程一次性截取的主轨道物件。
/// @return 根节点和各子段都在允许误差内时返回 true。
/// @note 轨道与类型必须完全相等；时间误差只吸收序列化精度和吸附误差。
bool sameGeometry(const Logic::ComposeLessonNote& left,
                  const Logic::ComposeLessonNote& right)
{
    // 2 ms 远小于教程示范使用的分拍间距，不能把相邻目标合并。
    // 持续时间共用这一阈值，避免 Hold 与瞬时 Note 被混淆。
    constexpr double TIME_TOLERANCE = 0.002;
    if ( left.type != right.type || left.track != right.track ||
         std::abs(left.timestamp - right.timestamp) > TIME_TOLERANCE ||
         left.subNotes.size() != right.subNotes.size() )
        return false;
    // 折线父级没有独立的持续时间或滑动距离：绘制时可能留下画笔末段值，
    // 而草稿剪贴板保留的是另一轮绘制的缓存。两者路径相同时仍应验收成功。
    if ( left.type != ::MMM::NoteType::POLYLINE &&
         (left.dtrack != right.dtrack ||
          std::abs(left.duration - right.duration) > TIME_TOLERANCE) )
        return false;
    // 折线必须从头到尾逐段对应；只比较包围盒会放过不同的路径。
    for ( std::size_t index = 0; index < left.subNotes.size(); ++index ) {
        const auto& a = left.subNotes[index];
        const auto& b = right.subNotes[index];
        if ( a.type != b.type || a.track != b.track || a.dtrack != b.dtrack ||
             std::abs(a.timestamp - b.timestamp) > TIME_TOLERANCE ||
             std::abs(a.duration - b.duration) > TIME_TOLERANCE )
            return false;
    }
    return true;
}
}  // namespace

/// @brief 加载 CanonRock 谱面并将时间戳批注配对成基础和进阶教学段落。
/// @details 相邻两条批注分别为起点和带结束后缀的终点；结束批注可简写或
/// 使用另一种说明，故只检查时间与终止标记，不要求两条说明逐字相同。
/// @param beatmapFile 真实教学谱面路径，侧车按同目录 .mmm 规则定位。
/// @return 全部段落与参考；任一结构性缺失返回人可读原因。
/// @warning 只能在服务初始化等低频路径调用；存在文件读取与排序。
std::expected<std::vector<ComposeLesson>, std::string> loadComposeLessons(
    const std::filesystem::path& beatmapFile)
{
    // 谱面是用户可更新的教程资源，读取失败时绝不退回旧的固定段落时间。
    // 非抛异常解析符合工程约束，也让损坏的资源包能显示具体错误。
    std::ifstream stream(beatmapFile, std::ios::binary);
    if ( !stream ) return std::unexpected("无法读取 CanonRock 教学谱面");
    const nlohmann::json root = nlohmann::json::parse(stream, nullptr, false);
    if ( root.is_discarded() || !root.is_object() ||
         !root.contains("annotations") || !root["annotations"].is_array() )
        return std::unexpected("CanonRock 教学谱面的批注格式无效");
    // 主轨道数必须从谱面自身读取，不能拿当前编辑器的键数配置替代。
    // 用户切换配置后仍应按示例谱面原来的四轨来解释草稿参照。
    // 先检查对象层级，再读取整数，避免资源损坏时触发 JSON 类型异常。
    const auto metadata = root.find("metadata");
    if ( metadata == root.end() || !metadata->is_object() )
        return std::unexpected("CanonRock 教学谱面缺少轨道信息");
    const auto base = metadata->find("base");
    if ( base == metadata->end() || !base->is_object() )
        return std::unexpected("CanonRock 教学谱面缺少轨道信息");
    const auto playerTrackCount = base->find("track_count");
    if ( playerTrackCount == base->end() ||
         !playerTrackCount->is_number_integer() ||
         playerTrackCount->get<int>() <= 0 )
        return std::unexpected("CanonRock 教学谱面轨道数无效");

    // 保持一份只含教学标记的列表，物件级批注不能改变教学段落数。
    // 用户可以为某个音符加批注，但不会意外把引导切成更短的片段。
    std::vector<Marker> markers;
    // 只接受时间戳教学标记。物件批注描述物件本身，不能成为段落边界。
    for ( const auto& item : root["annotations"] ) {
        if ( !item.is_object() ) continue;
        const auto targetKind = item.find("target_kind");
        if ( targetKind == item.end() || !targetKind->is_string() ||
             *targetKind != "timestamp" )
            continue;
        const auto content   = item.find("content");
        const auto timestamp = item.find("timestamp");
        // 时间戳批注缺少正文或数值时整份教程不可用。
        // 静默略过会把下一个终点误认成起点，影响所有后续段落。
        if ( content == item.end() || !content->is_string() ||
             timestamp == item.end() || !timestamp->is_number() )
            return std::unexpected("CanonRock 教学批注缺少名称或时间");
        const double time = timestamp->get<double>();
        if ( !std::isfinite(time) || time < 0.0 )
            return std::unexpected("CanonRock 教学批注时间无效");
        markers.push_back({ content->get<std::string>(), time });
    }
    // 同一时间的标记保留文件中的原始顺序，方便作者修正重叠标记。
    // 其它顺序以谱面时间为准，JSON 内的排列顺序不能改变播放路线。
    std::stable_sort(markers.begin(),
                     markers.end(),
                     [](const Marker& left, const Marker& right) {
                         return left.timestamp < right.timestamp;
                     });
    if ( markers.empty() || markers.size() % 2 != 0 )
        return std::unexpected("CanonRock 教学段落缺少开始或结束批注");

    // 每个段落消耗两个标记；奇数个标记说明资源编辑尚未完成。
    // 这里不推断缺失终点，也不使用下一段起点作为隐式结束时间。
    std::vector<ComposeLesson> lessons;
    lessons.reserve(markers.size() / 2);
    bool advanced = false;
    for ( std::size_t index = 0; index < markers.size(); index += 2 ) {
        const auto& begin = markers[index];
        const auto& end   = markers[index + 1];
        // 终点描述可简写，实际资产的进阶标记也只出现在起点。
        // 只检查“结束”后缀与正向时间，避免把说明措辞绑成程序协议。
        if ( begin.content.ends_with(END_SUFFIX) ||
             !end.content.ends_with(END_SUFFIX) ||
             end.timestamp <= begin.timestamp )
            return std::unexpected("CanonRock 教学批注未按起止标记配对");
        std::string_view title = begin.content;
        // 进阶标记首次出现后保持该阶段；作者无需给每个后续批注重复前缀。
        // 分支边界由谱面明确标记，不根据固定段落序号或标题关键词猜测。
        if ( title.starts_with(ADVANCED_PREFIX) ) {
            advanced = true;
            title.remove_prefix(ADVANCED_PREFIX.size());
        }
        if ( title.empty() )
            return std::unexpected("CanonRock 教学段落名称为空");
        lessons.push_back({ std::string(title),
                            begin.timestamp,
                            end.timestamp,
                            advanced,
                            {} });
    }
    if ( !advanced || lessons.front().m_advanced )
        return std::unexpected("CanonRock 教学缺少基础或进阶段落");

    // 草稿区并不在 .mmm 的 note 数组内；项目将其单独保存为剪贴板协议载荷。
    // 教程必须使用这一份真实参考，禁止从正式谱面猜测目标或让空参考自动通过。
    // .mmm 的正式音符可能本就为后面的调整教学准备，因此不能拿它们当标准答案。
    // 用户只更新 .mmm 而忘记侧车时，这里返回缺失信息而不是启动旧路线。
    std::ifstream draftStream(
        beatmapFile.parent_path() / ".mmm" / "draft_lanes.json",
        std::ios::binary);
    if ( !draftStream )
        return std::unexpected("CanonRock 项目缺少草稿区参考文件");
    const nlohmann::json draftRoot =
        nlohmann::json::parse(draftStream, nullptr, false);
    if ( draftRoot.is_discarded() || !draftRoot.is_object() ||
         !draftRoot.contains("m_draftLaneGroups") ||
         !draftRoot["m_draftLaneGroups"].is_array() )
        return std::unexpected("CanonRock 草稿区参考格式无效");
    // 一个项目可有多张谱面，必须按相对文件名选择本次示例谱面的草稿组。
    // 读取其它谱面的合法载荷也会导致练习目标看似存在却无法在画布完成。
    std::string payload;
    int         draftTrackCount = 0;
    // 草稿轨道数记录在谱面对应的组内；其它谱面的组宽度可能不同。
    // 本例草稿五轨、玩家四轨，最左草稿列留空以便显示相邻的四列。
    // 用两侧宽度之差换算轨道，不能把草稿编号当作主轨编号比较。
    for ( const auto& group : draftRoot["m_draftLaneGroups"] ) {
        if ( !group.is_object() ) continue;
        const auto path  = group.find("m_beatmapFilePath");
        const auto notes = group.find("m_notePayload");
        if ( path == group.end() || !path->is_string() ||
             notes == group.end() || !notes->is_string() )
            continue;
        if ( *path == Config::pathToUtf8(beatmapFile.filename()) ) {
            // 持久化协议自带轨道编码；保留原始文本交给同一解析器处理。
            // 不在 UI 另写一个剪贴板语法，以免新版本字段造成静默丢物件。
            payload          = notes->get<std::string>();
            const auto count = group.find("m_trackCount");
            if ( count != group.end() && count->is_number_integer() )
                draftTrackCount = count->get<int>();
            break;
        }
    }
    if ( payload.empty() )
        return std::unexpected(
            "CanonRock 草稿区没有参考物件，无法验收创作教学");
    if ( draftTrackCount < playerTrackCount->get<int>() )
        return std::unexpected("CanonRock 草稿轨道数小于主轨道数");
    // 对齐靠近边界的一侧，使草稿最后一列和主画布最后一列对应。
    // 只有参考几何需要换算；原侧车载荷继续保持自己的轨道坐标。
    const int  trackOffset = draftTrackCount - playerTrackCount->get<int>();
    const auto parsed = Logic::EditorClipboardProtocol::parse(payload, true);
    // 空载荷和无法解析均不能视作“空草稿参考”。
    // 后者意味着资源格式失效，不能让删除教程的空目标掩盖解析失败。
    if ( !parsed || parsed->notes.empty() )
        return std::unexpected("CanonRock 草稿区参考物件无法解析");
    for ( const auto& item : parsed->notes ) {
        const auto& note = item.note;
        // 子物件已经包含在根折线的 subNotes 内，单独计数会把一条折线算多次。
        if ( note.m_isSubNote ) continue;
        // 示范位于紧邻主画布的等宽草稿列；不能把左侧额外列映射成负主轨道。
        // 所有折线节点也必须落在相同范围，避免目标永远无法绘制完成。
        const auto validTrack = [&](int track) {
            return track >= trackOffset && track < draftTrackCount;
        };
        if ( !validTrack(note.m_trackIndex) ||
             std::any_of(
                 note.m_subNotes.begin(),
                 note.m_subNotes.end(),
                 [&](const auto& sub) { return !validTrack(sub.trackIndex); }) )
            return std::unexpected("CanonRock 草稿示范超出对应的主轨道范围");
        const double timestampMs = note.m_timestamp * 1000.0;
        // 参考按头部时间落入段落；Hold 和折线的尾部可以跨过本段结束线。
        // 只归给首个命中的段落，边界同时间不应重复要求绘制同一物件。
        for ( auto& lesson : lessons ) {
            if ( timestampMs >= lesson.m_beginMs &&
                 timestampMs <= lesson.m_endMs ) {
                lesson.m_reference.push_back(geometryOf(note, trackOffset));
                break;
            }
        }
    }
    // 前四段是从空白主画布放置 Note、Hold、Flick 与折线，必须有草稿示范。
    // 删除段可以以空草稿作为目标，因此不强求每一段都有参考物件。
    // 这一守卫也防止作者误把草稿参考写入正式 note 数组后教程自动通过。
    for ( std::size_t index = 0;
          index < std::min<std::size_t>(4, lessons.size());
          ++index )
        if ( lessons[index].m_reference.empty() )
            return std::unexpected("CanonRock 前四段缺少草稿区示范物件");
    return lessons;
}

/// @brief 对主轨道与草稿参考执行不依赖容器顺序的完整匹配。
/// @param lesson 已从项目草稿侧车提取目标几何的段落。
/// @param actual 当前正式轨道在该段的全部根物件。
/// @return 数量、类型、轨道、时间和全部子段都对应时返回 true。
/// @warning 仅在对象编辑事务后调用；避免在每帧搜索大量物件。
bool matchesComposeLessonNotes(
    const ComposeLesson&                         lesson,
    const std::vector<Logic::ComposeLessonNote>& actual)
{
    // 数量先行拒绝，额外物件和漏物件都不能通过。
    // 这一步也给空目标的删除练习提供明确语义：段内必须真的清空。
    if ( lesson.m_reference.size() != actual.size() ) return false;
    // 同一拍可能有重叠物件；消费匹配位置防止一个主物件满足两份参考。
    std::vector<bool> matched(actual.size(), false);
    // 主轨道 ECS 枚举顺序不稳定，不能要求它与草稿序列化顺序相同。
    // 每个实际物件最多被消费一次，保留同拍重叠物件的正确计数。
    for ( const auto& expected : lesson.m_reference ) {
        bool found = false;
        for ( std::size_t index = 0; index < actual.size(); ++index ) {
            if ( matched[index] || !sameGeometry(expected, actual[index]) )
                continue;
            matched[index] = true;
            found          = true;
            break;
        }
        if ( !found ) return false;
    }
    return true;
}

/// @brief 复用验收的几何语义，为未完成目标与错误物件保留独立标记。
/// @details 相同位置可有多个对象，不能用集合按坐标去重；
/// 每份参考只能消费一份正式物件，反过来同样成立。
/// 红框来源于未匹配的实际物件，普通框来源于未匹配的参考物件。
/// 几何判定直接调用最终验收的同一比较器，避免提示与进度结论冲突。
/// 此函数只在查询完成时调用，允许为稳定反馈分配两组匹配数组。
ComposeLessonFeedback compareComposeLessonNotes(
    const ComposeLesson& lesson, std::vector<Logic::ComposeLessonNote> actual,
    std::uintptr_t beatmapInstanceId, std::uint64_t composeNoteRevision)
{
    ComposeLessonFeedback feedback;
    feedback.lesson              = &lesson;
    feedback.beatmapInstanceId   = beatmapInstanceId;
    feedback.composeNoteRevision = composeNoteRevision;
    feedback.actual              = std::move(actual);
    feedback.expectedMatched.assign(lesson.m_reference.size(), false);
    feedback.actualMatched.assign(feedback.actual.size(), false);
    // 与最终验收共用 sameGeometry；先消费完全对应的物件，局部错误不会
    // 挡住同段内其它正确物件的提示状态。
    for ( std::size_t expected = 0; expected < lesson.m_reference.size();
          ++expected ) {
        for ( std::size_t found = 0; found < feedback.actual.size(); ++found ) {
            if ( feedback.actualMatched[found] ||
                 !sameGeometry(lesson.m_reference[expected],
                               feedback.actual[found]) )
                continue;
            feedback.expectedMatched[expected] = true;
            feedback.actualMatched[found]      = true;
            break;
        }
    }
    return feedback;
}

/// @brief 将实际批注段落展开为两条可单独进入的三阶段教学分支。
/// @details 步骤时间窗来自谱面，不写死在 JSON 中；进阶第一段标记切换分支。
/// 每一段分别预览、编辑、重播，后段的首步只依赖本分支上一段的重播结果。
/// @param topic 内置 JSON 解析得到的稳定主题身份和总体说明。
/// @param lessons 已解析并验收过草稿参考的有序段落。
/// @warning 服务构造时调用，生成的模型随后供 UI 只读访问。
void populateComposeLessonTopic(Topic&                            topic,
                                const std::vector<ComposeLesson>& lessons)
{
    // 两条分支沿用固定 ID，使已有学习记录能按分支稳定归属。
    // 不把进阶首段设为基础末段的前置条件，允许有经验用户直接练进阶。
    Branch basic;
    basic.m_id                            = "basic";
    basic.m_title.m_translations["zh_cn"] = "基础教学";
    basic.m_title.m_translations["en_us"] = "Basics";
    Branch advanced;
    advanced.m_id                            = "advanced";
    advanced.m_title.m_translations["zh_cn"] = "进阶教学";
    advanced.m_title.m_translations["en_us"] = "Advanced";
    // phaseIndex 决定同一段的固定顺序，也用于构造稳定的步骤 ID。
    // 三阶段都要求实际业务完成；静态确认不能跳过任何一次播放或编辑。
    const std::array phases{ ComposeLessonPhase::Preview,
                             ComposeLessonPhase::Practice,
                             ComposeLessonPhase::Review };
    for ( std::size_t index = 0; index < lessons.size(); ++index ) {
        const auto& lesson = lessons[index];
        auto&       branch = lesson.m_advanced ? advanced : basic;
        // 阶段顺序在数据中展开，而不在页面渲染时每帧动态生成。
        // 每段完整走完复播后，下一段首播才会成为当前目标。
        for ( std::size_t phaseIndex = 0; phaseIndex < phases.size();
              ++phaseIndex ) {
            Step              step;
            const std::string prefix = "lesson-" + std::to_string(index + 1);
            const char*       suffix = phaseIndex == 0   ? "preview"
                                       : phaseIndex == 1 ? "practice"
                                                         : "review";
            step.m_id                = prefix + "-" + suffix;
            // 依赖只指向当前分支前一步，不跨过基础与进阶的入口边界。
            if ( !branch.m_steps.empty() )
                step.m_prerequisites.push_back(branch.m_steps.back().m_id);
            step.m_title.m_translations["zh_cn"] =
                lesson.m_title + (phaseIndex == 0   ? "：先播放"
                                  : phaseIndex == 1 ? "：照草稿编辑"
                                                    : "：完成后重播");
            step.m_title.m_translations["en_us"] =
                "Lesson " + std::to_string(index + 1) +
                (phaseIndex == 0   ? ": preview"
                 : phaseIndex == 1 ? ": practice"
                                   : ": replay");
            step.m_body.m_translations["zh_cn"] =
                phaseIndex == 0 ? "先播放这一段，观察草稿区的目标物件。"
                : phaseIndex == 1
                    ? "暂停后按照同一时间段草稿区的物件，在主画布完成绘制或调整"
                      "；物件结构、轨道和拍位都要对应。"
                    : "目标物件已完成，再播放同一段检查实际效果。";
            step.m_body.m_translations["en_us"] =
                phaseIndex == 0
                    ? "Play this section and study its draft lane reference."
                : phaseIndex == 1
                    ? "Edit the player lanes to match the draft reference in "
                      "this section."
                    : "Replay this section to review your result.";
            // 播放与编辑各有独立语义目标；UI 只接收业务状态机的完成通知。
            // 所有步骤禁用“知道了”，避免错误谱面或未完成物件继续推进。
            Guide guide;
            guide.m_targets = { phaseIndex == 1 ? "compose.lesson.practice"
                                                : "compose.lesson.playback" };
            guide.m_requiresAction = true;
            guide.m_prompt.m_translations["zh_cn"] =
                "第 " + std::to_string(index + 1) + " 段 · " + lesson.m_title +
                "\n" + step.m_body.m_translations["zh_cn"];
            guide.m_prompt.m_translations["en_us"] =
                "Lesson " + std::to_string(index + 1) + ": " + lesson.m_title +
                "\n" + step.m_body.m_translations["en_us"];
            step.m_guide = std::move(guide);
            // 页面只需要时间窗、阶段和目录索引，不复制整份草稿答案。
            // 参考数组始终由 Service 持有，编辑完成后才低频取出比较。
            step.m_composeLesson = ComposeLessonStep{
                lesson.m_beginMs, lesson.m_endMs, phases[phaseIndex], index
            };
            branch.m_steps.push_back(std::move(step));
        }
    }
    // 资产已在加载时验证两个阶段都存在；此处保留固定基础、进阶顺序。
    // 成功加载才解除静态主题的占位标记；缺资产时服务不会调用本函数。
    topic.m_branches    = { std::move(basic), std::move(advanced) };
    topic.m_anyBranch   = false;
    topic.m_placeholder = false;
}
}  // namespace MMM::UI::Walkthrough
