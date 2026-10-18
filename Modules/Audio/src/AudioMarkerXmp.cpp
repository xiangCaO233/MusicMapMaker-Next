#include "AudioMarkerInternal.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace MMM::Audio::MarkerInternal
{
/// @brief 生成 Adobe 采用的同主文件名配套路径。
/// @param audio 已规范化的导出音频路径，不访问文件系统。
/// @return 与音频同目录、主文件名相同的 .xmp 路径。
/// @details 不追加音频扩展名，因此同名不同格式导出也必须检查 XMP 冲突。
/// 调用方负责检查已有文件，路径生成本身不授权覆盖元数据。
std::filesystem::path xmpPath(const std::filesystem::path& audio)
{
    auto path = audio;
    path.replace_extension(".xmp");
    return path;
}

/// @brief 将名称转换为合法的 XML 1.0 文本，保留中文及换行语义。
/// @param text 来自已验证标记快照的 UTF-8 名称。
/// @return 不含裸 XML 元字符及非法 XML 控制字符的文本。
/// @details 非法 UTF-8 与 XML 禁用字符替换为 U+FFFD，仅影响外部显示名称。
/// 精确原始测量数据继续由既有元数据通道保存，不从 XMP 反推。
static std::string escapeXml(const std::string& text)
{
    // 使用已有 JSON 编码器统一修复外部非法 UTF-8，避免生成不可解析的 XML。
    const auto encoded = nlohmann::json(text).dump(
        -1, ' ', false, nlohmann::json::error_handler_t::replace);
    const auto valid =
        nlohmann::json::parse(encoded, nullptr, false).get<std::string>();
    std::string result;
    // 名称处理仅在后台逐标记执行，输出字符串不与 UI 草稿共享存储。
    // 使用字节遍历保留合法 UTF-8，只有 XML 禁用的码点需要额外识别。
    for ( std::size_t i = 0; i < valid.size(); ++i ) {
        const auto c = static_cast<unsigned char>(valid[i]);
        // XML 1.0 排除 U+FFFE/U+FFFF；其余有效多字节编码可以直接复制。
        if ( c == 0xef && i + 2 < valid.size() &&
             static_cast<unsigned char>(valid[i + 1]) == 0xbf &&
             (static_cast<unsigned char>(valid[i + 2]) == 0xbe ||
              static_cast<unsigned char>(valid[i + 2]) == 0xbf) ) {
            result += "&#xFFFD;";
            i += 2;
            continue;
        }
        // 回车用字符引用绕过 XML 换行归一化，制表符与换行直接保留。
        // 所有元字符均转义，章节名不能注入元素或结束当前标记轨道。
        switch ( c ) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case 0x27: result += "&apos;"; break;
        case '\r': result += "&#13;"; break;
        default:
            // XML 1.0 无法用数字引用表示其禁止的控制字符。
            // 替换这些字符而非删除整条标记，保留该时间点的可导航性。
            if ( c < 0x20 && c != '\t' && c != '\n' )
                result += "&#xFFFD;";
            else
                result.push_back(static_cast<char>(c));
        }
    }
    return result;
}

/// @brief 输出与 Audition 样本一致的 DynamicMedia Cue 标记轨道。
/// @param path 事务所有的临时 XMP 路径，不能传入既有用户文件。
/// @param visible 已排序、去重的非负标记，包含用户选择的整拍。
/// @param error 文件写入失败时的原因。
/// @return 关闭文件并刷新缓冲成功时返回 true。
/// @pre 输入来自服务层筛选后的可见列表，不可直接传入未经校验的测量数据。
/// @details 使用 f1000000 表示每秒一百万刻度，独立于音频采样率。
/// startTime 从音频开头计时，微秒取整误差不超过半微秒。
/// 标记 GUID 与 marker_guid 参数保持一致，供 AU 后续编辑关联。
/// 不伪造 Adobe 编辑历史、音频哈希或来源歌曲信息。
/// @warning 仅在后台导出事务调用，不参与播放或 UI 每帧更新。
bool writeXmp(const std::filesystem::path&     path,
              const std::vector<AudioChapter>& visible, std::string& error)
{
    // 双时钟组成当前文档的标识前缀，序号区分同一文档内的标记。
    // 标识是 XMP 文本字段，不承诺随机 UUID 或内容摘要语义。
    const auto document =
        std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count()) +
        "-" +
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
    std::ofstream output(path, std::ios::binary);
    // 二进制模式保持各平台相同的 UTF-8 packet，不让 Windows 改写换行。
    // xpacket 的 begin 值是 U+FEFF；固定 id 是 XMP packet 的标准识别串。
    // RDF 命名空间用于描述资源结构，DynamicMedia 命名空间承载时间标记。
    // Tracks 使用无序 Bag，轨道中的 markers 则使用有序 Seq 保存时间顺序。
    // 只创建一条 Cue 轨道，不伪造没有内容的 CD Track 或 Subclip 轨道。
    output << "<?xpacket begin=\"\xEF\xBB\xBF\" "
              "id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
              "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
              "<rdf:RDF "
              "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
              "<rdf:Description rdf:about=\"\" "
              "xmlns:xmpDM=\"http://ns.adobe.com/xmp/1.0/DynamicMedia/\">\n"
              "<xmpDM:Tracks><rdf:Bag><rdf:li rdf:parseType=\"Resource\">\n"
              "<xmpDM:trackName>CuePoint Markers</xmpDM:trackName>\n"
              "<xmpDM:trackType>Cue</xmpDM:trackType>\n"
              "<xmpDM:frameRate>f1000000</xmpDM:frameRate>\n"
              "<xmpDM:markers><rdf:Seq>\n";
    for ( std::size_t i = 0; i < visible.size(); ++i ) {
        // 可见列表由服务层与其他容器章节共用，避免多种格式出现不同拍点。
        const auto& chapter = visible[i];
        // 锚点范围及拍点展开数量受上层预算约束，微秒转换不会溢出 64 位。
        // 不累加间隔，长音频和变 BPM 的拍线不会因导出再次产生累计漂移。
        const auto ticks =
            static_cast<std::int64_t>(std::round(chapter.seconds * 1000000.0));
        const auto guid =
            "xmp:id:mmm-" + document + "-" + std::to_string(i + 1);
        // cuePointParams 的 key/value 是 AU 样本中的标记身份关联约定。
        // guid 与参数值来自同一变量，后续增添其他参数也不能分配第二个身份。
        // 未指定 duration 表示单点标记，与输入的章节起点语义一致。
        output << "<rdf:li rdf:parseType=\"Resource\"><xmpDM:startTime>"
               << ticks << "</xmpDM:startTime><xmpDM:name>"
               << escapeXml(chapter.title)
               << "</xmpDM:name><xmpDM:cuePointParams><rdf:Seq>"
                  "<rdf:li "
                  "rdf:parseType=\"Resource\"><xmpDM:key>marker_guid</"
                  "xmpDM:key><xmpDM:value>"
               << guid
               << "</xmpDM:value></rdf:li></rdf:Seq></xmpDM:cuePointParams>"
                  "<xmpDM:guid>"
               << guid << "</xmpDM:guid></rdf:li>\n";
    }
    // 显式结束 XMP packet，并在发布前验证关闭状态，磁盘写满不能误报成功。
    // 外部程序只读取完整发布后的 .xmp，临时文件由上层统一回收。
    output
        << "</rdf:Seq></xmpDM:markers></rdf:li></rdf:Bag></xmpDM:Tracks>\n"
           "</rdf:Description></rdf:RDF></x:xmpmeta>\n<?xpacket end=\"w\"?>\n";
    output.close();
    if ( !output ) {
        error = "AU XMP 标记文件写入失败";
        return false;
    }
    return true;
}
}  // namespace MMM::Audio::MarkerInternal
