#include "ui/ITextureLoader.h"

#include "graphic/imguivk/VKTexture.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <cctype>
#include <lunasvg.h>
#include <system_error>

namespace MMM::UI
{

std::unique_ptr<Graphic::VKTexture> ITextureLoader::loadTextureResource(
    const std::filesystem::path& path, uint32_t targetSize,
    vk::PhysicalDevice& physicalDevice, vk::Device& logicalDevice,
    vk::CommandPool& commandPool, vk::Queue& queue,
    std::optional<std::array<float, 4>> overrideColor)
{
    std::error_code texturePathError;
    if ( !std::filesystem::exists(path, texturePathError) ||
         texturePathError ) {
        const auto u8Path = path.u8string();
        XWARN("Texture path not found: {}",
              std::string(reinterpret_cast<const char*>(u8Path.c_str()),
                          u8Path.size()));
        return nullptr;
    }

    const auto  extension = path.extension().u8string();
    std::string extensionString(
        reinterpret_cast<const char*>(extension.c_str()), extension.size());
    std::transform(extensionString.begin(),
                   extensionString.end(),
                   extensionString.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });

    if ( extensionString == ".svg" ) {
        const auto  u8Path = path.u8string();
        std::string utf8Path(reinterpret_cast<const char*>(u8Path.c_str()),
                             u8Path.size());
        auto        document = lunasvg::Document::loadFromFile(utf8Path);
        if ( !document ) {
            XWARN("lunasvg failed to load: {}", utf8Path);
            return nullptr;
        }

        auto bitmap = document->renderToBitmap(targetSize, targetSize);
        bitmap.convertToRGBA();

        if ( overrideColor.has_value() ) {
            uint8_t*    pixels = bitmap.data();
            const auto& color  = overrideColor.value();

            const uint8_t targetRed = static_cast<uint8_t>(
                std::clamp(color[0] * 255.0f, 0.0f, 255.0f));
            const uint8_t targetGreen = static_cast<uint8_t>(
                std::clamp(color[1] * 255.0f, 0.0f, 255.0f));
            const uint8_t targetBlue = static_cast<uint8_t>(
                std::clamp(color[2] * 255.0f, 0.0f, 255.0f));

            for ( uint32_t index = 0; index < targetSize * targetSize;
                  ++index ) {
                pixels[index * 4 + 0] = targetRed;
                pixels[index * 4 + 1] = targetGreen;
                pixels[index * 4 + 2] = targetBlue;
            }
        }

        return std::make_unique<Graphic::VKTexture>(bitmap.data(),
                                                    targetSize,
                                                    targetSize,
                                                    physicalDevice,
                                                    logicalDevice,
                                                    commandPool,
                                                    queue);
    }

    return std::make_unique<Graphic::VKTexture>(
        path, physicalDevice, logicalDevice, commandPool, queue);
}

}  // namespace MMM::UI
