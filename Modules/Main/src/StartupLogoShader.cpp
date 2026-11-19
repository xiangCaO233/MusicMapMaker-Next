#include "main/StartupLogoShader.h"

#include "StartupLogoSvg.h"

#include "graphic/imguivk/VKRenderPass.h"
#include "graphic/imguivk/VKShader.h"
#include "graphic/imguivk/VKSwapchain.h"
#include "graphic/imguivk/VKTexture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <lunasvg.h>
#include <string_view>
#include <utility>
#include <vulkan/vulkan.hpp>

namespace MMM::Main
{
namespace
{
// 字节码由维护的 GLSL 离线生成，运行环境既不需要 glslc，也不需要先下载资源。
// 顶点和片元均面向 Vulkan 1.0，桌面与移动窗口可共用同一份模块。
#include "StartupLogoSpirv.inc"

/// @brief 提供高 DPI 遮罩所需的栅格分辨率，独立于窗口逻辑尺寸。
constexpr std::uint32_t MASK_SIZE = 384;

/// @brief 留给轮廓外动画的空间比例，与片元着色器的视口缩放一致。
constexpr float LOGO_VIEWPORT_SCALE = 630.0f / 512.0f;

/// @brief 片元着色器需要的两个标量，布局与 GLSL push constant 一致。
/// @details 笔画颜色在纹理上传时确定；这里的灰度只用于波纹和扫描带。
/// @details 只传当前帧值。改变这个结构时需要同步修改 GLSL 并重新生成 SPIR-V。
/// 两个四字节标量按顺序传输，无矩阵或向量的对齐差异。
struct LogoStyle {
    float m_progress;  ///< 结束状态固定为一。
    float m_gray;      ///< 装饰特效的中性灰度。
};

/// @brief 单帧绘制参数，由 ImGui 复制到自己的命令列表。
/// @details 命令列表可能在 draw 返回后才执行。所有参数均为值，不指向栈或可变 UI
/// 状态。 其中 Vulkan 句柄由启动窗口对象持有到本轮主通道全部命令执行结束。
struct LogoDraw {
    vk::Pipeline       m_pipeline;  ///< 启动窗口专用管线。
    vk::PipelineLayout m_layout;    ///< 与 LogoStyle 匹配的布局。
    vk::DescriptorSet  m_mask;      ///< 内嵌 SVG 的只读采样描述符。
    ImVec2             m_origin;    ///< ImGui 屏幕坐标中的左上角。
    float              m_size;      ///< 逻辑像素边长。
    LogoStyle          m_style;     ///< 提交本帧时冻结的动画状态。
};

/// @brief 在 ImGui 主通道中录制一个无顶点缓冲的三角形。
/// @warning 每个启动帧执行，只读取本帧参数和 Vulkan 命令缓冲。
/// @param command 上一阶段登记的绘制回调，携带当前帧的复制参数。
/// @details 后端 render state 只在录制当前 draw data 时有效，不能保存到后续帧。
/// 此回调只在启动主 viewport 使用，因此可安全读取 GetDrawData 的显示变换。
void recordLogo(const ImDrawList*, const ImDrawCmd* command)
{
    // SDL 和 GLFW 的启动窗口均由同一 Vulkan 后端录制。
    // 若后端未提供命令缓冲，本回调没有可安全写入的目标。
    const auto* renderState = static_cast<const ImGui_ImplVulkan_RenderState*>(
        ImGui::GetPlatformIO().Renderer_RenderState);
    // Bootstrap 仅有主视口，draw data 和回调处于同一个坐标系。
    const auto* frame = ImGui::GetDrawData();
    // 回调参数由 ImGui 以字节存储。复制到自然对齐的栈对象再读取句柄。
    // ImGui 可能在合并窗口 draw list 时重排缓冲，因此这里不保留字节地址。
    LogoDraw draw;
    std::memcpy(&draw, command->UserCallbackData, sizeof(draw));
    if ( !frame || !renderState ) return;

    // 动态 viewport 恢复完整的 Logo 图形坐标，scissor 单独限制实际像素写入。
    // 保留负的原始 viewport 起点，窄屏裁切时不会把轮廓压扁或移动。
    // 平台坐标必须先扣除显示原点再乘 framebuffer 缩放。
    // 显示尺寸为逻辑像素；只有取整后的裁剪矩形使用物理像素。
    const float x =
        (draw.m_origin.x - frame->DisplayPos.x) * frame->FramebufferScale.x;
    const float y =
        (draw.m_origin.y - frame->DisplayPos.y) * frame->FramebufferScale.y;
    const float width  = draw.m_size * frame->FramebufferScale.x;
    const float height = draw.m_size * frame->FramebufferScale.y;
    const float maxX   = frame->DisplaySize.x * frame->FramebufferScale.x;
    const float maxY   = frame->DisplaySize.y * frame->FramebufferScale.y;
    // 左上向下、右下向上取整，保留反走样图形在像素边界的覆盖。
    // 右下端点独立裁剪，避免负偏移参与无符号宽高计算。
    const int left = static_cast<int>(std::clamp(std::floor(x), 0.0f, maxX));
    const int top  = static_cast<int>(std::clamp(std::floor(y), 0.0f, maxY));
    const int right =
        static_cast<int>(std::clamp(std::ceil(x + width), 0.0f, maxX));
    const int bottom =
        static_cast<int>(std::clamp(std::ceil(y + height), 0.0f, maxY));
    // 完全移出可见区域时无可绘制像素，不向 Vulkan 提交零尺寸裁剪区。
    if ( right <= left || bottom <= top ) return;

    // 使用当前后端命令缓冲，沿用已激活的主 render pass 和 framebuffer。
    // Logo 写入顺序由 ImGui 列表决定，背景在先、交互控件在后。
    vk::CommandBuffer buffer(renderState->CommandBuffer);
    buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, draw.m_pipeline);
    // 光栅范围允许超出窗口，但下面的 scissor 始终落在有效附件中。
    buffer.setViewport(0, vk::Viewport(x, y, width, height, 0.0f, 1.0f));
    buffer.setScissor(0,
                      vk::Rect2D({ left, top },
                                 { static_cast<std::uint32_t>(right - left),
                                   static_cast<std::uint32_t>(bottom - top) }));
    // 动画只推进两个标量，跨帧共享同一遮罩，不向纹理写入任何数据。
    // 纹理已在窗口准备阶段转为着色器只读布局，回调中无需执行同步屏障。
    buffer.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics, draw.m_layout, 0, draw.m_mask, {});
    // 片元阶段读取的布局保持到最后一次待执行的 draw 完成。
    buffer.pushConstants<LogoStyle>(
        draw.m_layout, vk::ShaderStageFlagBits::eFragment, 0, draw.m_style);
    // 顶点 shader 从三个顶点索引构造覆盖三角形，不读 ImGui 的顶点布局。
    buffer.draw(3, 1, 0, 0);
}
}  // namespace

/// @brief 只持有启动期间使用的设备及图形管线对象。
/// @details 成员按声明的逆序释放，管线先于布局销毁，设备仍由窗口上下文拥有。
/// 原生 descriptor 由 VKTexture 管理，所以它必须先于专用 descriptor pool 销毁。
/// 静态回退还使用窗口的 ImGui pool，因此本对象必须在关闭后端之前销毁。
struct StartupLogoShader::State {
    vk::Device                    m_device;  ///< 借用窗口设备，销毁时仍有效。
    vk::UniqueDescriptorSetLayout m_maskLayout;  ///< 声明片元 alpha 采样器。
    vk::UniqueDescriptorPool      m_maskPool;    ///< 纹理释放描述符后再销毁池。
    std::unique_ptr<Graphic::VKTexture> m_texture;  ///< GPU 上的单色遮罩。
    float             m_effectGray{};     ///< 按启动主题选择的特效灰度。
    ImTextureID       m_staticTexture{};  ///< 用于 ImGui 静态回退的纹理标识。
    vk::DescriptorSet m_mask;             ///< 在初始化阶段写入并供各帧复用。
    vk::UniquePipelineLayout m_layout;    ///< 裸管线销毁后才回收布局。
    vk::UniquePipeline       m_pipeline;  ///< 与当前交换链通道兼容的单色管线。
};

/// @brief 上传内嵌 SVG 的遮罩并建立透明合成管线。
/// @details 使用同构 render pass 来匹配已有的主交换链 pass，创建后即可丢弃临时
/// pass。
/// @param physicalDevice 提供一次性上传所需的内存类型。
/// @param device 窗口逻辑设备，生命周期覆盖动画及后续静态 Logo。
/// @param commandPool 用于上传的窗口命令池。
/// @param queue 用于上传的图形队列。
/// @param swapchain 提供主通道的附件格式，改变窗口面积不需要重建本管线。
/// @warning 初始化进入驱动，仅在首帧准备资源时创建一次，不从绘制回调调用。
StartupLogoShader::StartupLogoShader(vk::PhysicalDevice&    physicalDevice,
                                     vk::Device&            device,
                                     Graphic::VKSwapchain&  swapchain,
                                     const vk::CommandPool& commandPool,
                                     const vk::Queue&       queue)
    : m_state(std::make_unique<State>())
{
    // 稳定保留底层设备访问，以便离开启动阶段时与最后一次提交同步。
    m_state->m_device = device;
    // 首次启动没有皮肤文件，所以从可执行文件内的同一 SVG 母版生成遮罩。
    const auto document = lunasvg::Document::loadFromData(STARTUP_LOGO_SVG);
    if ( !document ) return;
    // 栅格化保留母版的圆角与透明留白；所有可见颜色由当前主题决定。
    auto bitmap = document->renderToBitmap(MASK_SIZE, MASK_SIZE);
    if ( !bitmap.data() ) return;
    bitmap.convertToRGBA();
    // 用内置主题底色选择明度范围；启动窗口的主题在整个同步期间固定。
    const ImVec4 bg       = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const bool   dark     = bg.x + bg.y + bg.z <= 1.5f;
    m_state->m_effectGray = dark ? 0.92f : 0.13f;

    // 保留 SVG 两笔的相对明度与透明度；两笔均为中性灰，不使用原始蓝色。
    // 深浅主题采用不同亮度范围，保证较暗一笔也能从底色中分辨。
    // LunaSVG 自有位图连续储存，与 VKTexture 的四字节像素格式一致。
    // 直接在位图上转换，避免为同一份母版分配第二个 CPU 缓冲。
    auto* pixels = bitmap.data();
    for ( std::uint32_t i = 0; i < MASK_SIZE * MASK_SIZE; ++i ) {
        auto* pixel = pixels + i * 4;
        // 使用亮度权重保留 SVG 里深笔压暗、浅笔提亮的顺序。
        const float luma =
            (0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2]) /
            255.0f;
        // 深色背景整体提亮两笔；浅色背景保留较大的文字式对比。
        const float gray = dark ? 0.34f + 0.77f * luma : -0.13f + 0.65f * luma;
        const auto  channel = static_cast<unsigned char>(
            std::round(255.0f * std::clamp(gray, 0.0f, 1.0f)));
        std::fill_n(pixel, 3, channel);
    }
    // VKTexture 在此处同步完成一次拷贝。后续 Startup UI 帧不等待上传。
    m_state->m_texture = std::make_unique<Graphic::VKTexture>(pixels,
                                                              MASK_SIZE,
                                                              MASK_SIZE,
                                                              physicalDevice,
                                                              device,
                                                              commandPool,
                                                              queue);
    if ( !m_state->m_texture->isValid() ) return;
    // 提前注册静态回退，错误路径无需在每帧绘制时进入描述符分配。
    m_state->m_staticTexture = m_state->m_texture->getImTextureID();

    // 自定义管线使用 binding 0 的组合采样器；它独立于 ImGui 后端的纹理布局。
    const vk::DescriptorSetLayoutBinding maskBinding(
        0,
        vk::DescriptorType::eCombinedImageSampler,
        1,
        vk::ShaderStageFlagBits::eFragment);
    auto maskLayout = device.createDescriptorSetLayoutUnique(
        vk::DescriptorSetLayoutCreateInfo().setBindings(maskBinding));
    // 任何后续图形对象创建失败都保留已经注册的普通 ImGui 静态纹理。
    if ( maskLayout.result != vk::Result::eSuccess ) return;
    m_state->m_maskLayout = std::move(maskLayout.value);

    // 专用小池有可预测的容量，并允许 VKTexture 在析构时独立释放描述符。
    // 启动窗口只上传一个 Logo，不与后续编辑器渲染资源争用描述符容量。
    const vk::DescriptorPoolSize maskCount(
        vk::DescriptorType::eCombinedImageSampler, 1);
    auto maskPool = device.createDescriptorPoolUnique(
        vk::DescriptorPoolCreateInfo(
            vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1)
            .setPoolSizes(maskCount));
    if ( maskPool.result != vk::Result::eSuccess ) return;
    m_state->m_maskPool = std::move(maskPool.value);
    m_state->m_mask     = m_state->m_texture->getNativeDescriptorSet(
        *m_state->m_maskPool, *m_state->m_maskLayout);
    if ( !m_state->m_mask ) return;

    // 片元阶段独占纹理采样和动画常量，顶点阶段只负责生成覆盖区域。
    const vk::PushConstantRange styleRange(
        vk::ShaderStageFlagBits::eFragment, 0, sizeof(LogoStyle));
    auto layout = device.createPipelineLayoutUnique(
        vk::PipelineLayoutCreateInfo()
            .setSetLayouts(*m_state->m_maskLayout)
            .setPushConstantRanges(styleRange));
    // 初始化失败时保持空管线，让启动界面使用静态轮廓。
    // 部分初始化的对象仍可正常析构，设备保留给窗口继续绘制错误信息。
    if ( layout.result != vk::Result::eSuccess ) return;
    // 从此处起由驱动句柄的 RAII 包装负责错误路径及正常退出的回收。
    m_state->m_layout = std::move(layout.value);

    // 与交换链使用同样的唯一颜色附件、采样数和子通道，管线只依赖其兼容性。
    Graphic::VKRenderPass pass(
        device, swapchain, vk::ImageLayout::ePresentSrcKHR);
    if ( !pass.getRenderPass() ) return;
    // 编译产物直接随主程序发布，首次启动不依赖资源同步或运行时着色器编译器。
    Graphic::VKShader shader(
        device,
        std::string_view(reinterpret_cast<const char*>(STARTUP_LOGO_VERTEX),
                         sizeof(STARTUP_LOGO_VERTEX)),
        std::string_view(reinterpret_cast<const char*>(STARTUP_LOGO_FRAGMENT),
                         sizeof(STARTUP_LOGO_FRAGMENT)));
    // 模块到 createGraphicsPipeline 返回为止保持有效，随后可独立销毁。
    const auto stages = shader.getShaderStageCreateInfos();

    // 顶点由 gl_VertexIndex 产生；支持任意窗口 DPI，不绑定单独的几何缓冲。
    // 空顶点描述对应程序化顶点，图元始终以三角形列表进入光栅化。
    const vk::PipelineVertexInputStateCreateInfo   inputs;
    const vk::PipelineInputAssemblyStateCreateInfo assembly(
        {}, vk::PrimitiveTopology::eTriangleList);
    // 位置和窗口面积可变，不把视口烘焙到管线中。
    const auto viewport = vk::PipelineViewportStateCreateInfo()
                              .setViewportCount(1)
                              .setScissorCount(1);
    // 图形不依赖三角形绕序，不开启背面剔除或额外的可选光栅化特性。
    const auto raster = vk::PipelineRasterizationStateCreateInfo()
                            .setPolygonMode(vk::PolygonMode::eFill)
                            .setCullMode(vk::CullModeFlagBits::eNone)
                            .setLineWidth(1.0f);
    // 主通道为单次采样，轮廓边缘由栅格化遮罩和线性采样提供。
    const vk::PipelineMultisampleStateCreateInfo samples(
        {}, vk::SampleCountFlagBits::e1);
    // 片元输出为直通 alpha，Logo 和光晕均在默认皮肤的窗口底色上合成。
    // RGB 使用遮罩中的笔画明度或特效灰度，零覆盖像素完全保留先前底色。
    const auto attachment =
        vk::PipelineColorBlendAttachmentState()
            .setBlendEnable(true)
            .setSrcColorBlendFactor(vk::BlendFactor::eSrcAlpha)
            .setDstColorBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha)
            .setSrcAlphaBlendFactor(vk::BlendFactor::eOne)
            .setDstAlphaBlendFactor(vk::BlendFactor::eOneMinusSrcAlpha)
            .setColorWriteMask(vk::ColorComponentFlagBits::eR |
                               vk::ColorComponentFlagBits::eG |
                               vk::ColorComponentFlagBits::eB |
                               vk::ColorComponentFlagBits::eA);
    // 附件数必须与单色子通道一致，动态状态随后由录制回调写入。
    const auto blend =
        vk::PipelineColorBlendStateCreateInfo().setAttachments(attachment);
    // 分辨率和缩放影响回调参数，不需要对每次窗口尺寸建立不同的固定管线。
    const std::array states{ vk::DynamicState::eViewport,
                             vk::DynamicState::eScissor };
    const auto       dynamic =
        vk::PipelineDynamicStateCreateInfo().setDynamicStates(states);
    // 管线真正创建后才允许 draw，初始化中途失败不会留下可提交的部分状态。
    auto pipeline = device.createGraphicsPipelineUnique(
        nullptr,
        vk::GraphicsPipelineCreateInfo()
            .setStages(stages)
            .setPVertexInputState(&inputs)
            .setPInputAssemblyState(&assembly)
            .setPViewportState(&viewport)
            .setPRasterizationState(&raster)
            .setPMultisampleState(&samples)
            .setPColorBlendState(&blend)
            .setPDynamicState(&dynamic)
            .setLayout(*m_state->m_layout)
            .setRenderPass(pass.getRenderPass()));
    // 驱动结果由无异常接口返回。未创建的管线留空，UI 会显示静态回退轮廓。
    if ( pipeline.result == vk::Result::eSuccess )
        m_state->m_pipeline = std::move(pipeline.value);
}

/// @brief 先结束 GPU 对管线的读取，再释放独立句柄。
/// @details 窗口负责整体 Vulkan 上下文；此处只覆盖本对象的纹理、布局与管线。
/// @warning 只在离开整个启动流程时等待一次，绝不进入帧绘制路径。
StartupLogoShader::~StartupLogoShader()
{
    // render 回调已退出不代表 GPU 已执行完毕，最后一帧可能仍引用管线。
    // 即便自定义管线未建成，普通 ImGui 静态路径仍可能在采样这一纹理。
    if ( m_state->m_texture ) (void)m_state->m_device.waitIdle();
}

/// @brief 只有完整的 Vulkan 图形管线才会提交回调。
bool StartupLogoShader::isValid() const
{
    // 布局创建先于管线，因此检查后者即可判定初始化事务成功。
    return static_cast<bool>(m_state->m_pipeline);
}

/// @brief 为当前帧保存绘制参数，并让 ImGui 恢复随后的控件绘制状态。
/// @warning 热路径：只追加固定大小的回调数据，不访问文件或创建 Vulkan 资源。
void StartupLogoShader::draw(float x, float y, float size, float progress) const
{
    auto& list = *ImGui::GetWindowDrawList();
    if ( !isValid() ) {
        // 复用 GPU 遮罩的中心和缩放，使两种路径保持同一张图。
        // inset 抵消特效区域留白，与 shader 稳定状态的 SVG 视口匹配。
        if ( m_state->m_staticTexture ) {
            const float inset = (size - size / LOGO_VIEWPORT_SCALE) * 0.5f;
            // 已换算好的两笔灰度保存在纹理中，回退路径不再覆写顶点颜色。
            list.AddImage(m_state->m_staticTexture,
                          { x + inset, y + inset },
                          { x + size - inset, y + size - inset },
                          { 0, 0 },
                          { 1, 1 });
        }
        return;
    }

    // 冻结本帧的坐标和 uniform，下一帧推进动画时不覆盖已登记的数据。
    // 回调直接写入主交换链图像，无需另建同步纹理或跨通道采样。
    LogoDraw params{ *m_state->m_pipeline,
                     *m_state->m_layout,
                     m_state->m_mask,
                     { x, y },
                     size,
                     { progress, m_state->m_effectGray } };
    // clip rect 不控制回调视口；Logo 以主窗口坐标为定位基准。
    // 指定大小使 ImGui 复制数据，回调不会引用此函数的局部变量。
    list.AddCallback(recordLogo, &params, sizeof(params));
    // 后续普通 ImGui 命令仍依赖默认顶点和管线需要的阶段状态。
    // 通过后端提供的重置回调恢复 viewport 与图形管线，避免污染进度和弹窗。
    list.AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState);
}
}  // namespace MMM::Main
