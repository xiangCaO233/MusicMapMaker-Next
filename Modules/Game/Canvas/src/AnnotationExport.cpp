#include "canvas/AnnotationExport.h"

#include "ui/utils/TimeFormatUtils.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <miniz.h>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace MMM::Canvas
{
namespace
{
/// @brief 将秒数固定为用户熟悉的 分:秒:毫秒 定位格式。
/// @param timestamp 有限的谱面时间，单位秒。
/// @return 例如 00:13:748；负时间保留前导负号。
/// @details 谱面模型使用秒作为计算单位，而导出示例要求毫秒三段式。
/// 先整体舍入为整数毫秒，再拆出分、秒、余毫秒，避免各段分别舍入
/// 造成 59.9995 秒在显示上变成 00:59:1000 的非法结果。
/// 负时间只在整体值前加负号，后续三段仍按绝对值拆解。
/// 非有限输入不参与数值转换，以免整数转换出现未定义范围问题。
std::string formatExportTimestamp(double timestamp)
{
    if ( !std::isfinite(timestamp) ) return "00:00:000";
    // 先保存符号；fabs 后的余数只用于展示，不改变真实谱面时间。
    const bool negative = timestamp < 0.0;
    const auto millis =
        static_cast<std::int64_t>(std::llround(std::abs(timestamp) * 1000.0));
    return fmt::format("{}{:02}:{:02}:{:03}",
                       negative ? "-" : "",
                       millis / 60000,
                       (millis / 1000) % 60,
                       millis % 1000);
}

/// @brief 把批注对象类型和起止轨道组合为一段可读描述。
/// @param row 已在持锁刷新时补齐物件类型的批注行。
/// @param labels 当前界面语言标签。
/// @return 时间标记为空描述，缺失目标保留明确状态。
/// @details 位置和目标分开存储，供 XLSX 分列；TXT 和 DOCX 再组合。
/// 表格快照中的轨道编号为零基，导出文本面向用户使用一基编号。
/// 缺失目标仍保留批注及其时间，不猜测原本的物件类型或轨道。
/// 采样轨与玩家轨是两个命名空间，采样使用 BGM 区的局部编号。
/// Flick 描述的是滑动起止轨，折线描述的是整条线的起止轨。
/// 折线子物件另加前缀，使读者知道它不是折线根节点。
/// 起止轨相同则写单轨描述，避免产生“轨道1＞1”的假跨度。
std::string formatExportTarget(const AnnotationTableRow&     row,
                               const AnnotationExportLabels& labels)
{
    using Kind = ::MMM::BeatmapAnnotationTargetKind;
    if ( row.targetKind == Kind::TIMESTAMP ) return {};
    // 目标失效的批注仍导出，优先标记缺失而非沿用历史轨道。
    if ( row.targetMissing ) return labels.targetMissing;

    if ( row.targetKind == Kind::AUDIO_SAMPLE ) {
        // BGM 区的局部轨与玩家轨互不换算；负值表示无可靠轨道。
        if ( row.exportTrack < 0 ) return labels.sample;
        return fmt::format(
            "{}{}{}", labels.bgmTrack, row.exportTrack + 1, labels.sample);
    }

    std::string typeLabel;
    // 未知物件枚举回退“物件”，但不丢失位置和备注正文。
    switch ( row.objectType ) {
    case AnnotationExportObjectType::Note: typeLabel = labels.note; break;
    case AnnotationExportObjectType::Hold: typeLabel = labels.hold; break;
    case AnnotationExportObjectType::Flick: typeLabel = labels.flick; break;
    case AnnotationExportObjectType::Polyline:
        typeLabel = labels.polyline;
        break;
    default: typeLabel = labels.playerObject; break;
    }
    if ( row.isPolylineSubNote ) {
        // 子段批注不伪装成整条折线，同时保留子段的具体 Note 类型。
        typeLabel = labels.polylineSubNote + typeLabel;
    }
    if ( row.exportTrack < 0 ) return typeLabel;
    // 草稿区负轨道没有稳定的一基显示编号，只保留物件类型。
    if ( row.exportEndTrack >= 0 && row.exportEndTrack != row.exportTrack ) {
        // 滑键尾轨和折线终轨使用相同一基编号，符号由真实终轨决定。
        return fmt::format("{}{}＞{}{}",
                           labels.track,
                           row.exportTrack + 1,
                           row.exportEndTrack + 1,
                           typeLabel);
    }
    return fmt::format("{}{}{}", labels.track, row.exportTrack + 1, typeLabel);
}

/// @brief 去除 XML 1.0 不允许的控制字节，并转义标记字符。
/// @param value 原始 UTF-8 批注或界面标签。
/// @return 可放入 XML 文本节点的 UTF-8 内容。
/// @note 高位 UTF-8 字节保持原值；批注编辑器负责存入有效 UTF-8。
/// @details XML 的五个保留符号要逐字节转义，否则用户备注中的
/// 标签样式、与号或引号会破坏 Office 文档结构。
/// 中文和 emoji 的 UTF-8 字节均位于高位区，可原样穿过此扫描。
/// C0 控制字符除了换行、回车、制表以外不属于 XML 1.0 字符集。
/// 这里仅净化 Office 文档；TXT 应保留批注原始内容。
std::string escapeXml(std::string_view value)
{
    std::string escaped;
    // 最坏情况下每个符号扩展为转义实体；reserve 先覆盖普通文本路径。
    escaped.reserve(value.size());
    for ( const unsigned char c : value ) {
        switch ( c ) {
        case '&': escaped += "&amp;"; break;
        // 转义在放入 OOXML 节点前完成，不能对整个文档再次转义。
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&apos;"; break;
        default:
            // XML 1.0 只容许制表、换行和回车三种 C0 控制字符。
            if ( c >= 0x20U || c == '\t' || c == '\n' || c == '\r' ) {
                escaped.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return escaped;
}

/// @brief 拼装 TXT 与 Word 共用的单条人类可读前缀。
/// @param record 已格式化的批注记录。
/// @param labels 作者标签随当前语言变化。
/// @return 位置、目标、作者和正文之间使用空格分隔。
/// @details 作者不是定位信息，使用方括号隔开，防止与正文首词混淆。
/// 时间戳批注没有目标描述，不能生成多余的对象占位词。
/// 备注正文保留原始换行和 Markdown 符号，不在纯文本阶段渲染。
/// DOCX 使用同一段人类可读语序，XLSX 则直接使用结构化列。
std::string formatRecordText(const AnnotationExportRecord& record,
                             const AnnotationExportLabels& labels)
{
    std::string line = record.position;
    // 只为存在的可选字段加入分隔符，纯时间批注仍以正文直接衔接。
    if ( !record.target.empty() ) line += " " + record.target;
    if ( !record.author.empty() ) {
        line += " [" + labels.author + ": " + record.author + "]";
    }
    if ( !record.content.empty() ) line += " " + record.content;
    return line;
}

/// @brief 为 Word 文本添加保留空白的片段和显式换行。
/// @param document 正在构造的 document.xml。
/// @param line 一条完整批注，可能包含多行 Markdown。
/// @details 一个批注对应一个 Word 段落，段内换行用 w:br 表示。
/// 若把换行直接放在 w:t 内，Word 可能把它折叠为普通空白。
/// xml:space=preserve 保留用户在 Markdown 前缀或缩进中的空格。
/// 空行也要写出一个空 w:t，确保连续换行的结构可逆。
/// 每一段的文本先经 XML 转义，正文不能成为可执行 XML 节点。
void appendWordText(std::string& document, std::string_view line)
{
    document += "<w:p><w:r>";
    std::size_t start = 0;
    // 使用原字符串视图切片，避免为每行 Markdown 额外构造中间字符串。
    while ( start <= line.size() ) {
        const auto end = line.find('\n', start);
        // npos 表示最后一段；末尾换行仍会进入下一轮空文本段。
        const auto length =
            end == std::string_view::npos ? line.size() - start : end - start;
        document += "<w:t xml:space=\"preserve\">";
        document += escapeXml(line.substr(start, length));
        document += "</w:t>";
        if ( end == std::string_view::npos ) break;
        document += "<w:br/>";
        start = end + 1;
    }
    document += "</w:r></w:p>";
}

/// @brief 为 XLSX 行追加一个内联字符串单元格。
/// @param sheet 正在构造的工作表 XML。
/// @param column 列字母，当前固定为 A 到 D。
/// @param row 一基行号。
/// @param value 保留换行的原始文本。
/// @details inlineStr 允许小规模批注表不构建 sharedStrings.xml。
/// 单元格地址由调用方提供的列和行拼出，文本只写入 is/t 节点。
/// xml:space=preserve 保留正文中的缩进以及结尾空格。
/// 作者、目标、位置即使为空也写单元格，以保持固定列结构。
/// 表格软件可在同一行稳定找到四个字段，不依赖空列推断。
void appendSheetCell(std::string& sheet, char column, std::size_t row,
                     std::string_view value)
{
    sheet += fmt::format(
        // 单元格地址与 row 节点的编号一致，空值不会改变列对齐。
        "<c r=\"{}{}\" t=\"inlineStr\"><is><t "
        "xml:space=\"preserve\">",
        column,
        row);
    sheet += escapeXml(value);
    sheet += "</t></is></c>";
}

/// @brief 把 OOXML 成员压缩到完整内存归档，失败时不打开目标文件。
/// @param entries 包内相对路径及 XML 文本。
/// @param output 接收归档字节。
/// @param error 接收首个失败原因。
/// @return ZIP 中央目录已完成时返回 true。
/// @details DOCX 和 XLSX 本质上是含固定部件路径的 ZIP 包。
/// 先在堆内完成所有成员与中央目录，才能原子地交给磁盘写入层。
/// XML 文件名来自代码内固定路径，用户正文只进入已转义的 XML 值。
/// 任一成员压缩失败后停止追加，保留第一个错误的成员路径。
/// finalize 后立即结束 writer，再复制归档内存，避免 writer 生命周期
/// 与调用方的输出缓冲区发生交叉所有权。
/// miniz 分配的堆缓冲只由 mz_free 释放；失败路径也执行清理。
bool packageOfficeXml(
    const std::vector<std::pair<std::string, std::string>>& entries,
    std::vector<std::uint8_t>& output, std::string& error)
{
    mz_zip_archive archive{};
    // miniz 的 heap writer 自己管理归档缓冲，调用方只接收完成品。
    if ( !mz_zip_writer_init_heap(&archive, 0, 0) ) {
        error = "无法初始化 Office 文档压缩器";
        return false;
    }
    bool success = true;
    // 成员追加按清单顺序执行，失败后不再加入后续工作表部件。
    for ( const auto& [name, content] : entries ) {
        // 文件名是编译期固定的 OOXML 部件路径，正文永远只作为内容写入。
        if ( !mz_zip_writer_add_mem(&archive,
                                    name.c_str(),
                                    content.data(),
                                    content.size(),
                                    MZ_BEST_COMPRESSION) ) {
            error   = "无法压缩 Office 文档成员：" + name;
            success = false;
            break;
        }
    }
    void* archiveBytes = nullptr;
    // finalize 负责生成中央目录；没有这一步 Office 包无法正常打开。
    std::size_t archiveSize = 0;
    if ( success && !mz_zip_writer_finalize_heap_archive(
                        &archive, &archiveBytes, &archiveSize) ) {
        error   = "无法完成 Office 文档归档";
        success = false;
    }
    mz_zip_writer_end(&archive);
    // writer 内部状态与 finalize 返回的堆归档按 API 分别释放。
    if ( success ) {
        const auto* begin = static_cast<const std::uint8_t*>(archiveBytes);
        output.assign(begin, begin + archiveSize);
    }
    if ( archiveBytes ) mz_free(archiveBytes);
    return success;
}

/// @brief 将完整输出先写到同目录临时文件，再替换目标。
/// @param path 用户选择的目标路径。
/// @param data 完整 TXT 或 ZIP 字节。
/// @param error 失败时接收具体阶段说明。
/// @return 目标已完整写入时返回 true。
/// @warning 用户触发的同步文件 IO；同一路径的导出由 UI 串行执行。
/// @details 临时文件位于目标同目录，正常 rename 不跨文件系统。
/// 写入和关闭都成功后才替换原目标，避免半个 ZIP 覆盖已有文件。
/// 标准 rename 在部分 Windows 文件系统上不能覆盖现有路径。
/// 因此复制覆盖仅作为 rename 失败时的兼容回退，并清理临时文件。
/// 两个文件操作均使用 error_code，导出失败不经异常机制传播。
/// 这里假设同一个 UI 操作不会并发导出到相同目标文件。
bool writeCompleteFile(const std::filesystem::path&     path,
                       const std::vector<std::uint8_t>& data,
                       std::string&                     error)
{
    if ( path.empty() || path.filename().empty() ) {
        error = "请选择有效的导出文件路径";
        return false;
    }
    auto tempPath = path;
    // 同名 .tmp 在每次导出时重写，避免上次失败留下的半成品被复用。
    tempPath += ".tmp";
    {
        std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
        if ( !stream ) {
            error = "无法创建导出临时文件";
            return false;
        }
        stream.write(reinterpret_cast<const char*>(data.data()),
                     // 数据由完整文本或完整 ZIP 归档产生，此处只做一次顺序写。
                     static_cast<std::streamsize>(data.size()));
        stream.close();
        if ( !stream ) {
            // 写入不完整时绝不触碰现有目标文件。
            std::error_code ignored;
            std::filesystem::remove(tempPath, ignored);
            error = "无法完整写入导出文件";
            return false;
        }
    }
    std::error_code replaceError;
    // 先尝试单步替换，成功时用户永远看不到中途的部分数据。
    std::filesystem::rename(tempPath, path, replaceError);
    if ( !replaceError ) return true;

    // Windows 不允许 rename 覆盖已有文件时沿用项目已有的覆盖复制回退。
    std::error_code copyError;
    // Windows 覆盖回退只在 rename 失败后触发，不主动删除旧目标。
    std::filesystem::copy_file(
        tempPath,
        path,
        std::filesystem::copy_options::overwrite_existing,
        copyError);
    std::error_code ignored;
    std::filesystem::remove(tempPath, ignored);
    if ( copyError ) {
        error = "无法替换导出文件：" + copyError.message();
        return false;
    }
    return true;
}
}  // namespace

/// @brief 按备注表顺序生成 TXT、DOCX 和 XLSX 共用的结构化内容。
/// @warning 用户显式导出路径：线性处理批注全文，不用于逐帧表格绘制。
/// @details 批注表已经解析目标引用并按真实时间排序，此处只读快照。
/// 即使编辑线程随后更新谱面，本轮导出的目标和正文仍来自同一版本。
/// 拍号换算复用表格本身的 BPM 与分拍上下文，不另造计时规则。
/// 拍号不可用时保留时间戳，保证每一条导出记录都可定位。
/// 作者与正文不做空值替换；空作者在文件中仍保持为空。
/// 格式化结果按值存储，后续压缩阶段不引用 UI 缓存的字符串。
std::vector<AnnotationExportRecord> buildAnnotationExportRecords(
    const std::vector<AnnotationTableRow>&    rows,
    const UI::Utils::CanvasTimeFormatContext& context,
    AnnotationExportPosition position, const AnnotationExportLabels& labels)
{
    std::vector<AnnotationExportRecord> records;
    // 记录数量等于快照行数，预分配可避免每条批注重复扩容。
    records.reserve(rows.size());
    for ( const auto& row : rows ) {
        // 时间戳始终先准备好，拍号无效时无需再算一遍时间文本。
        std::string location = formatExportTimestamp(row.timestamp);
        if ( position == AnnotationExportPosition::Beat ) {
            const auto beat =
                UI::Utils::calculateCanvasBeatPosition(row.timestamp, context);
            // 没有可用 BPM 时保留可靠的时间戳，而非写入无定位意义的空拍号。
            if ( beat.valid ) {
                location = fmt::format("#{} {}/{}",
                                       beat.beatNumber,
                                       beat.numerator,
                                       beat.denominator);
            }
        }
        records.push_back({ std::move(location),
                            // 目标描述只使用已经复制的轻量元数据。
                            formatExportTarget(row, labels),
                            row.author,
                            row.content });
    }
    return records;
}

/// @brief 保存 UTF-8 文本或标准 Office Open XML ZIP 文档。
/// @warning 用户显式导出路径：全部正文打包后才写磁盘。
/// @details 三种格式使用同一组已解析的记录，确保内容一致。
/// TXT 采用带 BOM 的 UTF-8，便于 Windows 上直接识别中文和 emoji。
/// DOCX 的主文档由 word/document.xml 提供，文件根关系指向它。
/// XLSX 用四列分别写位置、物件、作者和正文，首行是本地化表头。
/// 所有 Office 文本均经 XML 转义，原始 Markdown 标记仍保留为文本。
/// 若格式枚举无效，在创建磁盘文件前返回失败；空记录仍可导出。
/// 整个文档先在内存中构造，再经同目录临时文件写入目标。
/// 路径后缀由调用方按照所选格式规范化，此函数只决定实际文件编码。
/// 写入失败时返回原因供调用方日志记录，不假装导出成功。
/// 文本、Word 和 Excel 均按输入记录顺序输出，不重新排序备注。
/// 作者为空时保持空值，不用界面的“未知作者”占位文本替代。
/// ZIP 成员路径为规范固定值，后续可用标准归档工具独立检查。
bool writeAnnotationExportFile(
    const std::vector<AnnotationExportRecord>& records,
    const AnnotationExportLabels& labels, AnnotationExportFormat format,
    const std::filesystem::path& path, std::string& error)
{
    error.clear();
    // 统一清除上一次错误，避免本次成功仍把旧原因呈现给调用方。
    std::vector<std::uint8_t> bytes;
    if ( format == AnnotationExportFormat::Txt ) {
        // UTF-8 BOM 让 Windows 常见文本程序直接识别中文与 emoji。
        // 每条备注结束后补一个空行，连续多行正文依旧属于同一条记录。
        // 用户示例中的时间与轨道顺序由 formatRecordText 统一确定。
        std::string content = "\xEF\xBB\xBF";
        for ( const auto& record : records ) {
            // 文本保留 Markdown 语法，不改写用户输入的每行内容。
            content += formatRecordText(record, labels);
            content += "\n\n";
        }
        bytes.assign(content.begin(), content.end());
    } else {
        // XML 声明统一为 UTF-8；Office 包内各部件独立解析该声明。
        // 部件清单只包含实际写入的文件，不声明不存在的样式或媒体资源。
        constexpr std::string_view xmlHeader =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>";
        std::vector<std::pair<std::string, std::string>> entries;
        if ( format == AnnotationExportFormat::Docx ) {
            // 主文档仅依赖 w 命名空间，按备注顺序追加段落。
            // sectPr 结束正文，避免消费者误认为文档缺少最后一节。
            std::string document(xmlHeader);
            document +=
                "<w:document xmlns:w=\"http://schemas.openxmlformats.org/"
                "wordprocessingml/2006/main\"><w:body>";
            for ( const auto& record : records ) {
                // 备注表顺序即 Word 段落顺序，重复时间戳也不合并。
                appendWordText(document, formatRecordText(record, labels));
            }
            document += "<w:sectPr/></w:body></w:document>";
            entries = {
                // 内容类型清单将主文档标为 WordprocessingML 文档。
                // 默认 XML 与关系类型覆盖根关系文件和文档 XML。
                { "[Content_Types].xml",
                  std::string(xmlHeader) +
                      "<Types xmlns=\"http://schemas.openxmlformats.org/"
                      "package/2006/content-types\"><Default "
                      "Extension=\"rels\" "
                      "ContentType=\"application/vnd.openxmlformats-package."
                      "relationships+xml\"/><Default Extension=\"xml\" "
                      "ContentType=\"application/xml\"/><Override "
                      "PartName=\"/word/document.xml\" "
                      "ContentType=\"application/"
                      "vnd.openxmlformats-officedocument.wordprocessingml."
                      "document.main+xml\"/></Types>" },
                { "_rels/.rels",
                  // 根关系是包入口；Office 通过此路径找到正文。
                  std::string(xmlHeader) +
                      "<Relationships xmlns=\"http://schemas.openxmlformats."
                      "org/package/2006/relationships\"><Relationship "
                      "Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
                      "officeDocument/2006/relationships/officeDocument\" "
                      "Target=\"word/document.xml\"/></Relationships>" },
                { "word/document.xml", std::move(document) },
            };
        } else if ( format == AnnotationExportFormat::Xlsx ) {
            // 列宽只影响打开后的可读性，不改变单元格真实字符串。
            // 第一列为位置，第二列为目标，第三列为作者，第四列为全文。
            std::string sheet(xmlHeader);
            sheet +=
                "<worksheet xmlns=\"http://schemas.openxmlformats.org/"
                "spreadsheetml/2006/main\"><cols>"
                "<col min=\"1\" max=\"1\" width=\"19\" customWidth=\"1\"/>"
                "<col min=\"2\" max=\"2\" width=\"25\" customWidth=\"1\"/>"
                "<col min=\"3\" max=\"3\" width=\"18\" customWidth=\"1\"/>"
                "<col min=\"4\" max=\"4\" width=\"64\" customWidth=\"1\"/>"
                "</cols><sheetData>";
            const auto appendRow = [&sheet](std::size_t        index,
                                            const std::string& a,
                                            const std::string& b,
                                            const std::string& c,
                                            const std::string& d) {
                // 行号和单元格地址必须同步；电子表格按地址定位字段。
                // 固定四列让空作者不会使正文向左偏移。
                sheet += fmt::format("<row r=\"{}\">", index);
                appendSheetCell(sheet, 'A', index, a);
                appendSheetCell(sheet, 'B', index, b);
                appendSheetCell(sheet, 'C', index, c);
                appendSheetCell(sheet, 'D', index, d);
                sheet += "</row>";
            };
            appendRow(1,
                      // 表头与界面语言一致，用户无需猜测列的语义。
                      labels.positionHeader,
                      labels.targetHeader,
                      labels.authorHeader,
                      labels.contentHeader);
            for ( std::size_t i = 0; i < records.size(); ++i ) {
                // 工作表首行已用于标题，数据从第二行开始。
                const auto& record = records[i];
                appendRow(i + 2,
                          record.position,
                          record.target,
                          record.author,
                          record.content);
            }
            sheet += "</sheetData></worksheet>";
            // sheetData 必须在压缩前闭合，避免生成 XML 不完整的文件。
            entries = {
                // XLSX 主内容类型是 workbook，worksheet 单独声明。
                // 根关系先进入 workbook，再由 workbook 关系进入 sheet1。
                { "[Content_Types].xml",
                  std::string(xmlHeader) +
                      "<Types xmlns=\"http://schemas.openxmlformats.org/"
                      "package/2006/content-types\"><Default "
                      "Extension=\"rels\" "
                      "ContentType=\"application/vnd.openxmlformats-package."
                      "relationships+xml\"/><Default Extension=\"xml\" "
                      "ContentType=\"application/xml\"/><Override "
                      "PartName=\"/xl/workbook.xml\" ContentType=\"application/"
                      "vnd.openxmlformats-officedocument.spreadsheetml.sheet."
                      "main+xml\"/><Override "
                      "PartName=\"/xl/worksheets/sheet1.xml\" "
                      "ContentType=\"application/"
                      "vnd.openxmlformats-officedocument."
                      "spreadsheetml.worksheet+xml\"/></Types>" },
                { "_rels/.rels",
                  // 包入口不直接指向工作表，否则常见消费者无法识别文档。
                  std::string(xmlHeader) +
                      "<Relationships xmlns=\"http://schemas.openxmlformats."
                      "org/package/2006/relationships\"><Relationship "
                      "Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
                      "officeDocument/2006/relationships/officeDocument\" "
                      "Target=\"xl/workbook.xml\"/></Relationships>" },
                { "xl/workbook.xml",
                  // sheetId 是工作簿内编号；r:id 对应下方关系表。
                  std::string(xmlHeader) +
                      "<workbook xmlns=\"http://schemas.openxmlformats.org/"
                      "spreadsheetml/2006/main\" xmlns:r=\"http://schemas."
                      "openxmlformats.org/officeDocument/2006/relationships\">"
                      "<sheets><sheet name=\"Annotations\" sheetId=\"1\" "
                      "r:id=\"rId1\"/></sheets></workbook>" },
                { "xl/_rels/workbook.xml.rels",
                  // worksheet 关系路径相对于 xl/workbook.xml 所在目录。
                  std::string(xmlHeader) +
                      "<Relationships xmlns=\"http://schemas.openxmlformats."
                      "org/package/2006/relationships\"><Relationship "
                      "Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
                      "officeDocument/2006/relationships/worksheet\" "
                      "Target=\"worksheets/sheet1.xml\"/></Relationships>" },
                { "xl/worksheets/sheet1.xml", std::move(sheet) },
            };
        } else {
            // 未识别枚举不得猜测格式，也不能留下误导性的目标文件。
            error = "不支持的批注导出格式";
            return false;
        }
        // 压缩完成后才进入磁盘替换路径，失败不会破坏已有导出文件。
        if ( !packageOfficeXml(entries, bytes, error) ) return false;
    }
    // TXT 与 Office 文件共用磁盘提交路径，保证覆盖与失败行为一致。
    return writeCompleteFile(path, bytes, error);
}

}  // namespace MMM::Canvas
