#include "ui/walkthrough/ComposeLessonCatalog.h"
#include "config/Utf8Path.h"
#include "logic/EditorClipboardProtocol.h"
#include "ui/walkthrough/WalkthroughModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
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
/// @brief 将需要直接拖动现有几何的段落映射到移动工具。
/// @param title 批注中的规范化教学名称。
/// @return 其余绘制、续写、覆盖与右键删除段落使用绘制工具。
Logic::EditTool requiredComposeLessonTool(std::string_view title)
{
    // 教学动作由作者批注定义，不根据草稿物件类型猜测：同一种 Note
    // 可以在不同段落分别要求绘制、拖动头部或调整尾部。
    constexpr std::array<std::string_view, 5> moveLessons{
        "拖拽移动教学",     "滑键拖拽调整教学", "长条拖拽调整教学",
        "折线拖拽调整教学", "折线拖拽合并教学",
    };
    return std::find(moveLessons.begin(), moveLessons.end(), title) !=
                   moveLessons.end()
               ? Logic::EditTool::Move
               : Logic::EditTool::Draw;
}

namespace
{
/// @brief 结束批注使用的中文后缀；只剥离末尾，标题本身可包含“结束”。
constexpr std::string_view END_SUFFIX = "结束";
/// @brief 作者在进阶第一段起点使用的分组标记。
constexpr std::string_view ADVANCED_PREFIX = "（进阶教学）";

/// @brief 将谱面批注标题关联到打包的操作动画。
/// @return 未配置动画的自定义段落返回空，仍可正常进入教学。
/// @details 使用规范化后的批注标题作为键，进阶段落的组名前缀已被剥离。
/// 资源文件名使用 ASCII，避免配置同步与跨平台路径编码的差异。
/// 映射只决定教学媒体，不参与播放边界或 Note 答案的验收。
std::string_view lessonGif(std::string_view title)
{
    // 标题来自示例谱面，不能以段落数组下标匹配动画；作者插入新段落时
    // 不应错配后面所有演示。没有配套文件的段落保持纯文字引导。
    constexpr std::array<std::pair<std::string_view, std::string_view>, 14>
        GIFS{ { { "单键放置教学", "place-tap.gif" },
                { "长条放置教学", "place-hold.gif" },
                { "滑键教学（给嚓音踩滑键）", "place-flick.gif" },
                { "折线放置教学", "place-polyline.gif" },
                { "拖拽移动教学", "move-note.gif" },
                { "滑键拖拽调整教学", "adjust-flick.gif" },
                { "长条拖拽调整教学", "adjust-hold.gif" },
                { "删除物件教学", "delete-note.gif" },
                { "续写折线教学", "extend-polyline.gif" },
                { "折线连接教学", "connect-polyline.gif" },
                { "折线覆盖物件教学", "cover-note.gif" },
                { "折线拖拽调整教学", "adjust-polyline.gif" },
                { "折线拖拽合并教学", "merge-polyline.gif" },
                { "折线删除子物件教学", "delete-polyline-segment.gif" } } };
    // 启动目录时只查一次，每帧 UI 不重新遍历这张表。
    for ( const auto& [name, file] : GIFS )
        if ( title == name ) return file;
    return {};
}

/// @brief 根据批注中的教学动作给出实际鼠标手势，而非暴露内部草稿答案。
/// @return 中文和英文的练习提示；新增未知段落时给出可执行的通用定位说明。
/// @warning 目录生成时调用，不进入绘制或编辑的每帧路径。
/// @details 同一种 Note 的放置、移动、尾部调整和删除使用不同输入事件。
/// 这张映射以教学批注标题为键，避免从 Note 类型反推用户应执行的手势。
/// 文案与欢迎页的总览互补：每段只讲当前要执行的动作与必要的按键约束。
/// 青色轮廓和蓝色落点承担具体位置提示，文本不暴露作为答案的草稿轨。
/// Shift 拖拽类操作以左键释放作为提交点；提前释放 Shift 会改变工具语义。
/// 删除类操作明确右键目标，避免将程序的撤销入口误写成教学步骤。
/// 两种语言按同一分支返回，使翻译不会落到另一教学段落的操作说明。
std::pair<std::string_view, std::string_view> lessonPracticeInstruction(
    std::string_view title)
{
    // 单键直接点按创建，不要求拖动；青色目标提供轨道和拍位。
    // 这里不写 Shift，避免用户误进入其它物件的绘制方式。
    if ( title == "单键放置教学" )
        return { "使用绘制工具，在青色目标位置单击左键写入单键。",
                 "Use Draw and left-click each cyan target to place a tap." };
    // 长条从头到尾由一次按住左键的手势决定持续时间。
    // Shift 必须覆盖整次拖拽，松开左键时才结束输入事务。
    if ( title == "长条放置教学" )
        return {
            "使用绘制工具，按住 Shift "
            "并用左键从长条头拖到尾；左键松开前不要松开 Shift。",
            "Use Draw: hold Shift and left-drag from each hold head to its "
            "end. Keep Shift held until releasing the left button."
        };
    // 滑键的箭头方向由拖拽终点决定，提示必须区分头部和尾部。
    // 放置阶段创建新物件，不能套用后面编辑旧箭头的动作。
    if ( title == "滑键教学（给嚓音踩滑键）" )
        return {
            "使用绘制工具，按住 Shift "
            "并用左键从滑键头横向拖到箭头目标；左键松开前不要松开 Shift。",
            "Use Draw: hold Shift and left-drag from each flick head to its "
            "arrow target. Keep Shift held until releasing the left button."
        };
    // 折线是一条连续路径，起笔后要经过全部目标节点。
    // 中途松开 Shift 或左键会让路径提前结束。
    if ( title == "折线放置教学" )
        return {
            "使用绘制工具，按住 Shift "
            "并用左键沿青色折线路径连续拖拽；完成整条折线并松开左键前不要松开 "
            "Shift。",
            "Use Draw: hold Shift and left-drag through the cyan polyline "
            "path. Keep Shift held until the whole path is finished and the "
            "left button is released."
        };
    // 移动现有物件要抓住头部，蓝点标出最终位置。
    // 此段使用拖拽工具，不创建第二个同类型 Note。
    if ( title == "拖拽移动教学" )
        return {
            "使用拖拽工具，按住左键拖动标出的物件头部到蓝色落点。",
            "Use Move: left-drag each marked note head to its blue target."
        };
    // 调整滑键只移动箭头终点；旧头部已经是正确的起点。
    // 文案显式提醒不要移动头部，以保持物件拍位不变。
    if ( title == "滑键拖拽调整教学" )
        return {
            "使用拖拽工具，按住左键将滑键箭头尾部拖到蓝色落点，不要移动滑键头"
            "。",
            "Use Move: left-drag each flick arrow to its blue target without "
            "moving the head."
        };
    // 长条的持续时间由尾部决定，头部不应随修改平移。
    // 蓝色落点代表新的尾端，不是重画长条的起笔点。
    if ( title == "长条拖拽调整教学" )
        return {
            "使用拖拽工具，按住左键将长条尾部拖到蓝色落点，不要移动长条头。",
            "Use Move: left-drag each hold end to its blue target without "
            "moving the head."
        };
    // 删除段要求用户使用右键，红框用于定位待删正式物件。
    // 已选中物件的右键会批量删除，说明中保留这项实际行为。
    if ( title == "删除物件教学" )
        return {
            "使用绘制工具，右键点击红框物件将其删除；右键点击已选中的物件会一并"
            "删除所有选中物件。",
            "Use Draw and right-click each red-marked note to delete it. "
            "Right-clicking a selected note deletes all selected notes."
        };
    // 续写以现有折线末端为起点，不再创建独立的新路径。
    // 蓝点随已完成的子段前进，拖拽期间 Shift 需要持续按住。
    if ( title == "续写折线教学" )
        return {
            "使用绘制工具，按住 "
            "Shift，从已有折线的末端左键拖拽到蓝色目标，继续完成路径；左键松开"
            "前不要松开 Shift。",
            "Use Draw: hold Shift and left-drag from the existing polyline end "
            "through the blue targets. Keep Shift held until releasing the "
            "left button."
        };
    // 连接教学把两段已有路径接起来，目标是另一段的连接点。
    // 这里沿用折线的连续 Shift 拖拽输入，不要求用户搬动整条折线。
    if ( title == "折线连接教学" )
        return {
            "使用绘制工具，按住 "
            "Shift，从已有折线末端左键拖到另一段折线的连接位置；左键松开前不要"
            "松开 Shift。",
            "Use Draw: hold Shift and left-drag from one polyline end to the "
            "next connection point. Keep Shift held until releasing the left "
            "button."
        };
    // 本段允许沿新路径清除重叠单键；这项规则由教学状态单独启用。
    // 提示用户画完整条目标路径，不能让用户逐个手动删掉旧 Note。
    if ( title == "折线覆盖物件教学" )
        return {
            "使用绘制工具，按住 Shift "
            "并用左键沿青色目标路径拖过已有单键；本段已开启路径清理，左键松开前"
            "不要松开 Shift。",
            "Use Draw: hold Shift and left-drag along the cyan path across "
            "existing taps. Path cleanup is enabled for this lesson; keep "
            "Shift held until releasing the left button."
        };
    // 折线路径中已有的节点可逐个拖到目标位置。
    // 抓取节点而非空白折线主体，才能只改对应子段。
    if ( title == "折线拖拽调整教学" )
        return {
            "使用拖拽工具，按住左键拖动需要调整的折线节点到对应蓝色落点，逐个修"
            "正路径。",
            "Use Move: left-drag each polyline node that needs adjustment to "
            "its blue target."
        };
    // 合并段同样操作连接节点，蓝点表示最终拼接位置。
    // 两段路径的其它正确节点不需要重新绘制。
    if ( title == "折线拖拽合并教学" )
        return {
            "使用拖拽工具，按住左键将折线连接节点拖到对应蓝色落点，使两段路径合"
            "并。",
            "Use Move: left-drag each polyline connection node to its blue "
            "target to join the paths."
        };
    // 普通节点右键用于断开，折线头或 Shift+右键用于删除整条。
    // 需要两种动作的区别，才能避免误删已有正确路径。
    if ( title == "折线删除子物件教学" )
        return {
            "使用绘制工具，右键点击要断开的折线节点；Shift+"
            "右键点击折线，或直接右键点击折线头，会删除整条折线。右键点击已选中"
            "的物件会删除所有选中物件。",
            "Use Draw: right-click a polyline node to split it. "
            "Shift+right-click the polyline, or right-click its head, to "
            "delete the whole path. Right-clicking a selected note deletes all "
            "selected notes."
        };
    // 未识别的自定义批注仍给出可执行的位置提示，且不假定具体工具。
    // 目录中已有的十四段均命中上方分支，测试会防止其意外回退。
    // 新增教学段落时应同步加入专属手势说明和对应测试断言。
    // 回退说明仅保障资源可读，不替代正式教学文案。
    return { "按青色轮廓与蓝色落点，在主轨道完成当前段落的物件操作。",
             "Use the cyan outlines and blue targets to edit this section in "
             "the player lanes." };
}

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

/// @brief 取得折线子段的可操作末端，供下一步方向提示使用。
/// @details Hold 的末端在时间轴上，Flick 的末端在目标轨道上。
/// 折线子段按谱面中的连接顺序处理，不按时间或轨道重排。
/// 横移和纵向可以交替出现；使用实际操作末端才能继续引导。
std::pair<int, double> pathEndpoint(
    const Logic::ComposeLessonNote::SubNote& sub)
{
    if ( sub.type == ::MMM::NoteType::HOLD )
        return { sub.track, sub.timestamp + sub.duration };
    if ( sub.type == ::MMM::NoteType::FLICK )
        return { sub.track + sub.dtrack, sub.timestamp };
    return { sub.track, sub.timestamp };
}

/// @brief 将当前折线的首个差异或缺失子段转换为一步蓝色方向箭头。
/// @param expected 草稿中的完整折线路径。
/// @param actual 同一起点的现有折线或尚未续写的独立首段；空值表示未起笔。
/// @return 目标已完整覆盖或只多出待删除子段时不提供方向箭头。
/// @warning 仅在正式物件修订后调用，不在画布热路径比较子段。
/// @details 箭头只表达当前一步，完整目标仍由青色轮廓展示。
/// 从第一个不一致的子段开始提示，避免把用户引回已完成区域。
/// 若现有子段均正确但路径仍短，就提示下一子段的操作终点。
/// 物件修订后重新计算，因此箭头会随用户的续写进度前进。
/// 已有路径比目标更长时没有下一目标，应留给红框提示删除。
/// 独立 Hold 可以作为首段，因为示例谱面从长条开始续写折线。
/// 时间容差与最终验收相同，轨道和类型仍要求精确对应。
std::optional<ComposeLessonPathArrow> nextPathArrow(
    const Logic::ComposeLessonNote& expected,
    const Logic::ComposeLessonNote* actual)
{
    if ( expected.subNotes.empty() ) return std::nullopt;
    const auto& target = expected.subNotes;
    if ( !actual ) {
        // 没有现有起点时，从首段头部指向首段末端。
        // 覆盖物件段也从这里获得起笔方向，不凭空匹配背景 Note。
        const auto [endTrack, endTime] = pathEndpoint(target.front());
        return ComposeLessonPathArrow{
            target.front().track, target.front().timestamp, endTrack, endTime
        };
    }
    const Logic::ComposeLessonNote::SubNote firstActual{ actual->type,
                                                         actual->timestamp,
                                                         actual->duration,
                                                         actual->track,
                                                         actual->dtrack };
    const auto count = actual->type == ::MMM::NoteType::POLYLINE
                           ? actual->subNotes.size()
                           : std::size_t{ 1 };
    const auto at =
        [&](std::size_t index) -> const Logic::ComposeLessonNote::SubNote& {
        // 独立首段只有一个可比较节点；折线保留真实子段顺序。
        return actual->type == ::MMM::NoteType::POLYLINE
                   ? actual->subNotes[index]
                   : firstActual;
    };
    constexpr double TIME_TOLERANCE = 0.002;
    for ( std::size_t index = 0; index < std::min(count, target.size());
          ++index ) {
        const auto& current = at(index);
        const auto& wanted  = target[index];
        if ( current.type == wanted.type && current.track == wanted.track &&
             current.dtrack == wanted.dtrack &&
             std::abs(current.timestamp - wanted.timestamp) <= TIME_TOLERANCE &&
             std::abs(current.duration - wanted.duration) <= TIME_TOLERANCE )
            continue;
        // 头部正确而末端有差异时，从现有端点指向草稿端点。
        // 调整长度或终轨无需先删除物件，仍保持原有练习手势。
        const auto [sourceTrack, sourceTime] = pathEndpoint(current);
        const auto [targetTrack, targetTime] = pathEndpoint(wanted);
        return ComposeLessonPathArrow{
            sourceTrack, sourceTime, targetTrack, targetTime
        };
    }
    if ( count >= target.size() || count == 0 ) return std::nullopt;
    // 当前全部子段吻合时，从最后末端接到第一处缺失段的末端。
    // 不从折线根部重新起箭头，否则每次续写都会指回开头。
    const auto [sourceTrack, sourceTime] = pathEndpoint(at(count - 1));
    const auto [targetTrack, targetTime] = pathEndpoint(target[count]);
    return ComposeLessonPathArrow{
        sourceTrack, sourceTime, targetTrack, targetTime
    };
}

/// @brief 根据批注标题选择需要修正的部位，普通放置练习仍可删除多余物件。
/// @details 这里仅识别示例谱面已定义的动作标题，不根据 Note 类型推断手势。
/// 同一 Flick 在放置教学中应创建新物件，在调整教学中则应拖动现有箭头。
/// 删除练习没有参考终点；它使用独立语义禁止一键代替右键操作。
/// 未识别的标题保留普通几何反馈，避免未来新增段落误接入错误拖动提示。
ComposeLessonRepairKind repairKindFor(const ComposeLesson& lesson)
{
    if ( lesson.m_title == "拖拽移动教学" )
        return ComposeLessonRepairKind::Move;
    if ( lesson.m_title == "滑键拖拽调整教学" )
        return ComposeLessonRepairKind::FlickTail;
    if ( lesson.m_title == "长条拖拽调整教学" )
        return ComposeLessonRepairKind::HoldTail;
    if ( lesson.m_title == "删除物件教学" )
        return ComposeLessonRepairKind::Delete;
    return ComposeLessonRepairKind::None;
}

/// @brief 计算可修正物件与目标的距离；不兼容的结构不参与配对。
/// @return 距离越小越优先；无法通过该段教学手势修正时为空。
/// @details 调整尾部要求根时间和轨道已正确；移动整件要求相对结构不变。
/// 距离只负责多个兼容物件的配对顺序，不决定最终教学是否完成。
/// 最终验收仍由 sameGeometry 对每个物件的绝对坐标执行严格检查。
/// @warning 只在正式物件修订后的低频比较中调用，不进入画布热路径。
std::optional<double> repairDistance(const Logic::ComposeLessonNote& expected,
                                     const Logic::ComposeLessonNote& actual,
                                     ComposeLessonRepairKind         kind)
{
    constexpr double TIME_TOLERANCE = 0.002;
    if ( expected.type != actual.type ||
         expected.subNotes.size() != actual.subNotes.size() )
        return std::nullopt;
    // 数量相等只是形状比较的前提；下面仍逐项检查子段的相对轨道与时间。
    // 这样移动教程中的折线可以整体平移，但不能把路径形状改了再配对。
    // 尾部调整不能改变根部；用精确起点把每条滑键或长条对应到原物件。
    if ( kind == ComposeLessonRepairKind::FlickTail ||
         kind == ComposeLessonRepairKind::HoldTail ) {
        if ( expected.track != actual.track ||
             std::abs(expected.timestamp - actual.timestamp) > TIME_TOLERANCE ||
             !expected.subNotes.empty() ||
             expected.type != (kind == ComposeLessonRepairKind::FlickTail
                                   ? ::MMM::NoteType::FLICK
                                   : ::MMM::NoteType::HOLD) ||
             (kind == ComposeLessonRepairKind::FlickTail &&
              std::abs(expected.duration - actual.duration) > TIME_TOLERANCE) ||
             (kind == ComposeLessonRepairKind::HoldTail &&
              expected.dtrack != actual.dtrack) )
            return std::nullopt;
        // 尾部差值只影响重名根物件的竞争顺序，不放宽上述根部约束。
        // 长条以持续时间作为尾部时间差，滑键以有向轨差作为箭头位置差。
        return kind == ComposeLessonRepairKind::FlickTail
                   ? static_cast<double>(
                         std::abs(expected.dtrack - actual.dtrack))
                   : std::abs(expected.duration - actual.duration);
    }
    if ( kind != ComposeLessonRepairKind::Move ) return std::nullopt;
    // 整件移动只能平移时间和轨道；类型、长度及折线相对拓扑必须保留。
    // 不设置任意最大移动距离：教学段内的物件均应能被移回正确位置。
    // 距离只参与最近候选选择，不能让错误的形状通过验收。
    if ( expected.type != ::MMM::NoteType::POLYLINE &&
         (expected.dtrack != actual.dtrack ||
          std::abs(expected.duration - actual.duration) > TIME_TOLERANCE) )
        return std::nullopt;
    for ( std::size_t index = 0; index < expected.subNotes.size(); ++index ) {
        const auto& left  = expected.subNotes[index];
        const auto& right = actual.subNotes[index];
        if ( left.type != right.type || left.dtrack != right.dtrack ||
             left.track - expected.track != right.track - actual.track ||
             std::abs((left.timestamp - expected.timestamp) -
                      (right.timestamp - actual.timestamp)) > TIME_TOLERANCE ||
             std::abs(left.duration - right.duration) > TIME_TOLERANCE )
            return std::nullopt;
    }
    // 时间使用毫秒权重，与教学批注和谱面吸附的可见刻度一致。
    // 轨差额外计入成本，使同一时间附近的同类型音符优先找近轨目标。
    // 全局最近优先，再消费双方下标；相邻同类型物件不能都指向一个目标。
    return std::abs(expected.timestamp - actual.timestamp) * 1000.0 +
           static_cast<double>(std::abs(expected.track - actual.track)) * 100.0;
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
    bool advanced        = false;
    bool allowUndoButton = true;
    // 按钮开关沿时间顺序单向关闭；不能在后续进阶段重新打开。
    // 这样新增的后续基础段也自动继承“用户亲自完成”的教学要求。
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
        // 删除段起所有练习都由用户亲自完成；后续新增基础段也继承该规则。
        if ( title == "删除物件教学" ) allowUndoButton = false;
        // 每个段落保存当时的开关快照；反馈对象无需在每帧追溯前一段。
        lessons.push_back({ std::string(title),
                            begin.timestamp,
                            end.timestamp,
                            advanced,
                            {},
                            allowUndoButton });
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
/// @par 可修正物件
/// 完全匹配优先消费实际和参考，修正候选只看双方剩余下标。
/// 候选保留两侧未匹配状态，使目标轮廓和当前红框同时显示。
/// 候选表只代表该做哪一种手势，不能使播放复查提前完成。
/// 轨道或时间已错的尾部编辑物件不作为候选，因为该手势不能改根部。
/// 独占目标防止多个同类型物件把蓝点叠到同一处。
/// 因此画布不需要自行猜测最近参考，避免每帧出现不稳定的线条切换。
/// @par 按钮范围
/// 删除段和其后段落沿用加载时关闭的开关，普通新段也不重新启用。
/// 三种编辑段即使排在删除段之前，也不能用一键删除代替操作。
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
    feedback.suppressErrorForActual.assign(feedback.actual.size(), false);
    feedback.repairKind = repairKindFor(lesson);
    feedback.repairTargetForActual.assign(feedback.actual.size(), -1);
    feedback.pathArrowForExpected.resize(lesson.m_reference.size());
    // 可修正物件不能提供删除捷径；删除段之后连普通多余物件也不提供。
    // 该标记在逻辑查询结果到达时固定，画布只读它来决定是否创建按钮。
    feedback.showUndoButton =
        lesson.m_allowUndoButton &&
        feedback.repairKind == ComposeLessonRepairKind::None;
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
    // 先排除已经完整匹配的物件，再为可修正的错误建立独占目标。
    // 匹配只在编辑修订后运行；画布每帧只读取配对下标和投影点。
    if ( feedback.repairKind == ComposeLessonRepairKind::Move ||
         feedback.repairKind == ComposeLessonRepairKind::FlickTail ||
         feedback.repairKind == ComposeLessonRepairKind::HoldTail ) {
        std::vector<bool> reserved(lesson.m_reference.size(), false);
        // 最近候选全局消费，比逐个实际物件找第一个参考更稳定。
        // 先匹配几何完全正确的物件，再把未完成目标供拖动提示使用。
        // reserved 与 expectedMatched 分开，提示配对不代表目标已完成。
        while ( true ) {
            double      bestDistance = std::numeric_limits<double>::infinity();
            std::size_t bestActual   = feedback.actual.size();
            std::size_t bestExpected = lesson.m_reference.size();
            for ( std::size_t actualIndex = 0;
                  actualIndex < feedback.actual.size();
                  ++actualIndex ) {
                if ( feedback.actualMatched[actualIndex] ||
                     feedback.repairTargetForActual[actualIndex] >= 0 )
                    continue;
                for ( std::size_t expectedIndex = 0;
                      expectedIndex < lesson.m_reference.size();
                      ++expectedIndex ) {
                    if ( feedback.expectedMatched[expectedIndex] ||
                         reserved[expectedIndex] )
                        continue;
                    // 不兼容对象返回空值，不会因为距离近而抢走可修正目标。
                    // 相同类型但不同折线路径、不同根部的调整对象也会被过滤。
                    const auto distance =
                        repairDistance(lesson.m_reference[expectedIndex],
                                       feedback.actual[actualIndex],
                                       feedback.repairKind);
                    if ( distance && *distance < bestDistance ) {
                        bestDistance = *distance;
                        bestActual   = actualIndex;
                        bestExpected = expectedIndex;
                    }
                }
            }
            if ( bestActual == feedback.actual.size() ) break;
            // 一对一结果保留原始参考下标，画布可以直接取目标的皮肤投影。
            // 不改 actualMatched：实际物件仍未验收，必须继续显示红框。
            feedback.repairTargetForActual[bestActual] =
                static_cast<int>(bestExpected);
            reserved[bestExpected] = true;
        }
    }
    // 进阶折线以草稿的完整路径为参照，但蓝箭头只指出当前要完成的
    // 第一处子段。优先关联同一起点的现有折线，避免独立首段抢走续写提示。
    // 这些配对不改变验收标记；用户仍须完成整个路径并复播。
    // 完整匹配已经先消费，不会再对正确折线生成重复方向提示。
    // 这里只按根时间和轨道找候选，允许当前尾端尚未调整到位。
    // 段内可能还有同拍背景 Note，首段类型也要与参考相容。
    // 每个现有候选只指向一个参考，防止重叠目标共享一条箭头。
    // 提示配对不等于几何验收，不能借此提前结束编辑阶段。
    // 起点坐标采用谱面秒数，误差与参考几何比较中的 2 ms 一致。
    // 不用实体 ID 关联草稿，因为草稿区与正式区的实体本就不同。
    // 也不依赖逻辑线程枚举顺序，候选可按子段数稳定比较进度。
    // 完整形态的轮廓由画布单独绘制，这里只保存数值型箭头端点。
    // 用户滚动画布后会重新投影端点，因此不能在反馈里缓存像素。
    // 后续可能同时存在多条参考折线，reserved 防止重复使用候选。
    // 若没有任何候选，首段方向仍有效，但当前实际数量保持不变。
    // 删除子物件段的参考若只含独立 Note，不会进入此折线分支。
    if ( lesson.m_advanced ) {
        constexpr double  TIME_TOLERANCE = 0.002;
        std::vector<bool> reserved(feedback.actual.size(), false);
        for ( std::size_t expectedIndex = 0;
              expectedIndex < lesson.m_reference.size();
              ++expectedIndex ) {
            const auto& expected = lesson.m_reference[expectedIndex];
            if ( feedback.expectedMatched[expectedIndex] ||
                 expected.type != ::MMM::NoteType::POLYLINE ||
                 expected.subNotes.empty() )
                continue;
            std::size_t best  = feedback.actual.size();
            std::size_t score = 0;
            for ( std::size_t actualIndex = 0;
                  actualIndex < feedback.actual.size();
                  ++actualIndex ) {
                const auto& current = feedback.actual[actualIndex];
                if ( reserved[actualIndex] ||
                     feedback.actualMatched[actualIndex] ||
                     current.track != expected.track ||
                     std::abs(current.timestamp - expected.timestamp) >
                         TIME_TOLERANCE )
                    continue;
                // 单独的 Hold 可作为折线首段；其他独立物件不能用来
                // 指示续写，否则同拍的背景 Note 会得到错误蓝箭头。
                const auto& first = expected.subNotes.front();
                const auto  compatible =
                    current.type == ::MMM::NoteType::POLYLINE
                        ? !current.subNotes.empty() &&
                              current.subNotes.front().type == first.type &&
                              current.subNotes.front().track == first.track &&
                              std::abs(current.subNotes.front().timestamp -
                                       first.timestamp) <= TIME_TOLERANCE
                        : current.type == first.type;
                if ( !compatible ) continue;
                // 同根候选优先选子段较多的现有折线；独立首段兜底。
                // 这让教程沿用户实际续写的路径推进，而非停在首段。
                const std::size_t candidateScore =
                    current.type == ::MMM::NoteType::POLYLINE
                        ? current.subNotes.size() + 1
                        : 1;
                if ( best == feedback.actual.size() ||
                     candidateScore > score ) {
                    best  = actualIndex;
                    score = candidateScore;
                }
            }
            if ( best != feedback.actual.size() ) {
                reserved[best] = true;
                // 起始子段正确时，该物件是待续写或待调整路径，而非多余物件。
                // 保持未验收状态，仅抑制整件错误框；目标轮廓和蓝箭头照常显示。
                // 未配对的多余 Note 或错位折线继续由画布显示红框。
                feedback.suppressErrorForActual[best] = true;
            }
            // 没有候选也保留首段起笔提示，但不增加正式物件。
            // 用户完成修改后才由下一次查询更新箭头与验收状态。
            feedback.pathArrowForExpected[expectedIndex] = nextPathArrow(
                expected,
                best == feedback.actual.size() ? nullptr
                                               : &feedback.actual[best]);
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
                                  : phaseIndex == 1 ? "：动手编辑"
                                                    : "：完成后重播");
            step.m_title.m_translations["en_us"] =
                "Lesson " + std::to_string(index + 1) +
                (phaseIndex == 0   ? ": preview"
                 : phaseIndex == 1 ? ": practice"
                                   : ": replay");
            step.m_body.m_translations["zh_cn"] =
                phaseIndex == 0 ? "先播放这一段，观察主轨道物件的实际效果。"
                : phaseIndex == 1
                    ? std::string(
                          lessonPracticeInstruction(lesson.m_title).first)
                    : "目标物件已完成，再播放同一段检查实际效果。";
            step.m_body.m_translations["en_us"] =
                phaseIndex == 0
                    ? "Play this section and observe the player lanes."
                : phaseIndex == 1
                    ? std::string(
                          lessonPracticeInstruction(lesson.m_title).second)
                    : "Replay this section to review your result.";
            // 练习提示由动作名称决定，和青色轮廓、蓝色落点共同说明操作。
            // 草稿几何只供后台验收，不能要求用户看不可见的内部参考。
            // 播放与编辑各有独立语义目标；UI 只接收业务状态机的完成通知。
            // 所有步骤禁用“知道了”，避免错误谱面或未完成物件继续推进。
            Guide guide;
            // 同一段的首播、练习与复播都保留操作演示；媒体不改变
            // 三阶段实际由播放器和谱面验收推进的状态机。
            if ( const auto file = lessonGif(lesson.m_title); !file.empty() )
                guide.m_gif = "walkthrough-gif:" + std::string(file);
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
