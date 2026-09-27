#pragma once

#include "canvas/AnnotationTableData.h"

#include <filesystem>
#include <string>
#include <vector>

namespace MMM::Canvas
{

/// @brief 批注导出的文件格式。
enum class AnnotationExportFormat { Txt, Docx, Xlsx };

/// @brief 批注位置在导出文件中的表达方式。
enum class AnnotationExportPosition { Timestamp, Beat };

/// @brief 导出时使用的界面语言标签，由批注表按当前翻译提供。
struct AnnotationExportLabels {
    std::string track;
    std::string bgmTrack;
    std::string note;
    std::string hold;
    std::string flick;
    std::string polyline;
    std::string polylineSubNote;
    std::string sample;
    std::string playerObject;
    std::string targetMissing;
    std::string author;
    std::string positionHeader;
    std::string targetHeader;
    std::string authorHeader;
    std::string contentHeader;
};

/// @brief 一条已选定位置表达方式并补全目标描述的导出记录。
struct AnnotationExportRecord {
    std::string position;
    std::string target;
    std::string author;
    std::string content;
};

/// @brief 从批注表的稳定快照生成完整导出记录。
/// @param rows 当前谱面已经解析并按时间排序的批注行。
/// @param context 与批注表相同的 BPM 和分拍上下文。
/// @param position 用户选择时间戳或拍号；无 BPM 时拍号回退时间戳。
/// @param labels 当前界面语言中的目标和表头名称。
/// @return 每条批注对应一条记录，保留作者与原始 Markdown 正文。
/// @warning 用户显式导出路径：逐行格式化全部批注，不在 UI 绘制循环调用。
[[nodiscard]] std::vector<AnnotationExportRecord> buildAnnotationExportRecords(
    const std::vector<AnnotationTableRow>&    rows,
    const UI::Utils::CanvasTimeFormatContext& context,
    AnnotationExportPosition position, const AnnotationExportLabels& labels);

/// @brief 将全部批注写成 UTF-8 TXT 或 Office Open XML 文档。
/// @param records 已完成位置和目标描述的批注记录。
/// @param labels 当前语言的列标题与作者标签。
/// @param format 与目标文件扩展名匹配的格式。
/// @param path 用户选择的完整目标路径；父目录应已存在。
/// @param error 失败时接收可展示的错误原因。
/// @return 完整文件已写入并替换目标时返回 true。
/// @warning 用户显式导出路径：构造整个输出并执行磁盘 IO，不能进入热路径。
bool writeAnnotationExportFile(
    const std::vector<AnnotationExportRecord>& records,
    const AnnotationExportLabels& labels, AnnotationExportFormat format,
    const std::filesystem::path& path, std::string& error);

}  // namespace MMM::Canvas
