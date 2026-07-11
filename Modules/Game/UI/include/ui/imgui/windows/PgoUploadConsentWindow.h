#pragma once

namespace MMM::UI
{

/// @brief 管理首次启动时的 PGO 性能数据上传授权窗口。
class PgoUploadConsentWindow final
{
public:
    /// @brief 在尚未记录授权选择时渲染 PGO 上传授权窗口。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：每帧执行；仅在 PGO 插桩构建且尚未授权时绘制。
    void render(float dpiScale) const;
};

}  // namespace MMM::UI
