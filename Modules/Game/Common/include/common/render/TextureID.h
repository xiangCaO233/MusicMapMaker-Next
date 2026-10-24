#pragma once

#include <cstdint>

namespace MMM::Common::Render
{
/// @brief 跨逻辑与渲染线程共享的稳定纹理语义 ID。
/// @note 枚举是图集查找键，不是 Vulkan 或 ImGui 后端纹理句柄。
/// 不随图集重建分配新语义编号，生产与消费两端必须使用相同取值。
/// @note 数值分区避免内置纹理、动态特效和字体图集互相冲突。
enum class TextureID : uint32_t {
    /// @brief 无纹理哨兵，非法字形编号也返回此值。
    None = 0,
    /// @brief 内置纹理由固定低位编号索引，顺序不可因头文件拆分改变。
    Background = 1,
    Note       = 2,
    Node       = 3,
    HoldBodyVertical,
    HoldBodyHorizontal,
    HoldEnd,
    FlickArrowLeft,
    FlickArrowRight,
    Track,
    JudgeArea,
    Logo,

    /// @brief 独立长条头纹理；未提供的皮肤继续使用 Note。
    HoldHead,

    /// @brief 选中描边使用独立编号，避免与基础物件材质混用。
    NoteSelectionBorder = 100,

    /// @brief 动态特效序列帧的起始编号，与皮肤加载器分配约定一致。
    EffectStart = 1000,

    /// @brief ASCII 字形使用独立高位保留区，避免与皮肤动态特效 ID 冲突。
    AsciiGlyphStart = 0x00100000U,

    /// @brief 按需 Unicode 字形使用独立高位保留区。
    UnicodeGlyphStart = 0x00200000U
};

}  // namespace MMM::Common::Render
