#pragma once

#include "ui/IUIView.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vulkan/vulkan.hpp>

namespace MMM::Graphic
{
class VKTexture;
}

namespace MMM::UI
{

class ITextureLoader : virtual public IUIView
{
public:
    /// @brief 构造具备纹理资源加载能力的 UI 视图。
    /// @param name 传给虚基类 IUIView 的稳定视图名称。
    ITextureLoader(const std::string& name) : IUIView(name) {};
    /// @brief 允许移动构造，保持视图资源由派生类决定转移方式。
    ITextureLoader(ITextureLoader&&) = default;
    /// @brief 允许复制构造接口能力，具体资源仍由派生类管理。
    ITextureLoader(const ITextureLoader&) = default;
    /// @brief 禁止移动赋值，避免替换已注册视图的稳定身份。
    ITextureLoader& operator=(ITextureLoader&&) = delete;
    /// @brief 禁止复制赋值，避免覆盖虚基类持有的布局上下文。
    ITextureLoader& operator=(const ITextureLoader&) = delete;

    /// @brief 通过虚析构释放派生类纹理资源。
    virtual ~ITextureLoader() override = default;

    /// @brief 获取视图具体类型,替代 dynamic_cast
    ViewType getViewType() const override { return ViewType::TextureLoader; }

    /// @brief 安全转换为自身
    ITextureLoader* asTextureLoader() override { return this; }

    /// @brief 返回实际纹理加载器实例，供禁用 RTTI 的能力路由使用。
    void* getActualInstance() override { return this; }

    /// @brief 是否需要重载
    /// @return 纹理缓存存在待处理变更时返回 true。
    virtual bool needReload() = 0;

    /// @brief 重载纹理
    /// @param physicalDevice Vulkan 物理设备。
    /// @param logicalDevice Vulkan 逻辑设备。
    /// @param cmdPool 纹理上传使用的命令池。
    /// @param queue 执行上传命令的队列。
    /// @warning 资源准备路径：派生类只能在渲染器提供的安全时机创建或销毁 GPU
    /// 资源。
    virtual void reloadTextures(vk::PhysicalDevice& physicalDevice,
                                vk::Device&         logicalDevice,
                                vk::CommandPool& cmdPool, vk::Queue& queue) = 0;

protected:
    /**
     * @brief 公用接口：从文件路径加载纹理（自动识别 SVG 或位图）
     * @param path 文件路径
     * @param targetSize 如果是 SVG，栅格化的目标尺寸
     * @param overrideColor 可选：如果提供，SVG
     * 的所有非透明像素将被替换为此颜色 (RGB 范围 0.0~1.0)
     * @return 成功时返回 Vulkan 纹理所有权，路径或 SVG 解析失败时返回空。
     * @warning 低频资源路径：会访问文件系统、解析 SVG 并同步创建 GPU 纹理。
     */
    std::unique_ptr<Graphic::VKTexture> loadTextureResource(
        const std::filesystem::path& path, uint32_t targetSize,
        vk::PhysicalDevice& physicalDevice, vk::Device& logicalDevice,
        vk::CommandPool& commandPool, vk::Queue& queue,
        std::optional<std::array<float, 4>> overrideColor = std::nullopt);
};

}  // namespace MMM::UI
