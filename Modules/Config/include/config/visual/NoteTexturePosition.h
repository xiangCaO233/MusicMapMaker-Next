#pragma once

namespace MMM::Config
{
/// @brief Note 固定贴图对齐位置，只描述视觉定位，不改变拍位或轨道。
/// @note 默认中心保证旧配置升级后不改变画布上的音符位置。
/// @note 统一作用于单键、长条头尾、滑键箭头、横向连接体、折线节点和判定区。
/// @note 竖向连接体仍连接原有时轨端点，其他图像不参与此布局设置。
enum class NoteTexturePosition {
    Center,  ///< 贴图中心对齐物件的时间位置，兼容旧布局。
    Bottom   ///< 贴图底边中点对齐物件的时间位置，图像上移自身半高。
};
}  // namespace MMM::Config
