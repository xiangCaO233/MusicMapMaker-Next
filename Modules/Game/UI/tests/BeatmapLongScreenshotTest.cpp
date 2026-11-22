/// @file BeatmapLongScreenshotTest.cpp
/// @brief 验证正常编辑画布的静态长截图输出。
/// @details 测试从生产皮肤载入贴图与两套字体，使用同一音符几何。
/// 用独立 PNG 解码入口读取编码后的文件，并验证实际绘制的像素。
/// 裁剪测试含一个横跨两端的 Hold 和带水平节点的折线。
/// 整谱测试跨过默认十五秒列界，检查长条在相邻列的位置。
/// 自定义列宽通过时间标签及末列高度验证，网格开关直接比对 PNG 像素。
/// 纵向倍率覆盖压缩和放大两种情况，PNG 宽度须保持原轨道和列间布局。
/// 预览对照独立解码的数据，既覆盖原尺寸，也覆盖有跳行的缩略图。
/// 两种尺寸的比较都读取真实 PNG，避免只比对两个依赖同一布局的内部结构。
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
#include <limits>
#include <memory>
#include <span>
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

/// @brief 把预览像素与解码后的成品逐点对照，验证缩图仍来自同一个坐标系。
/// @param png 已成功解码的实际成品，测试从文件读取。
/// @param preview 生产预览入口返回的 RGBA8 缓冲。
/// @param region 预览对应的成品原始区域，覆盖整图或放大后的局部。
/// @return 所有采样点与对应的成品像素一致时返回 true。
/// @pre 调用方已经确认 PNG 可解码且对应同一组导出选项。
/// @details 用缩略图像素中心反查成品，不假定一比一映射。
/// 同一段断言也覆盖小于缩略图边长上限的成品，此时每个像素都会被验证。
/// 透明通道用于经由 UI 纹理呈现，即使原文件只有 RGB 也必须完全不透明。
bool matchesPreview(const Image& png, const BeatmapScreenshotPreview& preview,
                    ImageRegion region)
{
    // 先验证纹理尺寸和缓冲长度，回归时避免越界访问测试数据。
    if ( preview.m_width <= 0 || preview.m_width > region.m_width ||
         preview.m_height <= 0 || preview.m_height > region.m_height ||
         preview.m_pixels.size() !=
             static_cast<std::size_t>(preview.m_width) * preview.m_height * 4 )
        return false;
    for ( int y = 0; y < preview.m_height; ++y ) {
        const int sourceY =
            region.m_y + (2 * y + 1) * region.m_height / (2 * preview.m_height);
        for ( int x = 0; x < preview.m_width; ++x ) {
            // 纵向跳行时仍必须保留跨行的长条及之后的图元。
            // 横向跳点也应与输出对齐，尤其是列底和窄外框。
            const int  sourceX = region.m_x + (2 * x + 1) * region.m_width /
                                                  (2 * preview.m_width);
            const auto index   = (y * preview.m_width + x) * 4;
            if ( !std::ranges::equal(
                     png.pixel(sourceX, sourceY),
                     std::span(preview.m_pixels).subspan(index, 3)) ||
                 preview.m_pixels[index + 3] != 255 )
                return false;
        }
    }
    return true;
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

/// @brief 从真实皮肤生成几何，验证裁切、纹理、水印、分拍线开关和分列。
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

    // 小数倍率只改变纵向间距；节点的轨道和在时间窗中的相对位置保持一致。
    // 节点离选区末端半秒，在原图占 240 个像素；压缩后距边缘只有 150 像素。
    // 检查实际图元位置可以排除只减少空白或改变 PNG 声明尺寸的错误。
    chart.m_timelineZoomPercent = 62.5;
    const auto scaledSize = estimateBeatmapScreenshot(4, 1.0, 2.0, 15, 62.5, 0);
    const auto scaled     = prepareBeatmapScreenshot(chart, config);
    const auto scaledPath = directory / "zoom-62.5-percent.png";
    if ( !scaledSize || !scaled || scaledSize->m_columns != 1 ||
         writeBeatmapScreenshot(*scaled, scaledPath) !=
             ScreenshotResult::Saved )
        return 25;
    const auto compressed = readPng(scaledPath);
    const auto preview    = previewBeatmapScreenshot(*scaled);
    if ( !compressed.m_pixels || !preview ||
         compressed.m_width != scaledSize->m_width ||
         compressed.m_width != png.m_width ||
         compressed.m_height != scaledSize->m_height ||
         compressed.m_height != 430 || preview->m_width != compressed.m_width ||
         preview->m_height != compressed.m_height ||
         !isBright(compressed.pixel(123, 223)) )
        return 26;
    // 小成品使用完整图像大小，逐点比对可检测背景与分拍的细微差别。
    // 原尺寸的透明通道也应填满，避免将缩略图交给 ImGui 后背景透出。
    // 该对照不同于大图端点抽样，任何扫描行的遗漏都会被检出。
    if ( !matchesPreview(compressed,
                         *preview,
                         { 0, 0, compressed.m_width, compressed.m_height }) )
        return 27;
    chart.m_timelineZoomPercent = 100.0;

    // 两次导出使用同一时段，独立开关只应影响拍线所在的像素。
    // 第四轨的空白位置跨过整拍线，下一行提供没有网格的参考底色。
    // 再检查节点、长条及纵向轨道边界没有随拍线消失。
    chart.m_drawBeatLines   = false;
    const auto gridless     = prepareBeatmapScreenshot(chart, config);
    const auto gridlessPath = directory / "gridless.png";
    if ( !gridless || writeBeatmapScreenshot(*gridless, gridlessPath) !=
                          ScreenshotResult::Saved )
        return 17;
    const auto plain = readPng(gridlessPath);
    // 避开有物件的像素比较网格，避免把浅色音符误识别为分拍线。
    // 轨道边界单独取样，因为皮肤底板不归横向网格开关管理。
    if ( !plain.m_pixels || plain.m_width != png.m_width ||
         plain.m_height != png.m_height ||
         png.pixel(255, 313) == png.pixel(255, 315) ||
         plain.pixel(255, 313) != png.pixel(255, 315) ||
         !isBright(plain.pixel(123, 313)) || !isBright(plain.pixel(189, 550)) ||
         !isBright(plain.pixel(90, 220)) )
        return 18;
    // 后续整谱用例继续覆盖默认有分拍线的输出。
    chart.m_drawBeatLines = true;

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

    // 从零开始导整谱，默认 15 秒边界上的 Hold 身体应出现在左右两列。
    // 使用较短的末列验证它保持像素比例，而不是被拉到和整列同高。
    // 长图使用已导过区间的同一份谱面，之前排除的 Tap 理应恢复。
    // 新增的 Hold 头尾跨过两列，跨界两侧都需要保留主体。
    chart.m_begin = 0.0;
    chart.m_end   = 17.0;
    chart.m_notes.push_back(makeNote(NoteType::HOLD, 14.5, 1, 1.0));
    const auto whole = prepareBeatmapScreenshot(chart, config);
    // 从绝对时间推导标签和区域，结束列不能被补成另一个满时长列。
    // 文件实际写出后再读取，避免测试只验证内部布局元信息。
    const auto wholePath = directory / "whole.png";
    if ( !whole || whole->m_columns.size() != 2 ||
         whole->m_columns[0].m_label != "00:00.0000 - 00:15.0000" ||
         whole->m_columns[1].m_label != "00:15.0000 - 00:17.0000" ||
         whole->m_columns[1].m_height != 1026 ||
         writeBeatmapScreenshot(*whole, wholePath) != ScreenshotResult::Saved )
        return 11;
    // 实际文件应宽到能容纳两列，每列中物件不受前后水印状态影响。
    // 第一列顶端和第二列底端各检测长条，相反的裁切方向都能覆盖。
    const auto all = readPng(wholePath);
    if ( !all.m_pixels || all.m_width != 600 || all.m_height != 7330 ||
         !isBright(all.pixel(123, 100)) || !isBright(all.pixel(411, 1000)) ||
         !isBright(all.pixel(255, 6913)) )
        return 12;

    // 从两列之间读回成品，要求两侧蓝灰外框与中央留白各自可辨。
    // 同时取一条内部轨道线，避免在分列时一并改色。
    const auto frameColor = all.pixel(288, 100);
    const auto pageColor  = all.pixel(0, 100);
    if ( frameColor == pageColor || frameColor == all.pixel(90, 100) ||
         all.pixel(289, 100) != frameColor ||
         all.pixel(300, 100) != pageColor || all.pixel(310, 100) != frameColor )
        return 23;
    // 第二列实际高度为 1026 像素，上方有 40 像素时间栏。
    // 色值用侧边作参照，方便调整主题颜色时沿用同一几何断言。
    // 顶线与底线都跨越轨道，底线下应保留整图底色直到结束。
    if ( all.pixel(411, 38) != frameColor ||
         all.pixel(411, 1067) != frameColor ||
         all.pixel(411, 1069) != pageColor ||
         all.pixel(310, 1069) != pageColor )
        return 24;

    // 用同一谱面改成 7 秒，确认列数、时标和高度都以新参数为准。
    // 前三列分别覆盖 7 秒，最后一列只剩 1 秒，绘制比例保持一致。
    // 先独立检查非正时长，防止除零和意外生成空批次。
    if ( estimateBeatmapScreenshot(4, 0.0, 17.0, 0, 100.0, 0) ||
         estimateBeatmapScreenshot(4, 0.0, 17.0, -1, 100.0, 0) )
        return 19;
    // 拒绝非正、非有限及超出 PNG 像素预算的倍率。
    // 低于常用面板范围的正倍率依然可用，结束时间与分列保持独立。
    // 这里单独查尺寸边界，避免依赖极密网格在同一像素行上的叠加结果。
    const auto verySmall = estimateBeatmapScreenshot(4, 0.0, 17.0, 15, 0.05, 0);
    // 预期纵距 3.6 像素上取整到 4，另有 66 像素轨边和 64 像素页眉边距。
    if ( !verySmall || verySmall->m_columns != 2 || verySmall->m_height != 134 )
        return 33;
    for ( const double invalid :
          { 0.0,
            -25.0,
            1e8,
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN() } ) {
        if ( estimateBeatmapScreenshot(4, 0.0, 17.0, 15, invalid, 0) )
            return 28;
    }
    // 取与默认值不同且不能整除总时长的参数，覆盖短末列的布局。
    chart.m_end           = 22.0;
    chart.m_columnSeconds = 7;
    const auto custom     = prepareBeatmapScreenshot(chart, config);
    if ( !custom || custom->m_columns.size() != 4 ||
         custom->m_columns[0].m_label != "00:00.0000 - 00:07.0000" ||
         custom->m_columns[0].m_height != 3426 ||
         custom->m_columns[3].m_label != "00:21.0000 - 00:22.0000" ||
         custom->m_columns[3].m_height != 546 )
        return 20;
    // 回读自定义时长生成的 PNG，验证编码器使用新列数和新首列高度。
    // 短末列的空余区域只保留图像背景，不补出额外拍线或轨道。
    const auto customPath = directory / "custom-columns.png";
    if ( writeBeatmapScreenshot(*custom, customPath) !=
         ScreenshotResult::Saved )
        return 21;
    // 选择第四列一秒内容结束后的坐标，对比同一行的页边距。
    // 前三列沿用七秒的像素高度，整个图的宽高仍能独立读回。
    const auto customPng = readPng(customPath);
    if ( !customPng.m_pixels || customPng.m_width != 1176 ||
         customPng.m_height != 3490 ||
         customPng.pixel(900, 900) != customPng.pixel(0, 900) )
        return 22;

    // 放大后列的时间边界不动；末列和水印只增加各自的像素高度。
    // 同时开启两种署名，以便大图预览也经过字体布局与长列扫描。
    // 该倍率让整列超过缩略边长预算，能同时确认两个轴按相同比例抽样。
    chart.m_timelineZoomPercent = 137.5;
    chart.m_showMmm             = true;
    chart.m_author              = "谱师测试";
    const auto tallerSize =
        estimateBeatmapScreenshot(4, 0.0, 22.0, 7, 137.5, 2);
    const auto taller     = prepareBeatmapScreenshot(chart, config);
    const auto tallerPath = directory / "zoom-137.5-percent.png";
    if ( !tallerSize || !taller || tallerSize->m_columns != 4 ||
         taller->m_columns.back().m_label != custom->m_columns.back().m_label ||
         taller->m_columns.back().m_height != 726 ||
         writeBeatmapScreenshot(*taller, tallerPath) !=
             ScreenshotResult::Saved )
        return 29;
    const auto enlarged = readPng(tallerPath);
    // 用固定高度在估算和编码之间建立外部基准；同时验证宽度沿用分栏布局。
    if ( !enlarged.m_pixels || enlarged.m_width != customPng.m_width ||
         enlarged.m_width != tallerSize->m_width ||
         enlarged.m_height != tallerSize->m_height ||
         enlarged.m_height != 4810 )
        return 30;
    const auto thumbnail = previewBeatmapScreenshot(*taller);
    if ( !thumbnail || thumbnail->m_height >= enlarged.m_height ||
         thumbnail->m_width >= enlarged.m_width )
        return 31;
    // 按固定坐标逐点对照，补足单看尺寸无法发现的跨列裁剪和叠放错误。
    // 同时读取较短末列下方的成品底色，缩图不应把这一列拉伸到统一高度。
    if ( !matchesPreview(enlarged,
                         *thumbnail,
                         { 0, 0, enlarged.m_width, enlarged.m_height }) )
        return 32;

    // 模拟拖动到图中下部再放大，区域从非零行列开始且横跨列缝。
    // 跨首两列的长条应与原尺寸 PNG 一致，不能放大先前的整图缩略。
    // 起始行跳过了页眉与大量已结束的图元，采样仍须恢复准确的活动集合。
    // 从成品独立读回的像素同时覆盖真实皮肤、分拍线和底色。
    const ImageRegion crop{ 75, 3660, 510, 750 };
    // 用非整倍数偏移覆盖横向稀疏选点与 1:1 像素中心的共用路径。
    const auto detail = previewBeatmapScreenshotRegion(*taller, crop, 1024);
    if ( !detail || detail->m_width != crop.m_width ||
         detail->m_height != crop.m_height ||
         !matchesPreview(enlarged, *detail, crop) )
        return 36;
    // 靠近右下角的局部低清请求同样保留正确偏移，越界矩形则直接拒绝。
    // 边长预算只控制纹理大小，区域仍以成品坐标表达。
    // 此矩形右下边缘贴合成品边缘，增加一像素时必须失败。
    const ImageRegion bottomRight{ 860, 4500, 316, 310 };
    const auto        reduced =
        previewBeatmapScreenshotRegion(*taller, bottomRight, 150);
    if ( !reduced || reduced->m_width != 150 ||
         !matchesPreview(enlarged, *reduced, bottomRight) ||
         previewBeatmapScreenshotRegion(*taller, { 860, 4500, 317, 310 }, 150) )
        return 37;

    // 失败应在暂存阶段退出；已存在的 PNG 仍能读且原像素与尺寸一致。
    // 使用已成功的样本路径回测覆盖；缩略图失败同样没有理由生成或删改文件。
    chart.m_end = chart.m_begin;
    if ( prepareBeatmapScreenshot(chart, config) ) return 13;
    // 取消必须在分配待提交内容之前返回。
    // 复用已存在文件可以检测取消后错删目标的回归。
    std::stop_source stop;
    stop.request_stop();
    // 停止标记在处理开始前设置。编码和预览都应直接退出，后者不会返回部分像素。
    if ( previewBeatmapScreenshot(*prepared, stop.get_token()) ) return 34;
    if ( writeBeatmapScreenshot(*prepared, output, stop.get_token()) !=
         ScreenshotResult::Cancelled )
        return 14;
    prepared->m_asciiFont.clear();
    // ASCII 用于每列时标，即使简化图形也不能悄悄丢掉这些标签。
    if ( previewBeatmapScreenshot(*prepared) ) return 35;
    if ( writeBeatmapScreenshot(*prepared, output) !=
         ScreenshotResult::FontError )
        return 15;
    // 使用首次写出的像素作为参照，可检测误覆盖与文件截断。
    // 正常图片的尺寸、端点位置和非空 RGB 缺一不可。
    // 字体错误发生在提交前，所以后续可使用同一选择结果重试。
    const auto preserved = readPng(output);
    // 先前的输出保持可用；不能因为后台预览或后续编码失败而先清除成品。
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
        // 允许测试目录存在，让失败时保留先前成功生成的 PNG 供独立读回。
        MMM::Config::utf8ToPath(argv[3]) / "beatmap-screenshot";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return error ? 2 : run(directory);
}
