/// @file BeatmapLongScreenshotTest.cpp
/// @brief 验证正常编辑画布的静态长截图输出。
/// @details 测试从生产皮肤载入贴图与两套字体，使用同一音符几何。
/// 用独立 PNG 解码入口读取编码后的文件，并验证实际绘制的像素。
/// 裁剪测试含一个横跨两端的 Hold 和带水平节点的折线。
/// 整谱测试跨过指定的一分钟列界，检查长条在相邻列的位置。
/// 字体测试分别开启两个水印以及组合状态，不只验证输出尺寸。
/// 这些用例同时保护中文输出名、取消语义和覆盖前保留已有文件。
#include "ui/export/BeatmapLongScreenshot.h"

#include "config/EditorConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "logic/ecs/components/NoteComponent.h"

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
using MMM::NoteType;
using namespace MMM::UI;
using Color = std::array<unsigned char, 3>;

/// @brief 由独立 PNG 解码器读回的样本，检查实际写出的像素。
/// @details 测试访问像素前需要检查图像成功解码且尺寸符合约定。
/// 不暴露原始字节向 writer，避免测试和编码器共享同一实现假设。
struct Image {
    /// @brief 图片在 PNG 文件中声明的尺寸。
    int m_width{}, m_height{};
    /// @brief 解码器拥有的 RGB 像素跟随样本释放。
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> m_pixels{
        nullptr, &stbi_image_free
    };

    /// @brief 取固定测试坐标的像素；调用方先校验文件尺寸。
    /// @param x 列索引。
    /// @param y 行索引。
    /// @return 解码得到的 RGB，不额外做 PNG 颜色转换。
    Color pixel(int x, int y) const
    {
        const auto* p =
            m_pixels.get() + (static_cast<std::size_t>(m_width) * y + x) * 3;
        return { p[0], p[1], p[2] };
    }
};

/// @brief 以项目的 UTF-8 路径规则打开文件，再解码 PNG。
/// @param path 测试输出目录中的 UTF-8 文件名。
/// @return 成功时附带像素和尺寸，否则没有已解码的像素。
/// @note 通过文件流读入字节，让 Windows UTF-8 文件名也走项目转换规则。
Image readPng(const std::filesystem::path& path)
{
    std::ifstream                    input(path, std::ios::binary);
    const std::vector<unsigned char> raw(std::istreambuf_iterator<char>(input),
                                         std::istreambuf_iterator<char>{});
    Image                            decoded;
    int                              channels{};
    // 解码不共享生产端的 miniz 流或 PNG 块组装器。
    // 用这一路读回可以识别压缩流完整性、格式头错误和像素偏移。
    decoded.m_pixels.reset(stbi_load_from_memory(raw.data(),
                                                 static_cast<int>(raw.size()),
                                                 &decoded.m_width,
                                                 &decoded.m_height,
                                                 &channels,
                                                 STBI_rgb));
    return decoded;
}

/// @brief 取第一个时间栏前的水印区域，独立开关时与组合状态逐像素比较。
/// @param image 含一个或两个水印行的已解码图像。
/// @param start 首行起点，两个水印时第二行按固定间距定位。
/// @return 对应矩形的扫描顺序像素，直接用于图片间比较。
std::vector<Color> watermarkRows(const Image& image, int start)
{
    std::vector<Color> output;
    for ( int y = start; y < start + 30; ++y ) {
        for ( int x = 0; x < image.m_width; ++x )
            output.push_back(image.pixel(x, y));
    }
    return output;
}

/// @brief 组装正式编辑器中的物件组件，测试完整折线交给同一套画布批次。
/// @param type 正常编辑会话的物件类型。
/// @param time 秒域出发时间。
/// @param lane 从左向右的零基正式轨号。
/// @param duration Hold 的持续秒数，瞬时物件保持零。
/// @return 可以直接用于生产快照所需的根组件。
MMM::Logic::NoteComponent makeNote(NoteType type, double time, int lane,
                                   double duration = 0.0)
{
    MMM::Logic::NoteComponent note;
    note.m_type       = type;
    note.m_timestamp  = time;
    note.m_trackIndex = lane;
    note.m_duration   = duration;
    return note;
}

/// @brief 从真实皮肤生成几何，验证裁切、纹理、独立水印与 60 秒分列。
/// @param directory 已独占本测试用途的输出目录。
/// @return 零表示通过；非零值区分失败的验证阶段。
/// @details 多个模式共享同一个合成谱面，以便比较开关前后的像素。
/// 测试中故意把对象放在裁剪边界和列分界两侧，避免两段完全独立的谱面也通过。
/// 作者名采用默认 CJK 皮肤字体避免平台字体安装情况影响结果。
/// 最后复用已存在的输出测失败行为，源测试资源始终只读。
int run(const std::filesystem::path& directory)
{
    MMM::Config::EditorConfig config;
    BeatmapScreenshot         chart;
    chart.m_lanes     = 4;
    chart.m_begin     = 1.0;
    chart.m_end       = 2.0;
    auto& skin        = MMM::Config::SkinManager::instance();
    chart.m_asciiFont = skin.getFontPath("ascii");
    chart.m_cjkFont   = skin.getFontPath("cjk");
    chart.m_notes     = { makeNote(NoteType::NOTE, 0.75, 3),
                          makeNote(NoteType::HOLD, 0.25, 2, 2.0),
                          makeNote(NoteType::NOTE, 2.0, 0) };
    auto path         = makeNote(NoteType::POLYLINE, 1.25, 0, 0.65);
    path.m_subNotes   = { { NoteType::HOLD, 1.25, 0.25, 0 },
                          { NoteType::FLICK, 1.5, 0.0, 0, 1 },
                          { NoteType::HOLD, 1.5, 0.4, 1 } };
    chart.m_notes.push_back(std::move(path));
    auto flick     = makeNote(NoteType::FLICK, 1.75, 2);
    flick.m_dtrack = 1;
    chart.m_notes.push_back(std::move(flick));


    auto       prepared = prepareBeatmapScreenshot(chart, config);
    const auto output   = directory / MMM::Config::utf8ToPath("指定时段.png");
    if ( !prepared || prepared->m_columns.size() != 1 ||
         writeBeatmapScreenshot(*prepared, output) != ScreenshotResult::Saved )
        return 3;
    const auto png = readPng(output);
    // 宽度涵盖四条轨道和左右边缘，图高含时间栏与实际裁切纵距。
    // 先验证尺寸再访问固定坐标，失败时不会访问解码缓冲边界。
    if ( !png.m_pixels || png.m_width != 312 || png.m_height != 610 ) return 4;
    // Hold 在区间边缘仍有主体，超出区间的真头尾和普通 Tap 不会被画进来。
    // 像素近似判定允许皮肤着色阴影，不假定矩形一律使用纯色。
    const auto isBright = [](Color pixel) {
        return pixel[0] > 120 && pixel[1] > 120;
    };
    // 时段结束处的 Tap 应保留，其头部能在顶部时间边界清晰显示。
    // 区间之前的第四轨 Tap 不应被拉到第一行。
    if ( !isBright(png.pixel(57, 73)) || !isBright(png.pixel(189, 76)) ||
         !isBright(png.pixel(189, 550)) || isBright(png.pixel(210, 76)) ||
         isBright(png.pixel(255, 550)) )
        return 5;
    // 折线节点使用现有圆形纹理，中心亮而同半径对角没有方形填色。
    // 选择中心横向连接处可同时检测节点未被路径主体覆盖。
    // 对角线位置应该还保留背景，即便它处于之前的方形节点范围。
    // 固定分拍位置同时确保正常画布与输出时间轴的坐标约定一致。
    // 节点中心与对角像素比较可识别之前简化绘制的方形错误。
    // 测试同时依赖生产皮肤正确的连接位置。
    if ( !isBright(png.pixel(123, 313)) || isBright(png.pixel(113, 303)) )
        return 6;

    // 按位组合为仅 MMM、仅作者以及两种同时开启；关闭双方已在裁剪图中覆盖。
    // 不只检查高度，还用单项的真实像素与组合图逐行比对。
    // 若其中一个开关错误地同时控制两种水印，这两个样本就会相同。
    std::array<std::vector<Color>, 2> only;
    for ( int state = 1; state <= 3; ++state ) {
        prepared->m_showMmm = (state & 1) != 0;
        prepared->m_author  = (state & 2) != 0 ? "谱师测试" : "";
        const auto out =
            directory / ("watermark-" + std::to_string(state) + ".png");
        if ( writeBeatmapScreenshot(*prepared, out) != ScreenshotResult::Saved )
            return 7;
        const auto result = readPng(out);
        if ( !result.m_pixels || result.m_width != png.m_width ||
             result.m_height != png.m_height + (state == 3 ? 60 : 30) )
            return 8;
        // 启用的每一行都有笔画，组合开关不改变其中任一水印的内容。
        auto top = watermarkRows(result, 0);
        if ( std::ranges::all_of(top, [&](const auto& pixel) {
                 return pixel == result.pixel(0, 0);
             }) )
            return 9;
        if ( state < 3 )
            only[state - 1] = std::move(top);
        else if ( top != only[0] || watermarkRows(result, 30) != only[1] ||
                  only[0] == only[1] )
            return 10;
    }

    // 从零开始导整谱，60 秒边界上的 Hold 身体应出现在左右两列。
    // 使用较短的末列验证它保持像素比例，而不是被拉到和整列同高。
    // 长图使用已导过区间的同一份谱面，之前排除的 Tap 理应恢复。
    // 新增的 Hold 头尾跨过两列，跨界两侧都需要保留主体。
    chart.m_begin = 0.0;
    chart.m_end   = 62.0;
    chart.m_notes.push_back(makeNote(NoteType::HOLD, 59.5, 1, 1.0));
    const auto whole = prepareBeatmapScreenshot(chart, config);
    // 从绝对时间推导标签和区域，结束列不能被补成另一整分钟。
    // 文件实际写出后再读取，避免测试只验证内部布局元信息。
    const auto wholePath = directory / "whole.png";
    if ( !whole || whole->m_columns.size() != 2 ||
         whole->m_columns[0].m_label != "00:00.0000 - 01:00.0000" ||
         whole->m_columns[1].m_label != "01:00.0000 - 01:02.0000" ||
         whole->m_columns[1].m_height != 1026 ||
         writeBeatmapScreenshot(*whole, wholePath) != ScreenshotResult::Saved )
        return 11;
    // 实际文件应宽到能容纳两列，每列中物件不受前后水印状态影响。
    // 第一列顶端和第二列底端各检测长条，相反的裁切方向都能覆盖。
    const auto all = readPng(wholePath);
    if ( !all.m_pixels || all.m_width != 600 || all.m_height != 28930 ||
         !isBright(all.pixel(123, 100)) || !isBright(all.pixel(411, 1000)) ||
         !isBright(all.pixel(255, 28513)) )
        return 12;

    // 失败应在暂存阶段退出；已存在的 PNG 仍能读且原像素与尺寸一致。
    chart.m_end = chart.m_begin;
    if ( prepareBeatmapScreenshot(chart, config) ) return 13;
    // 取消必须在分配待提交内容之前返回。
    // 复用已存在文件可以检测取消后错删目标的回归。
    std::stop_source stop;
    stop.request_stop();
    if ( writeBeatmapScreenshot(*prepared, output, stop.get_token()) !=
         ScreenshotResult::Cancelled )
        return 14;
    prepared->m_asciiFont.clear();
    if ( writeBeatmapScreenshot(*prepared, output) !=
         ScreenshotResult::FontError )
        return 15;
    // 使用首次写出的像素作为参照，可检测误覆盖与文件截断。
    // 正常图片的尺寸、端点位置和非空 RGB 缺一不可。
    // 字体错误发生在提交前，所以后续可使用同一选择结果重试。
    const auto preserved = readPng(output);
    return preserved.m_pixels && preserved.m_height == png.m_height &&
                   preserved.pixel(57, 73) == png.pixel(57, 73)
               ? 0
               : 16;
}
}  // namespace

/// @brief 按生产资源约定装载皮肤，所有 PNG 仅写入当前构建目录。
/// @param argc 应包含三个独立参数。
/// @param argv 依次提供内置皮肤入口、翻译目录和构建输出根目录。
/// @return 皮肤、目录或 PNG 验证失败均以非零值返回。
/// @note 保留本轮 PNG 便于定位回归时对照整谱、裁剪和水印。
int main(int argc, char** argv)
{
    if ( argc != 4 ) return 1;
    if ( !MMM::Config::SkinManager::instance().loadSkin(
             argv[1], MMM::Config::utf8ToPath(argv[2])) )
        return 2;
    const auto directory =
        MMM::Config::utf8ToPath(argv[3]) / "beatmap-screenshot";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return error ? 2 : run(directory);
}
