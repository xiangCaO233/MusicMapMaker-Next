#pragma once

#include "ui/IRenderableView.h"

namespace MMM::Canvas
{
class Basic2DCanvas : public Graphic::UI::IRenderableView
{
public:
    Basic2DCanvas(const std::string& name, uint32_t w, uint32_t h);
    Basic2DCanvas(Basic2DCanvas&&)                 = delete;
    Basic2DCanvas(const Basic2DCanvas&)            = delete;
    Basic2DCanvas& operator=(Basic2DCanvas&&)      = delete;
    Basic2DCanvas& operator=(const Basic2DCanvas&) = delete;
    ~Basic2DCanvas()                               = default;

    // 接口实现
    void update() override;

    ///@brief 是否需要重新记录命令 (比如数据变了)
    bool isDirty() const override;

    /**
     * @brief 获取 Shader 源码接口 (在固定时刻需要创建)
     */
    std::vector<std::string> getShaderSources(
        const std::string& shader_name) override;

    /**
     * @brief 获取 Shader 名称(需要按唯一名称名称储存和销毁)
     */
    std::string getShaderName(const std::string& shader_module_name) override;

private:
    /// @brief 画布名称
    std::string m_canvasName;

    /// @brief 缓存spv源码，避免重复读盘
    std::unordered_map<std::string, std::vector<std::string>>
        m_shaderSourceCache;
};

}  // namespace MMM::Canvas
