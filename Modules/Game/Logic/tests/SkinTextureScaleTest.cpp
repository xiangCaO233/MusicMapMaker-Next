#include "logic/ecs/system/render/SkinTextureScale.h"
#include "config/EditorConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/ecs/system/HitFXSystem.h"
#include "logic/ecs/system/render/Batcher.h"

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

/// @file
/// @brief 将皮肤资源键的倍率映射与最终 CPU 绘制几何一起纳入回归。
///
/// 合成夹具负责隔离缩放算法：所有坐标、图集区域、纹理比例都有确定值。
/// 真实皮肤负责验证接线：RM 两套特效应放大，默认与 IVM 应保持原状。
/// 不通过加载 PNG 来计算期望值，因此资源解码错误不属于本测试职责。
/// 不创建窗口、声卡或 Vulkan 设备，可以由无图形会话的 CTest 运行。
/// 纯色与字形的排除规则同样通过顶点断言验证，而不是仅检查配置字典。
/// 整个程序顺序加载皮肤；测试进程不与应用的皮肤管理器共享状态。
/// 所有输出只能写入第一个参数指定的构建目录，不修改输入皮肤资源。

namespace
{
using MMM::Config::BackgroundFillMode;
using MMM::Logic::RenderSnapshot;
using MMM::Logic::TextureID;
using MMM::Logic::System::Batcher;
using MMM::Logic::System::HitFXSystem;

/// @brief 比较由图元缩放产生的浮点坐标。
/// @param lhs 实际快照分量。
/// @param rhs 手算的期望分量。
/// @return 数值有限且差值小于测试容差时返回 true。
/// @note 容差只吸收单精度运算误差，不允许把重复缩放视为像素舍入。
bool near(float lhs, float rhs)
{
    // NaN 差值不会通过严格比较，测试不会把非法几何当成相等。
    return std::abs(lhs - rhs) < 0.0001F;
}

/// @brief 校验一个四边形的全部角点，保留顶点顺序以检测翻转。
/// @param snapshot 待校验的独立 CPU 快照。
/// @param offset 四边形在快照顶点数组中的起始位置。
/// @param expected 按左下、右下、右上、左上纹理顺序给出的目标坐标。
/// @return 任一角点不符、Z 不为零或顶点不足时返回 false。
/// @note 自由四边形不一定轴对齐，不能只比较包围盒宽高。
bool checkQuad(const RenderSnapshot& snapshot, std::size_t offset,
               const std::array<glm::vec2, 4>& expected)
{
    // 先检查边界，缺少几何不能靠默认坐标零值通过断言。
    if ( snapshot.vertices.size() < offset + expected.size() ) return false;
    for ( std::size_t index = 0; index < expected.size(); ++index ) {
        const auto& position = snapshot.vertices[offset + index].pos;
        // 每个顶点独立比较，检测仅移动了局部角点或生成了错误深度的情况。
        if ( !near(position.x, expected[index].x) ||
             !near(position.y, expected[index].y) || !near(position.z, 0) ) {
            XERROR("Texture scale vertex {} mismatch: {}, {} expected {}, {}",
                   index,
                   position.x,
                   position.y,
                   expected[index].x,
                   expected[index].y);
            return false;
        }
    }
    return true;
}

/// @brief 使用绘制入口的底边 Y 约定检查轴对齐矩形。
/// @param snapshot 生成完图元后的快照，不要求已经提交批次命令。
/// @param offset 允许同一快照连续检查多个图元，不重复清空数组。
/// @param x 缩放后矩形的左边界。
/// @param y 缩放后矩形的底边界。
/// @param width 缩放后完整宽度。
/// @param height 缩放后完整高度，向负 Y 方向展开。
/// @return 四角均匹配时返回 true。
/// @note 同时检查位置和尺寸，避免倍率正确却围绕错误锚点缩放。
/// @pre 测试期望宽高为正；负向图元由自由四边形场景单独表达。
bool checkRect(const RenderSnapshot& snapshot, std::size_t offset, float x,
               float y, float width, float height)
{
    return checkQuad(snapshot,
                     offset,
                     { glm::vec2(x, y),
                       glm::vec2(x + width, y),
                       glm::vec2(x + width, y - height),
                       glm::vec2(x, y - height) });
}

/// @brief 检查缩放后的纹理坐标仍覆盖原采样区域。
/// @param snapshot 待校验的快照。
/// @param offset 与几何断言使用同一图元起点。
/// @param minimum 最终图集 UV 的左上界。
/// @param maximum 最终图集 UV 的右下界。
/// @return 全部顶点保持原采样方向和边界时返回 true。
/// @note 缩放只改变几何，UV 被乘以倍率会造成图集串图或边缘裁切。
/// @note 此处传入最终图集坐标，不再使用纹理尺寸反推采样区域。
bool checkUv(const RenderSnapshot& snapshot, std::size_t offset,
             glm::vec2 minimum, glm::vec2 maximum)
{
    const std::array<glm::vec2, 4> expected{ glm::vec2(minimum.x, maximum.y),
                                             maximum,
                                             glm::vec2(maximum.x, minimum.y),
                                             minimum };
    // 调用者此前已断言几何存在；这里仍独立保护数组边界。
    if ( snapshot.vertices.size() < offset + expected.size() ) return false;
    for ( std::size_t index = 0; index < expected.size(); ++index ) {
        const auto& uv = snapshot.vertices[offset + index].uv;
        // 不能只看 UV 包围盒：翻转角点也会破坏显示，需要逐点匹配。
        if ( !near(uv.u, expected[index].x) ||
             !near(uv.v, expected[index].y) ) {
            XERROR("Texture scaling changed UV coordinates");
            return false;
        }
    }
    return true;
}

/// @brief 在构建输出目录写入独立皮肤，避免污染实际皮肤和测试资源。
/// @note 帧图片无需存在；加载器解析资源键，CPU 快照提供合成图集区域。
/// @param outputRoot CTest 传入的专用输出目录。
/// @param translationsRoot 仓库公共翻译目录，不读取个人设置。
/// @return 文件写入与皮肤加载均成功时返回 true。
/// @note 不删除先前输出；只覆盖本测试拥有的固定夹具文件。
/// @warning 文件访问只在测试初始化阶段执行，不是生产渲染热路径。
bool loadFixture(const std::filesystem::path& outputRoot,
                 const std::filesystem::path& translationsRoot)
{
    // 非抛出式目录创建让测试环境错误按失败返回，不继续使用旧夹具。
    std::error_code error;
    std::filesystem::create_directories(outputRoot, error);
    if ( error ) return false;
    const auto    path = outputRoot / "texture-scale-skin.lua";
    std::ofstream file(path);
    // 未开启流异常；打开失败和后续写入失败统一由流状态表达。
    // Note 与 Hold 故意引用相同文件，倍率仍必须按照资源键独立生效。
    // 连接体、普通物件、面板与动态序列使用不同值，暴露错误的共享状态。
    // 所有倍率都为有限正数；非法配置与重载清理由配置模块测试负责。
    file << "return { assets = { note = { effect = { "
            "note = 'shared/[1 .. 2].png', flick = 'flick/[1 .. 3].png', "
            "hold = 'shared/[1 .. 2].png' } } }, texture_scales = { "
            "['note.note'] = 0.5, ['note.node'] = 2, "
            "['note.holdbodyvertical'] = 1.6, "
            "['note.holdbodyhorizontal'] = 1.6, "
            "['logo'] = 1.2, ['panel.track.background'] = 1.3, "
            "['panel.track.judgearea'] = 0.7, "
            "['note.effect.note'] = 0.75, ['note.effect.flick'] = 1.25, "
            "['note.effect.hold'] = 1.5 } }";
    file.close();
    // 显式关闭后检查流状态，磁盘写入失败不应伪装为 Lua 解析错误。
    return file && MMM::Config::SkinManager::instance().loadSkin(
                       MMM::Config::pathToUtf8(path), translationsRoot);
}

/// @brief 验证同一图集切换逻辑纹理时，倍率更新不依赖重新切批。
/// @note 同时覆盖 pushQuad 到 FreeQuad 的转发及直接 UVQuad 入口。
/// @pre 已加载合成夹具，Note 倍率为 0.5、Node 倍率为 2。
/// @return 两张图元尺寸、锚点、UV 与单批次合并都正确时返回 true。
/// @note 命令中的索引计数用于确认批次完整，不需要提交到 GPU。
bool testAtlasBatching()
{
    RenderSnapshot snapshot;
    // 两个逻辑资源具有不同图集区域，但应共用一条绘制命令。
    snapshot.uvMap[static_cast<std::uint32_t>(TextureID::Note)] = {
        0.1F, 0.2F, 0.3F, 0.4F
    };
    snapshot.uvMap[static_cast<std::uint32_t>(TextureID::Node)] = {
        0.5F, 0.6F, 0.1F, 0.2F
    };
    Batcher batcher(&snapshot);
    batcher.setTexture(TextureID::Note);
    batcher.pushQuad(10, 80, 40, 20, glm::vec4(1));
    batcher.setTexture(TextureID::Node);
    // 前一图元已经入批，本次切换必须覆盖“不切批但更新倍率”的分支。
    batcher.pushUVQuad(
        100, 200, 10, 30, { 0.2F, 0.3F }, { 0.7F, 0.8F }, glm::vec4(1));
    batcher.flush();
    // 最后批次只在显式 flush 后可见，空命令数组不能视为合并成功。
    // 第一张缩小到一半，第二张放大两倍；中心位置均保持不变。
    // 颜色固定为白色，排除乘色、透明度和混合策略对几何断言的影响。
    // 检查命令数可防止通过强制切批掩盖逻辑纹理状态未更新的问题。
    constexpr float HALF_PIXEL = 0.5F / 2048.0F;
    // FreeQuad 沿用图集防串色收缩；显式 UVQuad 不再做二次图集换算。
    return snapshot.cmds.size() == 1 && snapshot.indices.size() == 12 &&
           checkRect(snapshot, 0, 20, 75, 20, 10) &&
           checkRect(snapshot, 4, 95, 215, 20, 60) &&
           checkUv(snapshot,
                   0,
                   { 0.1F + HALF_PIXEL, 0.2F + HALF_PIXEL },
                   { 0.4F - HALF_PIXEL, 0.6F - HALF_PIXEL }) &&
           checkUv(snapshot, 4, { 0.2F, 0.3F }, { 0.7F, 0.8F });
}

/// @brief 验证普通自由四边形围绕四角平均中心缩放。
/// @note 使用斜四边形，防止实现只处理轴对齐矩形。
/// @pre 已加载合成夹具，Node 倍率为 2。
/// @return 斜边方向、四角中心和完整 UV 均保持时返回 true。
bool testFreeQuad()
{
    RenderSnapshot snapshot;
    Batcher        batcher(&snapshot);
    batcher.setTexture(TextureID::Node);
    batcher.pushFreeQuad(
        { 10, 70 }, { 50, 70 }, { 70, 10 }, { 30, 10 }, glm::vec4(1));
    // 原中心为 (40, 40)，每个角点相对它放大两倍；纹理方向不变。
    // 不提供图集条目，覆盖独立纹理默认 0～1 UV 的路径。
    return checkQuad(snapshot,
                     0,
                     { glm::vec2(-20, 100),
                       glm::vec2(60, 100),
                       glm::vec2(100, -20),
                       glm::vec2(20, -20) }) &&
           checkUv(snapshot, 0, { 0, 0 }, { 1, 1 });
}

/// @brief 验证填充策略先决定采样区域，几何倍率只应用一次。
/// @note Center 输入真实像素大小；AspectFill 检查裁剪 UV 保持不变。
/// @pre 已加载合成夹具，Note 倍率为 0.5。
/// @return 四种填充模式和缺失比例的兼容路径全部正确时返回 true。
/// @note 图集条目不是整张图，能区分局部裁剪和最终图集坐标。
bool testFillModes()
{
    for ( const auto mode : { BackgroundFillMode::Stretch,
                              BackgroundFillMode::AspectFit,
                              BackgroundFillMode::AspectFill,
                              BackgroundFillMode::Center } ) {
        RenderSnapshot snapshot;
        snapshot.uvMap[static_cast<std::uint32_t>(TextureID::Note)] = {
            0.1F, 0.2F, 0.4F, 0.6F
        };
        Batcher batcher(&snapshot);
        batcher.setTexture(TextureID::Note);
        batcher.pushFilledQuad(10, 90, 80, 40, { 20, 20 }, mode, glm::vec4(1));
        // 目标框中心为 (50, 70)，各填充模式不得改变这个中心。
        // 每种模式从新快照开始，防止上一种模式残留的顶点冒充本次输出。
        // 宽高按手算结果断言，不调用待测辅助函数计算“期望值”。
        const float width  = mode == BackgroundFillMode::Center      ? 10.0F
                             : mode == BackgroundFillMode::AspectFit ? 20.0F
                                                                     : 40.0F;
        const float height = mode == BackgroundFillMode::Center ? 10.0F : 20.0F;
        const bool  cropped = mode == BackgroundFillMode::AspectFill;
        // 正方形纹理填满二比一目标框时，应截取垂直方向中间一半。
        // 局部 0.25～0.75 映射到图集后为 0.35～0.65，不受倍率影响。
        if ( !checkRect(
                 snapshot, 0, 50 - width / 2, 70 + height / 2, width, height) ||
             !checkUv(snapshot,
                      0,
                      { 0.1F, cropped ? 0.35F : 0.2F },
                      { 0.5F, cropped ? 0.65F : 0.8F }) )
            return false;
    }
    // 缺少纹理比例时转入普通矩形，也不能漏掉倍率或重复应用。
    // 此分支绕过正常填充计算，单独检查能发现转发入口遗漏的缩放。
    RenderSnapshot snapshot;
    Batcher        batcher(&snapshot);
    batcher.setTexture(TextureID::Note);
    batcher.pushFilledQuad(
        10, 90, 80, 40, { 0, 0 }, BackgroundFillMode::AspectFit, glm::vec4(1));
    return checkRect(snapshot, 0, 30, 80, 40, 20);
}

/// @brief 验证长条与折线连接体只放大截面，不延长连接跨度。
/// @note 保留各端边中点，使时间端点和轨道端点仍贴合头尾物件。
/// @pre 两种连接体键的倍率都为 1.6。
/// @return 斜向与轴对齐连接体都保持跨度时返回 true。
/// @note 这里检查图元几何，不修改物件时间、轨道或 ECS 拾取数据。
/// @note 偏移相同端边的两个角点，而非缩放整个连接体的轴对齐包围框。
bool testConnectionBodies()
{
    RenderSnapshot snapshot;
    Batcher        batcher(&snapshot);
    // 竖向连接体可以倾斜，分别围绕上下端边中点放大横截面。
    // 两端中点为 (25, 100) 和 (55, 20)，不允许向外延伸时间轴。
    batcher.setTexture(TextureID::HoldBodyVertical);
    batcher.pushFreeQuad(
        { 20, 100 }, { 30, 100 }, { 60, 20 }, { 50, 20 }, glm::vec4(1));
    if ( !checkQuad(snapshot,
                    0,
                    { glm::vec2(17, 100),
                      glm::vec2(33, 100),
                      glm::vec2(63, 20),
                      glm::vec2(47, 20) }) )
        return false;
    // 横向连接体的左右端点保持原位，不能把跨轨长度也乘以 1.6。
    // 左右端中点为 (20, 65) 和 (100, 35)，分别放大各自边界偏移。
    batcher.setTexture(TextureID::HoldBodyHorizontal);
    batcher.pushFreeQuad(
        { 20, 70 }, { 100, 40 }, { 100, 30 }, { 20, 60 }, glm::vec4(1));
    if ( !checkQuad(snapshot,
                    4,
                    { glm::vec2(20, 73),
                      glm::vec2(100, 43),
                      glm::vec2(100, 27),
                      glm::vec2(20, 57) }) )
        return false;
    // 轴对齐 UV 入口和填充转发入口必须使用同样的连接体规则。
    // 选择不同入口能识别只修复 FreeQuad、遗漏矩形通道的实现。
    batcher.setTexture(TextureID::HoldBodyVertical);
    batcher.pushUVQuad(10, 100, 20, 80, { 0, 0 }, { 1, 1 }, glm::vec4(1));
    batcher.setTexture(TextureID::HoldBodyHorizontal);
    batcher.pushFilledQuad(
        10, 100, 80, 20, { 1, 1 }, BackgroundFillMode::Stretch, glm::vec4(1));
    return checkRect(snapshot, 8, 4, 100, 32, 80) &&
           checkRect(snapshot, 12, 10, 106, 80, 32);
}

/// @brief 检查固定资源键与非皮肤资源的映射边界。
/// @note 纯色、项目背景、字形、自定义纹理都不应继承任意皮肤倍率。
/// @pre 夹具为 Logo、轨道背景和判定区配置了互不相同的倍率。
/// @return 已配置键按倍率绘制，排除键完全保持原始尺寸时返回 true。
/// @note 项目背景属于谱面资源，不等同于皮肤轨道背景。
/// @note 字形高位区用于编辑标注，不能让用户放大 Note 后文字一起变大。
bool testResourceMapping()
{
    for ( const auto [texture, scale] :
          { std::pair{ TextureID::Logo, 1.2F },
            std::pair{ TextureID::Track, 1.3F },
            std::pair{ TextureID::JudgeArea, 0.7F } } ) {
        RenderSnapshot snapshot;
        Batcher        batcher(&snapshot);
        batcher.setTexture(texture);
        batcher.pushQuad(10, 80, 40, 20, glm::vec4(1));
        // 每一固定键实际生成几何，不仅比较映射辅助函数的返回值。
        // 在非原点矩形上检查中心，避免全局原点缩放错误偶然通过。
        if ( !checkRect(snapshot,
                        0,
                        30 - 20 * scale,
                        70 + 10 * scale,
                        40 * scale,
                        20 * scale) )
            return false;
    }
    for ( const auto texture : { TextureID::None,
                                 TextureID::Background,
                                 TextureID::NoteSelectionBorder,
                                 TextureID::AsciiGlyphStart,
                                 TextureID::UnicodeGlyphStart,
                                 static_cast<TextureID>(999999),
                                 static_cast<TextureID>(0x30000000U) } ) {
        RenderSnapshot snapshot;
        Batcher        batcher(&snapshot);
        batcher.setTexture(texture);
        batcher.pushQuad(10, 80, 40, 20, glm::vec4(1));
        // 未登记的动态编号同样退回单位倍率，不能访问不存在的帧缓存。
        // 999999 位于特效编号段内但未登记，另一自定义值位于高位区。
        // 两者必须安全回退，不能按编号跨度误读已配置的皮肤倍率。
        if ( !near(MMM::Logic::System::skinTextureScale(texture), 1) ||
             !checkRect(snapshot, 0, 10, 80, 40, 20) )
            return false;
    }
    return true;
}

/// @brief 验证所有动画帧沿用所属资源键的独立倍率。
/// @note 三类序列连续切换，同图片路径的 Hold/Note 也必须独立。
/// @pre 合成夹具为三个序列配置了互不相同的倍率。
/// @return 每一帧的实际图元都匹配所属序列倍率时返回 true。
/// @note 不能只测首帧；缓存展开漏掉尾帧会在循环播放时造成尺寸跳变。
/// @note 不断言分配后的具体数字 ID，只要求每个当前有效帧得到正确倍率。
bool testSequenceFrames()
{
    const auto& skin = MMM::Config::SkinManager::instance();
    for ( const auto& [key, scale] :
          { std::pair{ "note.effect.note", 0.75F },
            std::pair{ "note.effect.flick", 1.25F },
            std::pair{ "note.effect.hold", 1.5F } } ) {
        const auto* sequence = skin.getEffectSequence(key);
        // 缺少序列会使循环没有断言，因此必须主动视为失败。
        if ( !sequence || sequence->frames.empty() ) return false;
        for ( std::size_t frame = 0; frame < sequence->frames.size();
              ++frame ) {
            RenderSnapshot snapshot;
            Batcher        batcher(&snapshot);
            batcher.setTexture(static_cast<TextureID>(
                sequence->startId + static_cast<std::uint32_t>(frame)));
            batcher.pushQuad(10, 80, 40, 20, glm::vec4(1));
            // 检查最终顶点，覆盖帧 ID 到倍率缓存再到批处理器的完整路径。
            // startId 属于当前皮肤，不假定无序字典中的插入顺序。
            // 每帧重新设置纹理，使帧缓存路径确实参与本次绘制。
            if ( !checkRect(snapshot,
                            0,
                            30 - 20 * scale,
                            70 + 10 * scale,
                            40 * scale,
                            20 * scale) )
                return false;
        }
    }
    return true;
}

/// @brief 使用真实内置皮肤验证三类打击特效的最终快照尺寸。
/// @note 只消费 CPU 几何，不上传图片，不声称完成 GPU 像素验收。
/// @param skinsRoot 内置皮肤目录，必须包含全部四套受测皮肤。
/// @param translationsRoot 公共翻译目录。
/// @return 三类特效在四套皮肤的两个时刻均符合预期时返回 true。
/// @note 重载会重排动态 ID，序列指针只在当前皮肤作用域内使用。
/// @note 期望值由原布局尺寸乘明确倍率得到，不再读同一个配置值。
/// @warning 测试初始化允许同步解析 Lua，但不启动正式播放循环。
bool testBuiltInHitEffects(const std::filesystem::path& skinsRoot,
                           const std::filesystem::path& translationsRoot)
{
    auto& skin = MMM::Config::SkinManager::instance();
    for ( const std::string name : { "mmm-default", "ivm", "rm", "rm-old" } ) {
        if ( !skin.loadSkin(
                 MMM::Config::pathToUtf8(skinsRoot / name / "skin.lua"),
                 translationsRoot) )
            return false;
        const float scale = name == "rm" || name == "rm-old" ? 1.6F : 1.0F;
        // 使用固定期望值，不能把错误的皮肤配置同时当成测试正确答案。
        // IVM 使用整轨拉伸；其他三套使用判定线居中的固定框。
        const bool trackFill = name == "ivm";
        for ( const auto [type, key] :
              { std::pair{ MMM::NoteType::NOTE, "note.effect.note" },
                std::pair{ MMM::NoteType::FLICK, "note.effect.flick" },
                std::pair{ MMM::NoteType::HOLD, "note.effect.hold" } } ) {
            const auto* sequence = skin.getEffectSequence(key);
            if ( !sequence || sequence->frames.empty() ) return false;
            MMM::Config::EditorConfig config;
            // 显式固定填充方式，隔离个人配置和其他显示开关的影响。
            config.visual.enableHitEffects = true;
            config.visual.noteScaleX       = 1.25F;
            config.visual.noteScaleY       = 0.75F;
            config.visual.noteFillMode     = BackgroundFillMode::Stretch;
            // 使用非默认物件倍率，确保皮肤倍率是叠乘，而非覆盖现有配置。
            HitFXSystem::HitEvent event{};
            // 值初始化保留零起始时间、非草稿和独立物件角色。
            // 不绑定采样或调用 triggerAudio，测试不应初始化音频设备。
            event.type        = type;
            event.trackSpan   = 1;
            event.trackIndex  = 1;
            event.trackOffset = type == MMM::NoteType::FLICK ? 1 : 0;
            event.duration    = 2;
            HitFXSystem system;
            system.update(0, { event }, 4, config);
            // 每类物件单独触发，避免同轨覆盖规则混入前一类特效。
            // 跨首帧验证缓存覆盖后续动画帧；两时刻均处于普通特效寿命内。
            for ( const double time : { 0.0, 0.041 } ) {
                const auto frame = HitFXSystem::loopingEffectFrameIndex(
                    time, skin.getEffectBaseFps(), sequence->frames.size());
                if ( !frame ) return false;
                RenderSnapshot snapshot;
                snapshot.uvMap[sequence->startId +
                               static_cast<std::uint32_t>(*frame)] = {
                    0.2F, 0.3F, 0.1F, 0.2F
                };
                Batcher batcher(&snapshot);
                system.generateSnapshot(
                    batcher, time, config, 4, 250, 40, 20, 620, 100);
                // 合成图集帧宽高比为 1/2，固定框缩放前为 125×150。
                // 只登记当前期望帧，误选另一特效会直接缺少几何。
                // UV 给出的比例固定，不依赖 PNG 解码或显示器 DPI。
                // Flick 的终点位于第三轨，其他类型仍位于第二轨。
                // 判定线位于 250，而整轨纵向中心为 320，故能识别布局混用。
                const float width   = (trackFill ? 100 : 125) * scale;
                const float height  = (trackFill ? 600 : 150) * scale;
                const float centerX = type == MMM::NoteType::FLICK ? 290 : 190;
                const float centerY = trackFill ? 320 : 250;
                // 继续检查 UV，排除把裁掉中心图像错误实现为视觉放大。
                if ( snapshot.vertices.size() != 4 ||
                     !checkRect(snapshot,
                                0,
                                centerX - width / 2,
                                centerY + height / 2,
                                width,
                                height) ||
                     !checkUv(snapshot, 0, { 0.2F, 0.3F }, { 0.3F, 0.5F }) ) {
                    XERROR(
                        "Built-in hit effect scale mismatch: {} {}", name, key);
                    return false;
                }
            }
        }
    }
    return true;
}
}  // namespace

/// @brief 执行不依赖图形设备的皮肤纹理缩放回归。
/// @param argc 程序名之外必须恰好传入三个路径。
/// @param argv 输出目录、公共翻译目录、内置皮肤根目录。
/// @note 先用夹具隔离各几何入口，再加载真实皮肤验证功能接线。
/// @return 初始化或几何断言失败时返回非零状态，供 CTest 识别。
/// @note 首个错误立即终止，保留日志和夹具用于定位，不覆盖用户数据。
/// @note 资源根按参数传入，工作目录变化不会改变受测皮肤的来源。
int main(int argc, char* argv[])
{
    if ( argc != 4 ) {
        XERROR(
            "SkinTextureScaleTest requires output, translations and skins "
            "paths");
        return 1;
    }
    if ( !loadFixture(argv[1], argv[2]) || !testAtlasBatching() ||
         !testFreeQuad() || !testFillModes() || !testConnectionBodies() ||
         !testResourceMapping() || !testSequenceFrames() ||
         !testBuiltInHitEffects(argv[3], argv[2]) )
        return 1;
    XINFO("Skin texture scale geometry tests passed");
    return 0;
}
