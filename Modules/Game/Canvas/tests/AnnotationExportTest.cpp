#include "canvas/AnnotationExport.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <miniz.h>
#include <string>
#include <system_error>
#include <vector>

namespace
{
/// @brief 构造与中文示例一致的导出标签，覆盖 UTF-8 路径与正文。
/// @return 不依赖运行时翻译系统的稳定测试标签。
/// @details 物件名称取自界面翻译，但测试不启动 AppConfig 或 Lua。
/// 显式构造全部字段可以检查某个字段被遗漏时的文档表头退化。
/// 中文前缀还固定了一基轨道号和跨轨分隔符的展示形式。
/// 这组标签与业务导出器的参数契约一致，不模拟文件选择器。
/// 测试对象只负责文件内容，窗口交互由已有 UI 入口承担。
MMM::Canvas::AnnotationExportLabels makeLabels()
{
    return { .track           = "轨道",
             .bgmTrack        = "BGM轨道",
             .note            = "单键",
             .hold            = "长条",
             .flick           = "滑键",
             .polyline        = "折线",
             .polylineSubNote = "折线子段",
             .sample          = "采样",
             .playerObject    = "物件",
             .targetMissing   = "目标已丢失",
             .author          = "作者",
             .positionHeader  = "位置",
             .targetHeader    = "目标",
             .authorHeader    = "作者",
             .contentHeader   = "内容" };
}

/// @brief 读取构建树中的导出文本，失败时返回空串供断言识别。
/// @param path 刚导出的 UTF-8 TXT 路径。
/// @return 文件的原始字节。
/// @details 以二进制读取，才能同时检查 BOM 和换行的确切字节。
/// 内容无法打开时返回空串，后续文本断言自然失败。
/// 这里不解码 UTF-8，直接按预期 UTF-8 字面量查找。
/// 读取范围只包含本测试刚写到 build/test_output 的文件。
/// 生产代码使用同目录临时文件，测试只读取最终目标路径。
std::string readText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if ( !stream ) return {};
    return { std::istreambuf_iterator<char>(stream),
             std::istreambuf_iterator<char>() };
}

/// @brief 从 DOCX 或 XLSX ZIP 内读取指定 OOXML 成员。
/// @param path 构建树中的 Office 文件。
/// @param name Office 包内的标准成员路径。
/// @return 成员文本，归档或成员无效时为空串。
/// @details 不能只判断文件扩展名或 ZIP 魔数，必须能读取具体成员。
/// miniz 的 reader 在所有成功路径上调用 end 释放归档状态。
/// 解压后的堆字节由 miniz 分配，所以用 mz_free 配对释放。
/// 读取缺失成员得到空串，可覆盖 OOXML 部件清单遗漏。
/// 成员内容交给后续测试核对位置、作者和 XML 转义。
std::string readArchiveMember(const std::filesystem::path& path,
                              const char*                  name)
{
    mz_zip_archive archive{};
    const auto     file = path.string();
    if ( !mz_zip_reader_init_file(&archive, file.c_str(), 0) ) return {};
    std::size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&archive, name, &size, 0);
    std::string result;
    if ( data ) result.assign(static_cast<const char*>(data), size);
    if ( data ) mz_free(data);
    mz_zip_reader_end(&archive);
    return result;
}

/// @brief 覆盖用户示例中的时间戳、分拍和各类物件轨道描述。
/// @return 全部导出记录保持原始正文且定位正确时为 true。
/// @details 单个时间戳备注不应凭空追加目标物件描述。
/// 向左滑动的 Flick 固定负 dtrack 情况，防止范围被错误排序。
/// 折线根使用起止轨，折线子段还需保留所属关系和子类型。
/// Hold 与 Note 使用单轨类型名，音频采样使用 BGM 区局部轨。
/// 缺失目标仍保留正文，并以“目标已丢失”明确标识。
/// 同一组记录先测时间定位，再测 BPM 分拍和无 BPM 回退。
/// 正文包含 emoji、XML 保留字符和换行，后续文件测试再次覆盖。
bool testRecords(const MMM::Canvas::AnnotationExportLabels& labels)
{
    using namespace MMM::Canvas;
    std::vector<AnnotationTableRow> rows;
    rows.push_back({ .timestamp = 13.748, .content = "这里写的跟屎一样" });
    // 第一条来自用户所举的纯时间备注，验证无目标时的空描述。
    rows.push_back(
        { .timestamp      = 54.321,
          .targetKind     = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT,
          .exportTrack    = 3,
          .exportEndTrack = 0,
          .objectType     = AnnotationExportObjectType::Flick,
          .author         = "测试者",
          .content        = "这里是💩 & <说明>\n第二行" });
    // 起轨大于终轨的滑键应维持原方向，不能归一为升序轨道对。
    rows.push_back(
        { .timestamp      = 55.0,
          .targetKind     = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT,
          .exportTrack    = 0,
          .exportEndTrack = 1,
          .objectType     = AnnotationExportObjectType::Polyline,
          .content        = "折线内容" });
    // 折线目标只写起止轨，长路径本身仍由谱面编辑器负责查看。
    rows.push_back(
        { .timestamp   = 56.0,
          .targetKind  = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT,
          .exportTrack = 2,
          .objectType  = AnnotationExportObjectType::Hold,
          .content     = "长条内容" });
    // 长条没有横向尾轨，应保持单轨而非虚构跨轨范围。
    rows.push_back(
        { .timestamp   = 57.0,
          .targetKind  = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT,
          .exportTrack = 0,
          .objectType  = AnnotationExportObjectType::Note,
          .content     = "单键内容" });
    // 子段 Flick 和独立 Flick 共用类型，但增加折线子段前缀。
    rows.push_back(
        { .timestamp         = 58.0,
          .targetKind        = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT,
          .exportTrack       = 1,
          .exportEndTrack    = 3,
          .objectType        = AnnotationExportObjectType::Flick,
          .isPolylineSubNote = true,
          .content           = "折线子段内容" });
    // 音频采样脱离玩家轨道命名空间，编号从 BGM 区重新起算。
    rows.push_back(
        { .timestamp   = 59.0,
          .targetKind  = MMM::BeatmapAnnotationTargetKind::AUDIO_SAMPLE,
          .exportTrack = 0,
          .content     = "采样内容" });
    // 删除过目标的批注仍属于“全部内容”，不能在导出时丢掉。
    rows.push_back(
        { .timestamp     = 60.0,
          .targetKind    = MMM::BeatmapAnnotationTargetKind::PLAYER_OBJECT,
          .targetMissing = true,
          .content       = "物件已删除" });
    MMM::UI::Utils::CanvasTimeFormatContext context;
    context.bpmPoints   = { { 0.0, 120.0 } };
    context.beatDivisor = 8;
    // 120 BPM 提供精确的半拍场景，13.748 秒逼近但不等于网格线。

    // 按时间导出须保留用户给出的 分:秒:毫秒 写法和完整 Markdown 原文。
    const auto timed = buildAnnotationExportRecords(
        rows, context, AnnotationExportPosition::Timestamp, labels);
    if ( timed.size() != 8 || timed[0].position != "00:13:748" ||
         timed[1].position != "00:54:321" ||
         timed[1].target != "轨道4＞1滑键" ||
         timed[2].target != "轨道1＞2折线" || timed[3].target != "轨道3长条" ||
         timed[4].target != "轨道1单键" ||
         timed[5].target != "轨道2＞4折线子段滑键" ||
         timed[6].target != "BGM轨道1采样" || timed[7].target != "目标已丢失" ||
         timed[1].content.find("💩 & <说明>\n第二行") == std::string::npos ) {
        return false;
    }
    // 这里同时检查顺序和每个物件描述，避免只验证集合却打乱备注表序。
    // 按拍号导出沿用备注表的 BPM 计算；无 BPM 必须回退可靠时间戳。
    const auto beats = buildAnnotationExportRecords(
        rows, context, AnnotationExportPosition::Beat, labels);
    if ( beats[0].position != "#28 1/2" ) return false;
    // 清掉 BPM 后仍能通过时间戳定位，不输出看似有效的零拍号。
    context.bpmPoints.clear();
    const auto fallback = buildAnnotationExportRecords(
        rows, context, AnnotationExportPosition::Beat, labels);
    return fallback[0].position == "00:13:748";
}

/// @brief 导出并解析三种文件，确保 OOXML 正文、表头和转义值均存在。
/// @param output 构建树内的隔离输出目录。
/// @return TXT、DOCX 和 XLSX 均写入有效内容时为 true。
/// @details 同一组记录写入三种格式，检测不同序列化器的内容一致性。
/// 第一条备注无作者且含换行，防止空作者导致 XLSX 列错位。
/// 第二条使用拍号并包含作者，验证定位模式的结果原样落盘。
/// TXT 检查 BOM、原文及方括号作者；Office 检查 ZIP 内实际 XML。
/// XML 保留字符在 TXT 中不转义，在 Office 中必须转义。
/// 最少检查包入口、主文档或 workbook 及工作表成员。
/// 目录显式位于构建树，不依赖个人配置中的项目或最近文件列表。
bool testFiles(const std::filesystem::path& output)
{
    using namespace MMM::Canvas;
    std::error_code ec;
    std::filesystem::create_directories(output, ec);
    // 创建输出目录失败时测试立即失败，不退回源码资源目录。
    if ( ec ) return false;
    const auto                                labels = makeLabels();
    const std::vector<AnnotationExportRecord> records{
        { "00:13:748", "轨道1单键", "", "这里是💩 & <说明>\n第二行" },
        { "#91 7/8", "轨道1＞2折线", "测试者", "另一条" },
    };
    std::string error;
    const auto  txt  = output / "annotations.txt";
    const auto  docx = output / "annotations.docx";
    const auto  xlsx = output / "annotations.xlsx";
    // 三种格式逐一调用同一写文件入口，不用测试替身代替 ZIP 实现。
    if ( !writeAnnotationExportFile(
             records, labels, AnnotationExportFormat::Txt, txt, error) ||
         !writeAnnotationExportFile(
             records, labels, AnnotationExportFormat::Docx, docx, error) ||
         !writeAnnotationExportFile(
             records, labels, AnnotationExportFormat::Xlsx, xlsx, error) ) {
        return false;
    }
    // 文本内容包含 UTF-8 BOM、原始换行及作者，不丢弃多行备注。
    const auto plain = readText(txt);
    if ( !plain.starts_with("\xEF\xBB\xBF") ||
         plain.find("00:13:748 轨道1单键 这里是💩 & <说明>\n第二行") ==
             std::string::npos ||
         plain.find("#91 7/8 轨道1＞2折线 [作者: 测试者] 另一条") ==
             std::string::npos ) {
        return false;
    }
    // 字符串查找用完整定位和正文，避免只有标签或时间却漏掉批注。
    // Office 包不能只判断 ZIP 魔数，还要读取正文成员和 XML 转义结果。
    const auto word  = readArchiveMember(docx, "word/document.xml");
    const auto sheet = readArchiveMember(xlsx, "xl/worksheets/sheet1.xml");
    // Word 的 w:br 对应原始换行；Sheet 内保留原文换行和标签。
    // 主内容类型和 workbook 入口的存在性防止生成不可打开的裸 XML。
    return word.find("这里是💩 &amp; &lt;说明&gt;") != std::string::npos &&
           word.find("<w:br/>") != std::string::npos &&
           sheet.find("这里是💩 &amp; &lt;说明&gt;") != std::string::npos &&
           sheet.find("#91 7/8") != std::string::npos &&
           sheet.find("测试者") != std::string::npos &&
           !readArchiveMember(docx, "[Content_Types].xml").empty() &&
           !readArchiveMember(xlsx, "xl/workbook.xml").empty();
}
}  // namespace

/// @brief 运行备注导出回归测试，所有文件只写入构建树。
/// @return 全部内容和格式检查通过时为零。
/// @details 先检验独立记录格式，再写文件检查真实归档结构。
/// 任一阶段失败均返回非零，供 CTest 标记具体用例失败。
/// 测试运行不启动 UI，不访问用户的活动谱面和配置目录。
/// 输出使用构建时注入的目录，不将生成文件混入 tests/data。
/// 记录测试先于文件测试运行，定位规则错误时不再写无用输出。
/// 文件测试读取实际生成的归档成员，不检查构造过程内部细节。
/// 测试夹具使用固定中文内容，覆盖多字节编码的原样保留。
/// CTest 每次在独立进程中执行，不与其它 Canvas 用例共享状态。
/// 失败不删除输出，便于开发者检查文档包内的 XML。
/// 同一个输出目录可反复覆盖，覆盖行为也经过实际写文件路径。
/// 用例返回值只取最终检查结果，不依赖运行环境的图形设备。
int main()
{
    return testRecords(makeLabels()) &&
                   testFiles(MMM_ANNOTATION_EXPORT_TEST_OUTPUT)
               ? 0
               : 1;
}
