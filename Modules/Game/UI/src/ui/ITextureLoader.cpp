#include "ui/ITextureLoader.h"

#include "graphic/imguivk/VKTexture.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <cctype>
#include <lunasvg.h>
#include <system_error>

namespace MMM::UI
{

/// @brief 从 SVG 或位图文件创建 Vulkan 纹理。
/// @param path 纹理文件路径。
/// @param targetSize SVG 栅格化的正方形边长。
/// @param physicalDevice Vulkan 物理设备。
/// @param logicalDevice Vulkan 逻辑设备。
/// @param commandPool 上传命令池。
/// @param queue 执行上传的 Vulkan 队列。
/// @param overrideColor SVG 非透明像素的可选替换颜色。
/// @return 成功时返回独占纹理；路径或 SVG 解析失败时返回空。
/// @warning 低频资源路径：会访问文件、解析 SVG 并同步创建 GPU 资源。
/// @details 加载策略分为两条路径：
/// - SVG 先按目标正方形尺寸栅格化为 RGBA；
/// - 可选主题色只替换 RGB，保留矢量图原始透明度；
/// - 其他扩展名直接交给 VKTexture 的文件构造器；
/// - 扩展名比较不区分大小写；
/// - 所有失败都通过空指针表达，不抛出异常；
/// - 成功结果由调用方独占并管理 GPU 生命周期。
std::unique_ptr<Graphic::VKTexture> ITextureLoader::loadTextureResource(
    const std::filesystem::path& path, uint32_t targetSize,
    vk::PhysicalDevice& physicalDevice, vk::Device& logicalDevice,
    vk::CommandPool& commandPool, vk::Queue& queue,
    std::optional<std::array<float, 4>> overrideColor)
{
    // 使用 error_code 避免文件系统异常穿过项目的无异常边界。
    std::error_code texturePathError;
    if ( !std::filesystem::exists(path, texturePathError) ||
         texturePathError ) {
        // 日志路径显式转换为 UTF-8，保证非 ASCII 资源名可诊断。
        const auto u8Path = path.u8string();
        XWARN("Texture path not found: {}",
              std::string(reinterpret_cast<const char*>(u8Path.c_str()),
                          u8Path.size()));
        return nullptr;
    }

    // 扩展名统一转换为小写，以兼容 Windows 项目中的大写后缀。
    const auto  extension = path.extension().u8string();
    std::string extensionString(
        reinterpret_cast<const char*>(extension.c_str()), extension.size());
    std::transform(extensionString.begin(),
                   extensionString.end(),
                   extensionString.begin(),
                   [](unsigned char character) {
                       // 先转 unsigned char，避免负 char 传给 tolower
                       // 的未定义行为。
                       return static_cast<char>(std::tolower(character));
                   });

    if ( extensionString == ".svg" ) {
        // lunasvg 接口接收 UTF-8 字符串而非 std::filesystem::path。
        const auto  u8Path = path.u8string();
        std::string utf8Path(reinterpret_cast<const char*>(u8Path.c_str()),
                             u8Path.size());
        auto        document = lunasvg::Document::loadFromFile(utf8Path);
        if ( !document ) {
            // 解析失败不构造空纹理，由调用方保留旧资源或使用兜底。
            XWARN("lunasvg failed to load: {}", utf8Path);
            return nullptr;
        }

        // SVG 按调用方目标尺寸栅格化，并转换为 VKTexture 期望的 RGBA 顺序。
        auto bitmap = document->renderToBitmap(targetSize, targetSize);
        bitmap.convertToRGBA();

        if ( overrideColor.has_value() ) {
            // 替色保留原始 alpha，只统一非透明图形的 RGB 主题色。
            // 完全透明像素即使 RGB 被改写也不会改变最终合成结果。
            uint8_t*    pixels = bitmap.data();
            const auto& color  = overrideColor.value();

            // 浮点颜色通道先映射到八位范围并钳制，防止越界转换。
            const uint8_t targetRed = static_cast<uint8_t>(
                std::clamp(color[0] * 255.0f, 0.0f, 255.0f));
            const uint8_t targetGreen = static_cast<uint8_t>(
                std::clamp(color[1] * 255.0f, 0.0f, 255.0f));
            const uint8_t targetBlue = static_cast<uint8_t>(
                std::clamp(color[2] * 255.0f, 0.0f, 255.0f));

            for ( uint32_t index = 0; index < targetSize * targetSize;
                  ++index ) {
                // RGBA 每像素四字节，alpha 通道位于 index * 4 + 3 且保持不变。
                pixels[index * 4 + 0] = targetRed;
                pixels[index * 4 + 1] = targetGreen;
                pixels[index * 4 + 2] = targetBlue;
            }
        }

        // VKTexture 在构造期间完成像素上传，局部 bitmap 随后可安全释放。
        return std::make_unique<Graphic::VKTexture>(bitmap.data(),
                                                    targetSize,
                                                    targetSize,
                                                    physicalDevice,
                                                    logicalDevice,
                                                    commandPool,
                                                    queue);
    }

    // 非 SVG 格式交给 VKTexture 的位图文件加载路径处理。
    return std::make_unique<Graphic::VKTexture>(
        path, physicalDevice, logicalDevice, commandPool, queue);
}

}  // namespace MMM::UI
