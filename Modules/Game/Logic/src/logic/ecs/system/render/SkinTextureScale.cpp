#include "logic/ecs/system/render/SkinTextureScale.h"

#include "common/render/RenderSnapshot.h"
#include "config/skin/SkinConfig.h"

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
}  // namespace MMM::Logic::System
