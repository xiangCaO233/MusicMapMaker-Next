#include "logic/ecs/system/render/SkinTextureScale.h"

#include "common/render/RenderSnapshot.h"
#include "config/skin/SkinConfig.h"
#include "config/visual/BackgroundConfig.h"

#include <algorithm>
#include <array>
#include <string>

namespace MMM::Logic::System
{
/// @brief 查询当前皮肤的纹理倍率，资源键与路径和图集 UV 相互独立。
/// @param texture 调用方已经选定的逻辑纹理，而非 Vulkan 或图集描述符。
/// @return 未配置的纹理返回单位倍率，不继承相邻或同源资源的设置。
/// @note 同一图片可绑定多个资源键，每个键的视觉倍率互不影响。
/// @note HoldHead 回退由渲染器选用 Note ID 完成，此处不另做资源回退。
/// @note 项目背景和字体保持原布局，不能随皮肤纹理倍率整体缩放。
/// @note 不缓存配置值，以便皮肤切换后立即读取新表而不保留旧倍率。
/// @warning 快照热路径仅索引固定键表和配置缓存，不枚举序列帧。
float skinTextureScale(Common::Render::TextureID texture)
{
    using Common::Render::TextureID;
    // 静态键只初始化一次，避免长资源键在每个图元处构造临时字符串。
    // 索引直接对应稳定 TextureID；空槽刻意排除纯色和项目背景。
    // 增加静态 TextureID 时需同步此表，不能按资源名称重新排序。
    // 这里保留逻辑键而不是文件路径，以区分共享图片的不同组件配置。
    static const std::array<std::string,
                            static_cast<std::size_t>(TextureID::HoldHead) + 1>
                KEYS{ "",
                      "",
                      "note.note",
                      "note.node",
                      "note.holdbodyvertical",
                      "note.holdbodyhorizontal",
                      "note.holdend",
                      "note.arrowleft",
                      "note.arrowright",
                      "panel.track.background",
                      "panel.track.judgearea",
                      "logo",
                      "note.holdhead" };
    const auto  id   = static_cast<std::uint32_t>(texture);
    const auto& skin = Config::SkinManager::instance();
    if ( id < KEYS.size() ) {
        // 缺失声明由 SkinManager 回退为 1，不从其他皮肤或组件猜测倍率。
        return KEYS[id].empty() ? 1.0F : skin.getTextureScale(KEYS[id]);
    }
    // 字形、选框与自定义非皮肤资源不继承任何 Note 或特效倍率。
    // 动态序列的 ID 已在皮肤加载阶段逐帧登记，此处只做常数时间查询。
    if ( id >= static_cast<std::uint32_t>(TextureID::EffectStart) &&
         id < static_cast<std::uint32_t>(TextureID::AsciiGlyphStart) ) {
        return skin.getEffectTextureScale(id);
    }
    // 非皮肤 ID 必须原样绘制，尤其不能放大文字、拍线或交互辅助图形。
    return 1.0F;
}
/// @brief 为 Note 的点状部件选择统一的固定锚点，不复用皮肤资产参数。
/// @param texture 当前点状部件的逻辑纹理 ID。
/// @param position 布局设置选择的中心或底边位置。
/// @return 乘最终显示高度的位移比例，负值沿画布上方向移动。
/// @note 是否进入 Note 绘制范围由批处理器的作用域状态控制。
/// @warning 热路径只比较枚举；未知纹理、辅助标记及竖向连接体返回零位移。
float noteTextureVerticalOffset(Common::Render::TextureID   texture,
                                Config::NoteTexturePosition position)
{
    using Common::Render::TextureID;
    if ( position != Config::NoteTexturePosition::Bottom ) return 0.0F;
    // 动态帧只在打击动画入口开启位置作用域，其他资源不继承该偏移。
    const auto id = static_cast<std::uint32_t>(texture);
    if ( id >= static_cast<std::uint32_t>(TextureID::EffectStart) &&
         id < static_cast<std::uint32_t>(TextureID::AsciiGlyphStart) )
        return -0.5F;
    // 连接体保留时轨跨度，其端点由可见头尾的视觉中心单独决定。
    switch ( texture ) {
    case TextureID::Note:
    case TextureID::HoldHead:
    case TextureID::HoldEnd:
    case TextureID::Node:
    case TextureID::FlickArrowLeft:
    case TextureID::FlickArrowRight:
    // 判定区由专用绘制入口启用作用域，不改变其他轨道图像的位置。
    case TextureID::JudgeArea: return -0.5F;
    default: return 0.0F;
    }
}
/// @brief 在底边模式下取得点贴图最终视觉中心的纵向偏移。
/// @note 输入尺寸已含独立皮肤倍率，不再次乘倍率或改变时间锚点。
/// @warning 每个连接点与拾取部件调用一次，仅使用常量时间运算。
float noteTextureCenterShiftY(Common::Render::TextureID texture, float width,
                              float height, float aspect,
                              Config::BackgroundFillMode  fillMode,
                              Config::NoteTexturePosition position)
{
    const float offset = noteTextureVerticalOffset(texture, position);
    if ( offset == 0.0F ) return 0.0F;
    // Fit 的留白不是实际图像，连接点必须落在收缩后的可见矩形中心。
    if ( fillMode == Config::BackgroundFillMode::AspectFit && aspect > 0.0F )
        height = std::min(height, width / aspect);
    // Center、Stretch 和 Fill 的纵向框保持既有布局尺寸。
    return height * offset;
}
}  // namespace MMM::Logic::System
