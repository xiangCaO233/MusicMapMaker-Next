#pragma once

#include "config/visual/SpectrumConfig.h"
#include "graphic/imguivk/mesh/VKBasicVertex.h"
#include "ui/IRenderableView.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

namespace ice
{
class GraphicEqualizer;
class AudioBuffer;
}  // namespace ice

namespace MMM::Logic
{
struct RenderSnapshot;
}  // namespace MMM::Logic

namespace MMM::Graphic
{
class VKTexture;
}  // namespace MMM::Graphic

namespace MMM::UI
{

/// @brief 高频谱概览视图。
/// 通过后台 CPU FFT 生成 R8 强度缓存，再转换为 RGBA8 色图纹理并由离屏
/// Vulkan 管线绘制。
class AudioSpectrumView : public IRenderableView
{
public:
    AudioSpectrumView(const std::string& name);
    ~AudioSpectrumView() override;

    /// @brief ImGui 更新
    void update(UIManager* sourceManager) override;

    /// @brief 是否需要重载纹理
    bool needReload() override;

    /// @brief 重载纹理 (由 UIManager 在准备资源阶段回调)
    void reloadTextures(vk::PhysicalDevice& physicalDevice,
                        vk::Device& logicalDevice, vk::CommandPool& cmdPool,
                        vk::Queue& queue) override;

private:
    /// @brief 离屏绘制频谱纹理分块时使用的命令。
    struct SpectrumDrawCmd {
        /// @brief 需要绑定的频谱纹理。
        Graphic::VKTexture* texture{ nullptr };

        /// @brief 索引数量。
        uint32_t indexCount{ 0 };

        /// @brief 索引偏移。
        uint32_t indexOffset{ 0 };
    };

    /// @brief 同步 EQ 参数到预览 EQ
    void syncEQ();

    /// @brief 启动异步全局预计算 (非阻塞)
    void startAsyncRecalculate();

    /// @brief 后台线程执行体：多核并行 FFT 计算
    struct EQSettings {
        bool                enabled{ false };
        std::vector<double> freqs;
        std::vector<double> gains;
        std::vector<double> qs;
    };
    /// @brief 后台重新计算完整频谱缓存。
    /// @param eq 计算时捕获的 EQ 设置。
    /// @param maxFreq 最高显示频率。
    /// @param logBias 对数频率映射偏置。
    /// @param detailLevel 计算时捕获的频谱精细度枚举。
    /// @param detailProfile 计算时捕获的频谱精细度参数。
    /// @warning 后台耗时路径：执行完整音频 FFT 计算，不在 UI/渲染热路径运行。
    void backgroundRecalculate(std::stop_token stopToken, const EQSettings& eq,
                               float maxFreq, float logBias,
                               Config::SpectrumDetailLevel   detailLevel,
                               Config::SpectrumDetailProfile detailProfile);

    /// @brief 添加一个带自定义 UV 的频谱分块矩形。
    void addSpectrumQuad(float x, float y, float w, float h, float uv0X,
                         float uv1X, Graphic::VKTexture* texture);

    /// @brief 构建指定通道的离屏绘制几何。
    /// @param textures 当前通道的频谱纹理分块。
    /// @param plotY 通道绘制区域在离屏表面内的 Y 坐标。
    /// @param plotW 通道绘制区域宽度。
    /// @param plotH 通道绘制区域高度。
    /// @param viewStart 当前全局视觉视野起点，单位为秒。
    /// @param viewEnd 当前全局视觉视野终点，单位为秒。
    /// @param spectrumVisualOffset 频谱采样内容使用的专用偏移，单位为秒。
    void buildChannelGeometry(
        const std::vector<std::unique_ptr<Graphic::VKTexture>>& textures,
        float plotY, float plotW, float plotH, double viewStart, double viewEnd,
        float spectrumVisualOffset);

    /// @brief 绘制指定通道的 ImGui 交互覆盖层。
    /// @param seekId ImGui 隐形交互区域 ID。
    /// @param channelIndex 交互通道索引，用于绑定拖动生命周期。
    /// @param groupMin 交互区域左上角。
    /// @param groupMax 交互区域右下角。
    /// @param viewStart 当前全局视觉视野起点，单位为秒。
    /// @param viewEnd 当前全局视觉视野终点，单位为秒。
    /// @param globalVisualOffset 全局视觉偏移，单位为秒。
    /// @param totalTime 音频总时长，单位为秒。
    /// @param visualTime 当前全局视觉时间，单位为秒。
    /// @param snapshot 当前活动画布同步快照，可以为空。
    void renderChannelInteractionOverlay(const char* seekId, int channelIndex,
                                         ImVec2 groupMin, ImVec2 groupMax,
                                         double viewStart, double viewEnd,
                                         float  globalVisualOffset,
                                         double totalTime, double visualTime,
                                         const Logic::RenderSnapshot* snapshot);

    std::shared_ptr<ice::GraphicEqualizer> m_previewEQ;
    std::unique_ptr<ice::AudioBuffer>      m_processBuffer;
    std::unique_ptr<ice::AudioBuffer>      m_rawBuffer;

    // --- 全局缓存数据 ---
    /// @brief R8 高分辨率强度缓存，行优先 [bin * totalSegments + t]。
    std::vector<std::uint8_t> m_cachedIntensityL;

    /// @brief R8 高分辨率强度缓存，行优先 [bin * totalSegments + t]。
    std::vector<std::uint8_t> m_cachedIntensityR;

    /// @brief 当前缓存时间分辨率，单位为段/秒。
    double m_cacheSegmentsPerSecond{ 100.0 };

    /// @brief 缓存的总时间段数
    int m_cachedNumTotalSegments{ 0 };

    /// @brief 当前频率 bin 数，映射到 20Hz~20kHz 对数空间。
    int m_numFrequencyBins{ 128 };

    /// @brief 当前已生成频谱纹理对应的全局精细度。
    Config::SpectrumDetailLevel m_spectrumDetailLevel{
        Config::SpectrumDetailLevel::Balanced
    };

    /// @brief 后台计算完成后等待主线程提交的精细度。
    Config::SpectrumDetailLevel m_pendingSpectrumDetailLevel{
        Config::SpectrumDetailLevel::Balanced
    };

    /// @brief 后台计算完成后等待主线程提交的频谱时间分辨率。
    double m_pendingCacheSegmentsPerSecond{ 100.0 };

    /// @brief 后台计算完成后等待主线程提交的频率 bin 数。
    int m_pendingNumFrequencyBins{ 128 };

    // --- 异步计算状态 ---

    /// @brief 后台计算在线程池中的任务句柄。
    /// @warning 生命周期路径：仅由用户触发重算或窗口销毁时写入；任务体不在 UI
    /// 热路径执行。
    std::future<void> m_calcFuture;

    /// @brief 后台计算停止请求源。
    /// @warning 跨线程停止信号：UI 线程低频写入，后台 FFT
    /// 任务分段读取，只承载取消标志。
    std::stop_source m_calcStopSource;

    /// @brief 是否正在后台计算
    std::atomic<bool> m_isCalculating{ false };

    /// @brief 计算进度 [0.0, 1.0]
    std::atomic<float> m_calcProgress{ 0.0f };

    /// @brief 后台计算是否已完成 (主线程读取后重置)
    std::atomic<bool> m_calcFinished{ false };

    // --- 像素缓冲与纹理 (全量静态存储) ---

    /// @brief 当前正在显示的纹理分块存储 (L/R 通道)。
    std::vector<std::unique_ptr<Graphic::VKTexture>> m_texturesL;
    std::vector<std::unique_ptr<Graphic::VKTexture>> m_texturesR;

    /// @brief 当前正在逐帧上传的纹理分块存储 (L/R 通道)。
    std::vector<std::unique_ptr<Graphic::VKTexture>> m_loadingTexturesL;
    std::vector<std::unique_ptr<Graphic::VKTexture>> m_loadingTexturesR;

    /// @brief 上一代纹理延迟释放缓存，避免切换帧销毁仍可能被 GPU 使用的资源。
    std::vector<std::unique_ptr<Graphic::VKTexture>> m_retiredTexturesL;
    std::vector<std::unique_ptr<Graphic::VKTexture>> m_retiredTexturesR;

    /// @brief 纹理分块的 RGBA8 色图数据缓冲区 (待上传)。
    struct TextureChunkData {
        /// @brief RGBA8 像素数据。
        std::vector<unsigned char> pixels;

        /// @brief 分块宽度。
        uint32_t width;

        /// @brief 分块高度。
        uint32_t height;
    };
    std::vector<TextureChunkData> m_pendingChunksL;
    std::vector<TextureChunkData> m_pendingChunksR;

    /// @brief 纹理是否需要继续分块上传。
    bool m_texturesNeedReload{ false };

    /// @brief 当前分块上传流程是否已经开始。
    bool m_textureReloadStarted{ false };

    /// @brief 下一个要上传的纹理分块索引。
    std::size_t m_nextChunkUploadIndex{ 0 };

    /// @brief 分块宽度 (通常取 16384 或更小以适配硬件限制)
    static constexpr uint32_t MAX_TEXTURE_W{ 16384 };

    /// @brief 每帧最多上传的 L/R 分块对数量。
    static constexpr std::size_t MAX_UPLOAD_CHUNK_PAIRS_PER_FRAME{ 1 };

    /// @brief 构建全量像素缓冲并准备上传
    void prepareFullGlobalTextures();

    /// @brief 是否需要重新记录离屏绘制命令。
    /// @warning 热路径：渲染准备阶段可能读取；当前频谱视图每帧都会更新播放头和
    /// 覆盖层，因此恒定 dirty。
    bool isDirty() const override;

    /// @brief 频谱窗口尺寸变化回调。
    void resizeCall(uint32_t oldW, uint32_t oldH, uint32_t w,
                    uint32_t h) const override;

    /// @brief 获取频谱离屏 shader 源码。
    std::vector<std::string> getShaderSources(
        const std::string& shaderName) override;

    /// @brief 获取频谱离屏 shader 名称。
    std::string getShaderName(const std::string& shaderModuleName) override;

    /// @brief 获取离屏绘制顶点。
    const std::vector<Graphic::Vertex::VKBasicVertex>&
    getVertices() const override;

    /// @brief 获取离屏绘制索引。
    const std::vector<uint32_t>& getIndices() const override;

    /// @brief 录制频谱分块离屏绘制命令。
    /// @warning 热路径：每帧离屏命令录制时执行；只遍历已生成的分块命令并复用
    /// descriptor。
    void onRecordDrawCmds(vk::CommandBuffer&      cmdBuf,
                          vk::PipelineLayout      pipelineLayout,
                          vk::DescriptorSetLayout setLayout,
                          vk::DescriptorSet       defaultDescriptor,
                          uint32_t                frameIndex) override;

    /// @brief 清空缓存的 shader 源码。
    /// @warning 低频资源重载路径：皮肤热切换时执行，禁止放入命令录制热路径。
    void invalidateShaderSourceCache() override;

    /// @brief 离屏绘制顶点缓存。
    std::vector<Graphic::Vertex::VKBasicVertex> m_vertices;

    /// @brief 离屏绘制索引缓存。
    std::vector<uint32_t> m_indices;

    /// @brief 离屏绘制命令缓存。
    std::vector<SpectrumDrawCmd> m_spectrumDrawCmds;

    /// @brief SPIR-V shader 源码缓存。
    std::unordered_map<std::string, std::vector<std::string>>
        m_shaderSourceCache;

    // --- 视图状态 ---
    float m_zoom{ 1.0f };
    float m_maxFreq{ 20000.0f };
    float m_logBias{ 6.91f };

    /// @brief 当前拥有频谱拖动生命周期的通道索引，-1 表示没有拖动。
    int m_seekDragOwnerChannel{ -1 };

    /// @brief 频谱拖动期间独立推进的视觉视野中心，单位为秒。
    double m_seekDragViewCenter{ 0.0 };
};

}  // namespace MMM::UI
