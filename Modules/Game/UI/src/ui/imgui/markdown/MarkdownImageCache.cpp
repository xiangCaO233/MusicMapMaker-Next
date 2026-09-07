#include "ui/imgui/markdown/MarkdownImageCache.h"

#include "common/VideoFrameDecoder.h"
#include "graphic/imguivk/VKTexture.h"
#include "runtime/AppThreadPool.h"
#include "ui/imgui/markdown/MarkdownParser.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <ice/thread/ThreadPool.hpp>
#include <map>
#include <memory>
#include <span>
#include <stb_image.h>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace MMM::UI
{
namespace
{
constexpr std::size_t MAX_DOWNLOAD_BYTES =
    32U * 1024U * 1024U;  ///< 单个下载上限。
constexpr std::size_t MAX_CACHE_BYTES =
    192U * 1024U * 1024U;  ///< GPU 图集总预算。
/// @brief 限制下载大小，拒绝无界响应。
std::size_t appendDownload(char* data, std::size_t size, std::size_t count,
                           void* context)
{
    auto& bytes = *static_cast<std::vector<unsigned char>*>(context);
    if ( size && count > (MAX_DOWNLOAD_BYTES - bytes.size()) / size ) return 0;
    const auto length = size * count;
    bytes.insert(bytes.end(), data, data + length);
    return length;
}
/// @brief 将一帧缩小后写入图集，避免存储原尺寸动画的全部帧。
void copyFrame(MarkdownImagePixels& out, unsigned index,
               const unsigned char* source, unsigned width, unsigned height)
{
    const unsigned left = (index % out.columns) * out.frameWidth;
    const unsigned top  = (index / out.columns) * out.frameHeight;
    for ( unsigned y = 0; y < out.frameHeight; ++y ) {
        for ( unsigned x = 0; x < out.frameWidth; ++x ) {
            const auto sourceOffset =
                (static_cast<std::size_t>(y) * height / out.frameHeight *
                     width +
                 static_cast<std::size_t>(x) * width / out.frameWidth) *
                4U;
            const auto targetOffset =
                (static_cast<std::size_t>(top + y) * out.width + left + x) * 4U;
            std::memcpy(
                out.pixels.data() + targetOffset, source + sourceOffset, 4U);
        }
    }
}
/// @brief 创建有界帧网格，保持宽高比。
MarkdownImagePixels makeAtlas(unsigned width, unsigned height, unsigned frames,
                              double duration)
{
    MarkdownImagePixels out;
    const double        scale =
        std::min(1.0, (frames > 1U ? 320.0 : 1280.0) / std::max(width, height));
    out.frameWidth  = std::max(1U, static_cast<unsigned>(width * scale));
    out.frameHeight = std::max(1U, static_cast<unsigned>(height * scale));
    out.columns     = std::min(frames, 4096U / out.frameWidth);
    out.frames      = frames;
    out.width       = out.columns * out.frameWidth;
    out.height = ((frames + out.columns - 1U) / out.columns) * out.frameHeight;
    out.duration = duration;
    out.pixels.resize(static_cast<std::size_t>(out.width) * out.height * 4U);
    return out;
}

/// @brief 只清理本次成功创建的独立临时目录，不触及用户资源。
struct TemporaryImage {
    std::filesystem::path path;  ///< 精确的任务目录。
    /// @brief 解码器先关闭文件后，回收任务临时文件。
    ~TemporaryImage()
    {
        if ( !path.empty() ) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
};
}  // namespace

std::string resolveUpdateImageUrl(std::string_view destination)
{
    if ( destination.empty() ||
         destination.find_first_of("\r\n\t\\") != std::string_view::npos )
        return {};
    if ( destination.starts_with("https://") ||
         destination.starts_with("http://") )
        return std::string(destination);
    if ( destination.starts_with("//") )
        return "https:" + std::string(destination);
    if ( destination.starts_with('/') )
        return "https://mmm.xiang233.top" + std::string(destination);
    if ( destination.find(':') != std::string_view::npos ) return {};
    return "https://mmm.xiang233.top/download/check/" +
           std::string(destination);
}

MarkdownImagePixels loadUpdateImage(const std::string& url)
{
    std::vector<unsigned char> bytes;
    auto curl = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>(
        curl_easy_init(), curl_easy_cleanup);
    if ( !curl ) return {};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, appendDownload);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &bytes);
    if ( curl_easy_perform(curl.get()) != CURLE_OK || bytes.empty() ) return {};
    return decodeUpdateImage(bytes);
}

MarkdownImagePixels decodeUpdateImage(std::span<const unsigned char> bytes)
{
    if ( bytes.empty() || bytes.size() > MAX_DOWNLOAD_BYTES ) return {};
    int width = 0, height = 0, channels = 0;
    if ( !stbi_info_from_memory(bytes.data(),
                                static_cast<int>(bytes.size()),
                                &width,
                                &height,
                                &channels) ||
         width <= 0 || height <= 0 || width > 4096 || height > 4096 )
        return {};
    const bool gif =
        bytes.size() >= 6U && (std::memcmp(bytes.data(), "GIF89a", 6U) == 0 ||
                               std::memcmp(bytes.data(), "GIF87a", 6U) == 0);
    if ( !gif ) {
        auto pixels =
            std::unique_ptr<unsigned char, decltype(&stbi_image_free)>(
                stbi_load_from_memory(bytes.data(),
                                      static_cast<int>(bytes.size()),
                                      &width,
                                      &height,
                                      &channels,
                                      4),
                stbi_image_free);
        if ( !pixels ) return {};
        auto out = makeAtlas(width, height, 1U, 0.0);
        copyFrame(out, 0U, pixels.get(), width, height);
        return out;
    }
    std::error_code ec;
    const auto      root = std::filesystem::temp_directory_path(ec);
    if ( ec ) return {};
    TemporaryImage temporary;
    const auto     stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for ( unsigned attempt = 0; attempt < 16U; ++attempt ) {
        auto path = root / ("mmm-update-image-" + std::to_string(stamp) + "-" +
                            std::to_string(attempt));
        if ( std::filesystem::create_directory(path, ec) ) {
            temporary.path = std::move(path);
            break;
        }
    }
    if ( temporary.path.empty() ) return {};
    const auto path = temporary.path / "image.gif";
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        file.close();
        if ( !file ) return {};
    }
    Utils::VideoFrameDecoder decoder;
    if ( !decoder.open(path) ) return {};
    const double duration = decoder.info().duration;
    if ( !std::isfinite(duration) || duration <= 0.0 || duration > 120.0 )
        return {};
    const unsigned frames =
        std::clamp(static_cast<unsigned>(std::ceil(duration * 15.0)), 1U, 96U);
    auto out = makeAtlas(width, height, frames, duration);
    for ( unsigned index = 0; index < frames; ++index ) {
        const auto* frame = decoder.decodeFrameAt(duration * index / frames);
        if ( !frame ) return {};
        copyFrame(out, index, frame->rgba.data(), frame->width, frame->height);
    }
    return out;
}

/// @brief 缓存仅在 UI/资源准备线程访问，工作任务只返回独占 CPU 数据。
struct MarkdownImageCache::Impl {
    struct Entry {
        std::unique_ptr<Graphic::VKTexture> m_texture;  ///< 图集 GPU 所有权。
        MarkdownImagePixels                 m_layout;  ///< 不保留像素的帧布局。
        ImTextureID m_id{};         ///< 资源准备阶段注册的描述符。
        double      m_startTime{};  ///< 动画起播时钟。
        bool        m_failed{};     ///< 失败后不逐帧重复请求。
    };
    std::map<std::string, Entry, std::less<>>
                                     m_entries;  ///< 按 Markdown 目标去重。
    std::vector<std::string>         m_pending;  ///< 有界、尚未开始下载的键。
    std::string                      m_active;   ///< 当前任务键。
    std::future<MarkdownImagePixels> m_future;   ///< 单任务非阻塞交接。
    std::size_t                      m_bytes{};  ///< 已上传图集总大小。
};
MarkdownImageCache::MarkdownImageCache()
    : IUIView("UpdateMarkdownImages")
    , ITextureLoader("UpdateMarkdownImages")
    , m_impl(std::make_unique<Impl>())
{
}
MarkdownImageCache::~MarkdownImageCache() = default;
void MarkdownImageCache::prepareDocument(std::string_view markdown)
{
    visitMarkdownBlocks(markdown, [&](const MarkdownBlock& block) {
        if ( block.kind == MarkdownBlockKind::Code ) return;
        visitMarkdownInline(block.text, [&](const MarkdownInlineSpan& span) {
            if ( span.kind != MarkdownInlineKind::Image ||
                 m_impl->m_entries.contains(span.destination) ||
                 m_impl->m_entries.size() >= 32U )
                return;
            auto  key      = std::string(span.destination);
            auto& entry    = m_impl->m_entries[key];
            entry.m_failed = resolveUpdateImageUrl(key).empty();
            if ( !entry.m_failed ) m_impl->m_pending.push_back(std::move(key));
        });
    });
}
bool MarkdownImageCache::needReload()
{
    if ( !m_impl->m_future.valid() && !m_impl->m_pending.empty() ) {
        auto* pool = Runtime::AppThreadPool::instance().get();
        if ( !pool ) return false;
        m_impl->m_active = std::move(m_impl->m_pending.front());
        m_impl->m_pending.erase(m_impl->m_pending.begin());
        const auto url = resolveUpdateImageUrl(m_impl->m_active);
        m_impl->m_future =
            pool->enqueue([url]() { return loadUpdateImage(url); });
    }
    return m_impl->m_future.valid() &&
           m_impl->m_future.wait_for(std::chrono::seconds(0)) ==
               std::future_status::ready;
}
void MarkdownImageCache::reloadTextures(vk::PhysicalDevice& physical,
                                        vk::Device&         device,
                                        vk::CommandPool& pool, vk::Queue& queue)
{
    if ( !m_impl->m_future.valid() ||
         m_impl->m_future.wait_for(std::chrono::seconds(0)) !=
             std::future_status::ready )
        return;
    auto       data = m_impl->m_future.get();
    const auto it   = m_impl->m_entries.find(m_impl->m_active);
    if ( it == m_impl->m_entries.end() ) return;
    auto& entry = it->second;
    if ( data.pixels.empty() ||
         data.pixels.size() > MAX_CACHE_BYTES - m_impl->m_bytes ) {
        entry.m_failed = true;
        return;
    }
    auto texture = std::make_unique<Graphic::VKTexture>(data.pixels.data(),
                                                        data.width,
                                                        data.height,
                                                        physical,
                                                        device,
                                                        pool,
                                                        queue);
    if ( !texture->isValid() ) {
        entry.m_failed = true;
        return;
    }
    entry.m_id      = texture->getImTextureID();
    entry.m_texture = std::move(texture);
    m_impl->m_bytes += data.pixels.size();
    std::vector<unsigned char>().swap(data.pixels);
    entry.m_layout    = std::move(data);
    entry.m_startTime = ImGui::GetTime();
}
MarkdownImage MarkdownImageCache::findImage(std::string_view destination) const
{
    const auto it = m_impl->m_entries.find(destination);
    if ( it == m_impl->m_entries.end() ) return { .failed = true };
    const auto& entry = it->second;
    if ( !entry.m_texture ) return { .failed = entry.m_failed };
    const auto&    layout = entry.m_layout;
    const unsigned frame =
        layout.duration > 0.0
            ? std::min(
                  layout.frames - 1U,
                  static_cast<unsigned>(
                      std::fmod(
                          std::max(0.0, ImGui::GetTime() - entry.m_startTime),
                          layout.duration) /
                      layout.duration * layout.frames))
            : 0U;
    const float x =
        static_cast<float>((frame % layout.columns) * layout.frameWidth);
    const float y =
        static_cast<float>((frame / layout.columns) * layout.frameHeight);
    return { entry.m_id,
             { static_cast<float>(layout.frameWidth),
               static_cast<float>(layout.frameHeight) },
             { (x + 0.5F) / layout.width, (y + 0.5F) / layout.height },
             { (x + layout.frameWidth - 0.5F) / layout.width,
               (y + layout.frameHeight - 0.5F) / layout.height },
             false };
}
}  // namespace MMM::UI
