#pragma once

#include "logic/EditorClipboardProtocol.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace MMM::UI::Test
{
/// @brief 在隔离配置根创建含起止批注和草稿参考的最小 CanonRock 教学夹具。
/// @details 四段基础和一段进阶足以检验分支、阶段及几何目标，不依赖个人项目。
/// @param directory 由 CTest 隔离的 CanonRock 目录。
/// @return 谱面与侧车都成功写入时返回 true。
/// @warning 测试夹具只允许向调用方提供的隔离目录写文件。
///
/// 本夹具使用与生产项目相同的两份数据：
/// - .mmm 内的 annotations 决定五个教学时间窗；
/// - .mmm/draft_lanes.json 的剪贴板载荷决定目标音符；
/// - 前四段均有草稿物件，覆盖生产解析器的必需参考检查；
/// - 第五段首次带进阶前缀，覆盖基础与进阶的切换；
/// - 终点说明与起点标题不同，覆盖真实资产的简写方式。
/// 夹具不需要音频文件，因为测试只检查目录生成和欢迎页布局。
inline bool writeComposeLessonFixture(const std::filesystem::path& directory)
{
    // 用错误码反馈无写入权限，保持测试启动流程不依赖异常。
    // 此目录由测试隔离，不能在个人 CanonRock 项目中创建侧车。
    std::error_code error;
    std::filesystem::create_directories(directory / ".mmm", error);
    if ( error ) return false;

    // 五个时间窗互不重叠，每段中点各放一枚物件。
    // 这样边界归属和浮点舍入不会影响分支结构测试。
    nlohmann::json                    annotations = nlohmann::json::array();
    std::vector<Logic::ClipboardItem> draftNotes;
    for ( int index = 0; index < 5; ++index ) {
        const double      beginMs = 1000.0 + index * 3000.0;
        const double      endMs   = beginMs + 1500.0;
        const std::string title = index == 4
                                      ? "（进阶教学）进阶示范"
                                      : "基础示范" + std::to_string(index + 1);
        // 终点统一使用短说明，检查解析器不要求和起点逐字相同。
        annotations.push_back({ { "target_kind", "timestamp" },
                                { "content", title },
                                { "timestamp", beginMs } });
        annotations.push_back({ { "target_kind", "timestamp" },
                                { "content", "示范结束" },
                                { "timestamp", endMs } });
        Logic::ClipboardItem item;
        // 草稿比主画布多一条左侧轨道；示范使用靠近主画布的四列。
        item.note.m_type       = NoteType::NOTE;
        item.note.m_trackIndex = index % 4 + 1;
        item.note.m_timestamp  = (beginMs + 500.0) / 1000.0;
        draftNotes.push_back(std::move(item));
    }

    // 正式 note 数组留空，确保教程答案只能来自草稿载荷。
    // 用正式谱面推断草稿的实现会在本夹具中暴露为缺参考。
    const nlohmann::json beatmap{ { "metadata",
                                    { { "base", { { "track_count", 4 } } } } },
                                  { "annotations", annotations },
                                  { "note", nlohmann::json::array() } };
    const nlohmann::json draft{
        { "m_draftLaneGroups",
          nlohmann::json::array(
              { { { "m_beatmapFilePath", "卡农-示例谱面.mmm" },
                  { "m_notePayload",
                    Logic::EditorClipboardProtocol::serializeNotes(
                        draftNotes) },
                  { "m_trackCount", 5 } } }) }
    };
    // 两份文件都完整关闭后才能启动服务读取并生成教学步骤。
    // close 后检查流状态，避免半写入 JSON 被误认为有效资产。
    std::ofstream beatmapStream(directory / "卡农-示例谱面.mmm",
                                std::ios::binary | std::ios::trunc);
    beatmapStream << beatmap.dump();
    beatmapStream.close();
    std::ofstream draftStream(directory / ".mmm" / "draft_lanes.json",
                              std::ios::binary | std::ios::trunc);
    // 侧车必须先于 Service 构造完成写入，否则服务会固定为占位主题。
    // 测试不复用上次运行的文件内容，始终以本轮载荷为准。
    // 两个 stream 的状态共同决定夹具是否可用于后续 UI 断言。
    draftStream << draft.dump();
    draftStream.close();
    return beatmapStream.good() && draftStream.good();
}
}  // namespace MMM::UI::Test
