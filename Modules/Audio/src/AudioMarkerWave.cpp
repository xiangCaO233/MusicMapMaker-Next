#include "AudioMarkerInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>

namespace MMM::Audio::MarkerInternal
{
namespace
{
/// @brief 读取 RIFF 的固定小端整数，不依赖主机字节序或未对齐访问。
/// @param bytes 至少包含四个字节的已验证缓冲。
/// @return 不依赖 CPU 字节序的无符号数值。
/// @details 每个字节先扩展为 unsigned char，避免符号扩展污染高位。
/// 访问边界由调用点保证，不能直接传入未验证的外部块地址。
static std::uint32_t read32(const char* bytes)
{
    std::uint32_t value = 0;
    for ( int i = 0; i < 4; ++i )
        value |=
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i]))
            << (8 * i);
    return value;
}

/// @brief 向块缓冲追加一个小端无符号整数。
/// @param bytes 当前块构造缓冲，由调用方独占。
/// @param value 要追加的 32 位字段。
/// @details 使用低字节先写的 RIFF 约定，不通过结构体内存布局保存。
/// 输出缓冲可包含零字节，不按字符串结束符截断。
static void append32(std::string& bytes, std::uint32_t value)
{
    for ( int i = 0; i < 4; ++i )
        bytes.push_back(static_cast<char>((value >> (8 * i)) & 255));
}

/// @brief 写出 RIFF 子块，块长度不包含奇数字节对齐填充。
/// @param output 当前临时文件输出流。
/// @param id 正好四字节的 RIFF 块类型。
/// @param bytes 尚未添加 RIFF 头和填充的块载荷。
/// @details 写入失败由最终流状态统一报告。
/// 载荷是值语义字符串，只借用其内存，不保存悬空视图。
/// 奇数长度补一个零字节，供下一块恢复偶数字节对齐。
static void writeChunk(std::ostream& output, const char* id,
                       const std::string& bytes)
{
    std::string header(id, 4);
    append32(header, static_cast<std::uint32_t>(bytes.size()));
    output.write(header.data(), 8);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if ( bytes.size() & 1 ) output.put('\0');
}

/// @brief 一个经过文件边界验证的 RIFF 子块。
struct WaveChunk {
    /// @brief 四字符类型，使用固定数组避免每块分配。
    std::array<char, 4> id{};
    /// @brief 有效载荷在文件内的位置。
    std::uint64_t offset{ 0 };
    /// @brief 载荷长度，不含头和对齐字节。
    std::uint32_t size{ 0 };
};

/// @brief 扫描 RIFF 索引，跳过音频载荷并拒绝越界或不完整块。
/// @details RF64 和 RIFX 留给无损复制加配套文件，不把它们误当 RIFF。
/// @param input 已打开的来源文件，游标须位于文件开头。
/// @param chunks 成功时保存所有子块的位置和大小。
/// @param sampleRate 从基本 fmt 字段恢复的真实采样率。
/// @return 文件头、块边界和采样率都合法时返回 true。
/// @details 文件长度与 RIFF 声明长度同时检查，截短文件不能继续读取。
/// 每个块的对齐长度使用 64 位加法，避免 uint32 溢出绕过边界。
/// 跳过载荷时只移动游标，内存需求由块数量而非音频大小决定。
/// 只支持一个 RIFF 根；未声明的尾部字节不属于标准音频块。
/// 不处理 RF64 的 ds64 大长度映射，交给上层配套无损复制。
/// 出现过多块时拒绝，防止构造大量零长度块占满索引。
static bool scanWave(std::ifstream& input, std::vector<WaveChunk>& chunks,
                     std::uint32_t& sampleRate)
{
    std::array<char, 12> header{};
    input.read(header.data(), header.size());
    if ( !input || std::memcmp(header.data(), "RIFF", 4) ||
         std::memcmp(header.data() + 8, "WAVE", 4) )
        return false;
    input.seekg(0, std::ios::end);
    const auto          fileLength = input.tellg();
    const std::uint64_t end =
        static_cast<std::uint64_t>(read32(header.data() + 4)) + 8;
    if ( fileLength < 12 || end < 12 ||
         end > static_cast<std::uint64_t>(fileLength) )
        return false;
    /// 根头之后从第一个子块开始，索引不能含根头本身。
    /// 声明长度以根块起点加八计算，与子块头的大小语义一致。
    /// 遍历终点同时受物理文件和 RIFF 声明边界约束。
    std::uint64_t cursor = 12;
    while ( cursor < end ) {
        if ( end - cursor < 8 || chunks.size() >= MAX_MARKERS ) return false;
        std::array<char, 8> chunkHeader{};
        input.seekg(static_cast<std::streamoff>(cursor));
        input.read(chunkHeader.data(), chunkHeader.size());
        if ( !input ) return false;
        WaveChunk chunk;
        std::copy_n(chunkHeader.data(), 4, chunk.id.data());
        chunk.offset = cursor + 8;
        chunk.size   = read32(chunkHeader.data() + 4);
        const std::uint64_t padded =
            static_cast<std::uint64_t>(chunk.size) + (chunk.size & 1);
        // 验证填充字节也在声明范围内，奇数载荷不能越过根块尾。
        // 下一个游标仅来自当前合法边界，不信任载荷内部偏移。
        // fmt 的基本字段读取前另查最小长度，避免从下一块取采样率。
        if ( padded > end - chunk.offset ) return false;
        if ( !std::memcmp(chunk.id.data(), "fmt ", 4) ) {
            // 基本 fmt 中的采样率位置对 PCM、浮点及扩展格式保持一致。
            if ( chunk.size < 16 ) return false;
            std::array<char, 8> format{};
            input.read(format.data(), format.size());
            if ( !input ) return false;
            sampleRate = read32(format.data() + 4);
        }
        chunks.push_back(chunk);
        cursor = chunk.offset + padded;
    }
    return sampleRate != 0;
}

/// @brief 只读取有预算的标记块，绝不把 data 块载入字符串。
/// @param input 扫描过的来源文件流。
/// @param chunk 通过文件边界验证的标记块。
/// @param bytes 调用方复用的读取缓冲。
/// @return 大小未超预算且完整读取时返回 true。
/// @details 调用方不得用本 helper 读取音频 data 块。
/// 每次按索引 seek，不依赖上一块读取后的游标位置。
/// 失败时不允许继续解析可能不完整的缓冲。
static bool readChunk(std::ifstream& input, const WaveChunk& chunk,
                      std::string& bytes)
{
    if ( chunk.size > MAX_METADATA_BYTES ) return false;
    bytes.resize(chunk.size);
    input.seekg(static_cast<std::streamoff>(chunk.offset));
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(input);
}

/// @brief 使用固定缓冲复制一个块的原始头、载荷和填充。
/// @details fmt 与 data 都按字节复制，保持位深、声道、采样率及 PCM 样本。
/// @param input 保持打开的来源文件。
/// @param output 事务拥有的临时文件。
/// @param chunk 来源 RIFF 块的边界索引。
/// @return 原始头、载荷和填充均复制成功时返回 true。
/// @details 固定 64KiB 栈缓冲限制内存，不把长音频读入 vector。
/// 块头也复制，保留原厂商类型与长度字段。
/// 每次短读立即失败，不能在磁盘错误后发布截短音频。
/// 循环剩余字节使用 uint64，包含头和填充时不会发生 32 位加法溢出。
static bool copyChunk(std::ifstream& input, std::ofstream& output,
                      const WaveChunk& chunk)
{
    std::array<char, 65536> buffer{};
    std::uint64_t           remaining =
        8 + static_cast<std::uint64_t>(chunk.size) + (chunk.size & 1);
    input.seekg(static_cast<std::streamoff>(chunk.offset - 8));
    while ( remaining ) {
        const auto count = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), count);
        output.write(buffer.data(), count);
        if ( !input || !output ) return false;
        remaining -= static_cast<std::uint64_t>(count);
    }
    return true;
}
}  // namespace

/// @brief 读取 RIFF/WAVE 标记而不扫描或解码 PCM 样本。
/// @param path 具有原生路径语义的音频文件名。
/// @return 合法 WAV 无标记时返回成功空列表，私有载荷损坏则失败。
/// @details 先建立有界块索引，cue 和 adtl 的出现顺序不影响标签关联。
/// 私有 mmmt 块保留双精度时间、负首拍与变速段 BPM。
/// 标准 cue 只保存样本位置，labl 使用 cue 的唯一 ID 关联名称。
/// 没有私有数据时只恢复章节，不凭 BPM 字样猜测精确节拍信息。
/// 未知 LIST 类别不分配完整块缓冲，避免 INFO 导致无界读取。
/// RIFF 外的 RF64 或大端 RIFX 返回未处理，由上层选择配套方案。
/// @warning 后台低频路径，任何块访问都不应移动到波形每帧绘制中。
AudioMarkerReadResult readWave(const std::filesystem::path& path)
{
    std::ifstream          input(path, std::ios::binary);
    std::vector<WaveChunk> chunks;
    std::uint32_t          sampleRate = 0;
    if ( !scanWave(input, chunks, sampleRate) ) return {};
    AudioMarkerReadResult result{ true, {}, {} };
    /// cue 和 labl 可以位于不同 LIST 顺序，因此先按 ID 建立关联。
    /// 位置和名称暂存在当前读取域，不把字节缓冲地址保存为长期引用。
    /// 索引数量受 cue 数量预算约束。
    std::map<std::uint32_t, std::uint32_t> positions;
    std::map<std::uint32_t, std::string>   labels;
    std::string                            bytes;
    for ( const auto& chunk : chunks ) {
        // 私有块具有完整版本数据，优先级高于样本粒度的标准 cue。
        // 损坏私有块必须报告错误，不能悄悄只恢复一部分章节。
        // 标准元数据始终保留供其他软件使用，但不覆盖精确锚点。
        if ( !std::memcmp(chunk.id.data(), "mmmt", 4) ) {
            // 精确私有块优先，标准 cue 仅用于外部软件和没有私有块的音频。
            if ( !readChunk(input, chunk, bytes) ||
                 !decodeData(bytes, result.data) ) {
                return { false, "WAV 测量标记损坏或版本不支持", {} };
            }
            return result;
        }
        if ( !std::memcmp(chunk.id.data(), "cue ", 4) ) {
            if ( !readChunk(input, chunk, bytes) || bytes.size() < 4 )
                return { false, "WAV cue 块损坏", {} };
            const auto count = read32(bytes.data());
            if ( count > MAX_MARKERS ||
                 4 + static_cast<std::uint64_t>(count) * 24 > bytes.size() )
                return { false, "WAV cue 数量无效", {} };
            for ( std::uint32_t i = 0; i < count; ++i ) {
                const auto* cue = bytes.data() + 4 + i * 24;
                // 只解释直接引用 data 的样本偏移，不猜测复杂 wavl 定位。
                if ( !std::memcmp(cue + 8, "data", 4) )
                    positions[read32(cue)] = read32(cue + 20);
            }
        }
        if ( !std::memcmp(chunk.id.data(), "LIST", 4) ) {
            // INFO 可能很大；先读四字符分类，仅对 adtl 分配缓冲。
            std::array<char, 4> kind{};
            input.seekg(static_cast<std::streamoff>(chunk.offset));
            if ( chunk.size < 4 ) continue;
            input.read(kind.data(), kind.size());
            if ( std::memcmp(kind.data(), "adtl", 4) ) continue;
            if ( !readChunk(input, chunk, bytes) )
                return { false, "WAV 标签过大", {} };
            std::size_t cursor = 4;
            while ( cursor + 8 <= bytes.size() ) {
                /// 全部块写完后才知道 RIFF 总长度，包括新标签的对齐字节。
                /// 长度超过 32 位上限时不能写入截断字段，事务将回退到配套复制。
                /// 回填只修改临时文件头，不接触输入文件。
                const auto length = read32(bytes.data() + cursor + 4);
                if ( static_cast<std::uint64_t>(length) + 8 >
                     bytes.size() - cursor )
                    return { false, "WAV 标签损坏", {} };
                if ( !std::memcmp(bytes.data() + cursor, "labl", 4) &&
                     length >= 4 ) {
                    const auto id = read32(bytes.data() + cursor + 8);
                    const std::string_view label(bytes.data() + cursor + 12,
                                                 length - 4);
                    labels[id] = std::string(label.substr(0, label.find('\0')));
                }
                cursor += 8 + static_cast<std::size_t>(length) + (length & 1);
            }
        }
    }
    for ( const auto& [id, sample] : positions ) {
        // 原生 cue 的粒度由采样率决定，精确 BPM 不由名称反推。
        result.data.chapters.push_back(
            { static_cast<double>(sample) / sampleRate, labels[id] });
    }
    std::stable_sort(
        result.data.chapters.begin(),
        result.data.chapters.end(),
        [](const auto& a, const auto& b) { return a.seconds < b.seconds; });
    return result;
}

/// @brief 流式复制原 WAV 音频块并生成新的时间标记块。
/// @param inputPath 只读来源，不打开写权限。
/// @param outputPath 当前事务的临时路径，不覆盖已存在的最终文件。
/// @param data 精确章节和 BPM，写入版本化 mmmt 块。
/// @param visible 非负有序外部标记，写入标准 cue 和 adtl 块。
/// @param error 失败时的可报告原因。
/// @return 所有块和 RIFF 总长度均完成写出时返回 true。
/// @details fmt 与 data 块连同填充按原字节复制，不改变音频采样数据。
/// 已存在的 cue、adtl 和旧 mmmt 由当前完整列表替换。
/// 其他块保持原顺序及原载荷，不删除来源备注或厂商附加信息。
/// 标准 cue 按来源采样率定位，不使用引擎内部重采样时钟。
/// 每个 cue ID 从一开始编号，labl 通过同一个 ID 关联名称。
/// 奇数块长度单独补齐，长度字段永远不包含填充字节。
/// RIFF 大小在所有块写完后回填，失败时不会发布残缺最终文件。
/// 负首拍与亚样本精度仅保留在私有块，不截断成无符号 cue 偏移。
bool writeWave(const std::filesystem::path&     inputPath,
               const std::filesystem::path&     outputPath,
               const AudioMarkerData&           data,
               const std::vector<AudioChapter>& visible, std::string& error)
{
    std::ifstream          input(inputPath, std::ios::binary);
    std::vector<WaveChunk> chunks;
    std::uint32_t          sampleRate = 0;
    if ( !scanWave(input, chunks, sampleRate) ) {
        error = "非标准 RIFF/WAVE，使用配套标记";
        return false;
    }
    std::ofstream output(outputPath, std::ios::binary);
    output.write("RIFF\0\0\0\0WAVE", 12);
    for ( const auto& chunk : chunks ) {
        // RIFF 标准只允许一个 cue 块；已有 cue 与 adtl 将由完整列表替换。
        bool marker = !std::memcmp(chunk.id.data(), "cue ", 4) ||
                      !std::memcmp(chunk.id.data(), "mmmt", 4);
        if ( !std::memcmp(chunk.id.data(), "LIST", 4) && chunk.size >= 4 ) {
            std::array<char, 4> kind{};
            input.seekg(static_cast<std::streamoff>(chunk.offset));
            input.read(kind.data(), kind.size());
            marker = !std::memcmp(kind.data(), "adtl", 4);
        }
        if ( !marker && !copyChunk(input, output, chunk) ) {
            error = "WAV 块复制失败";
            return false;
        }
    }
    /// 标准 cue 表的第一项是记录数量，后面每条记录固定为六个字段。
    /// 每条记录直接引用 data 块，chunkStart 和 blockStart 设为零。
    /// 样本偏移表达一帧的位置，不能乘以声道数或每帧字节数。
    std::string cue;
    append32(cue, static_cast<std::uint32_t>(visible.size()));
    // 标签区是 RIFF LIST 的 adtl 类型，关联使用 cue 的唯一整数 ID。
    // 名称允许零终止 UTF-8，大小字段包含终止符而不包含对齐填充。
    // 用户名称保存在私有 JSON 中，标准标签的编码兼容性不影响正式恢复。
    std::string labels = "adtl";
    for ( std::size_t i = 0; i < visible.size(); ++i ) {
        const double sample = std::round(visible[i].seconds * sampleRate);
        // cue 的位置只有 32 位，不能通过截断生成错误定位。
        if ( sample < 0.0 ||
             sample > std::numeric_limits<std::uint32_t>::max() ) {
            error = "WAV cue 样本位置超限";
            return false;
        }
        const auto id = static_cast<std::uint32_t>(i + 1);
        append32(cue, id);
        append32(cue, static_cast<std::uint32_t>(sample));
        cue += "data";
        append32(cue, 0);
        append32(cue, 0);
        append32(cue, static_cast<std::uint32_t>(sample));
        std::string label;
        append32(label, id);
        label += visible[i].title;
        label.push_back('\0');
        labels += "labl";
        append32(labels, static_cast<std::uint32_t>(label.size()));
        labels += label;
        if ( label.size() & 1 ) labels.push_back('\0');
    }
    writeChunk(output, "cue ", cue);
    writeChunk(output, "LIST", labels);
    writeChunk(output, "mmmt", encodeData(data));
    // 全部块写完后才回填根长度，避免使用尚未加入标记的来源大小。
    // RIFF 长度不含前八字节，写入前先确认可由 32 位字段表示。
    // 超限文件走上层配套复制路径，不能截断成错误的大文件长度。
    const auto length = output.tellp();
    if ( length < 12 || static_cast<std::uint64_t>(length) - 8 >
                            std::numeric_limits<std::uint32_t>::max() ) {
        error = "WAV 大于 RIFF 长度限制";
        return false;
    }
    std::string size;
    append32(
        size,
        static_cast<std::uint32_t>(static_cast<std::streamoff>(length) - 8));
    output.seekp(4);
    output.write(size.data(), size.size());
    // 显式关闭让最终状态包含磁盘缓冲刷新结果。
    // 未成功落盘的容器不能通过回读或进入发布步骤。
    // 同格式 WAV 的 fmt 和 data 仍来自逐块原样复制。
    output.close();
    if ( !output ) {
        error = "WAV 标记写入失败";
        return false;
    }
    return true;
}
}  // namespace MMM::Audio::MarkerInternal
