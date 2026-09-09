#include "logic/ecs/system/BackgroundRenderSystem.h"

#include "audio/BackgroundSpectrum.h"
#include "log/colorful-log.h"
#include "logic/ecs/system/BackgroundSpectrumRenderSystem.h"
#include "logic/ecs/system/render/Batcher.h"

#include <cmath>

// 本组测试检查 CPU 侧快照中的顶点、索引和绘制命令，不创建窗口或提交 GPU 命令。
// 背景资源路径只用于选择绘制分支，不要求磁盘上存在对应图片。
// 频谱使用确定的双声道幅度，避免把音频采集或 FFT 的误差带入布局回归。
namespace
{

/// @brief 使用小容差比较背景顶点数据。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
/// @note NaN 不会通过该比较；无穷值也不作为有效几何预期。
bool near(float lhs, float rhs)
{
    // 这些预期值是小范围画布坐标与归一化颜色，统一使用绝对误差即可。
    return std::abs(lhs - rhs) < 1e-6f;
}

/// @brief 验证无背景资源时仍生成覆盖整个视口的暗化混合层。
/// @return 行为符合预期时返回 true。
/// @note 同时检查图元数量、覆盖区域与颜色，避免只生成命令却没有正确几何。
bool testMissingBackgroundUsesFixedOverlay()
{
    // 有谱面但没有背景路径，区别于下面的空会话场景。
    // 特意留下小尺寸元数据，防止无图分支错误地按旧资源尺寸铺底。
    MMM::Logic::RenderSnapshot snapshot;
    snapshot.hasBeatmap = true;
    snapshot.bgSize     = { 32.0f, 32.0f };
    // 提供有效的纯色纹理图集区域；本例不验证 UV 数值或图集加载过程。
    snapshot.uvMap.emplace(static_cast<uint32_t>(MMM::Logic::TextureID::None),
                           glm::vec4{ 0.1f, 0.2f, 0.3f, 0.4f });

    // 选择 Center 而非 Stretch，确认缺图覆盖层不受图片填充方式支配。
    // 暗化与透明度取不同的非端点值，可区分 RGB 乘色和 alpha 的来源。
    MMM::Config::EditorConfig config;
    config.visual.background.fillMode = MMM::Config::BackgroundFillMode::Center;
    config.visual.background.darken_ratio = 0.25f;
    config.visual.background.opaque_ratio = 0.4f;

    // 使用独立快照隔离用例；数量断言不需要减去其他场景的历史输出。
    MMM::Logic::System::Batcher batcher(&snapshot);
    MMM::Logic::System::BackgroundRenderSystem::render(
        batcher, 320.0f, 180.0f, config, &snapshot);
    // Batcher 延迟提交命令，必须刷新后再检查最终的批次数量。
    batcher.flush();

    // 单个四边形对应四个顶点、六个索引和一条纯色命令。
    // 同时约束数量，避免无图分支重复叠加覆盖层或意外使用背景纹理。
    if ( snapshot.vertices.size() != 4 || snapshot.indices.size() != 6 ||
         snapshot.cmds.size() != 1 ||
         snapshot.cmds.front().customTextureId !=
             static_cast<uint32_t>(MMM::Logic::TextureID::None) ) {
        XERROR("Missing background did not generate a solid overlay draw call");
        return false;
    }

    // 顶点顺序按 Batcher 的四边形约定读取，对角点覆盖整个 320×180 视口。
    // 前面的数量检查先保护索引访问，失败时不继续解释不存在的顶点。
    const auto& topLeft     = snapshot.vertices[0];
    const auto& bottomRight = snapshot.vertices[2];
    // 坐标判定使用实际 pos，不依赖局部变量的角点命名推断上下方向。
    if ( !near(topLeft.pos.x, 0.0f) || !near(topLeft.pos.y, 180.0f) ||
         !near(bottomRight.pos.x, 320.0f) || !near(bottomRight.pos.y, 0.0f) ) {
        XERROR("Missing background overlay did not cover the viewport");
        return false;
    }

    // 亮度应为 1 - 0.25，alpha 则直接采用 0.4，而不是两者相乘。
    // 四角都要检查，确保覆盖层没有被错误地生成颜色渐变。
    for ( const auto& vertex : snapshot.vertices ) {
        if ( !near(vertex.color.r, 0.75f) || !near(vertex.color.g, 0.75f) ||
             !near(vertex.color.b, 0.75f) || !near(vertex.color.a, 0.4f) ) {
            XERROR("Missing background overlay ignored darken or opacity");
            return false;
        }
    }

    return true;
}

/// @brief 验证存在背景资源时仍使用专用背景纹理。
/// @return 行为符合预期时返回 true。
/// @note 此例验证纹理选择，不覆盖图像解码、GPU 上传或实际显示效果。
bool testConfiguredBackgroundKeepsTexture()
{
    // 非空路径触发有图分支，原图尺寸由快照直接提供，不在测试中加载文件。
    MMM::Logic::RenderSnapshot snapshot;
    snapshot.hasBeatmap     = true;
    snapshot.backgroundPath = "/project/background.png";
    snapshot.bgSize         = { 640.0f, 360.0f };

    // 使用默认暗化和透明度即可：此场景只负责有图分支，颜色已由缺图场景验证。
    MMM::Config::EditorConfig config;
    // 原图和视口保持相同比例，Stretch 排除裁剪、居中留白对命令数量的影响。
    config.visual.background.fillMode =
        MMM::Config::BackgroundFillMode::Stretch;

    MMM::Logic::System::Batcher batcher(&snapshot);
    MMM::Logic::System::BackgroundRenderSystem::render(
        batcher, 320.0f, 180.0f, config, &snapshot);
    batcher.flush();

    // 只允许一条背景命令，避免将存在图片的情况也降级为无图覆盖层。
    // 不检查纹理句柄有效性；customTextureId 是资源解析前的语义标识。
    if ( snapshot.cmds.size() != 1 ||
         snapshot.cmds.front().customTextureId !=
             static_cast<uint32_t>(MMM::Logic::TextureID::Background) ) {
        XERROR("Configured background stopped using its dedicated texture");
        return false;
    }
    return true;
}

/// @brief 验证未打开谱面时不生成背景或固定覆盖层。
/// @return 行为符合预期时返回 true。
/// @note 与缺少背景图片不同，空会话应完全跳过本系统的输出。
bool testNoBeatmapSkipsBackgroundLayer()
{
    // 默认快照没有谱面，保留该默认值以覆盖正常空会话入口。
    MMM::Logic::RenderSnapshot snapshot;
    MMM::Config::EditorConfig  config;

    // 视口仍为有效正尺寸，确保跳过原因是无谱面，而不是画布退化。
    MMM::Logic::System::Batcher batcher(&snapshot);
    MMM::Logic::System::BackgroundRenderSystem::render(
        batcher, 320.0f, 180.0f, config, &snapshot);
    batcher.flush();

    // 即使显式 flush，也不能产生空绘制命令或残留几何。
    // 三个容器一起检查，避免仅命令为空掩盖无用的顶点或索引写入。
    if ( !snapshot.vertices.empty() || !snapshot.indices.empty() ||
         !snapshot.cmds.empty() ) {
        XERROR("Background layer was generated without an open beatmap");
        return false;
    }
    return true;
}

/// @brief 验证左右声道由画布中心分别向两侧绘制且位于背景命令之后。
/// @return 立体声几何、透明度和图层顺序符合预期时返回 true。
/// @note 直接调用频谱渲染器，不依赖 AudioManager 的实时频谱更新。
/// @note 图层顺序由命令列表证明，不代表已验证 GPU 混合后的画面。
bool testStereoSpectrumRendersAboveBackground()
{
    MMM::Logic::RenderSnapshot snapshot;
    snapshot.bgSize = { 320.0F, 180.0F };

    // 频谱占满画布宽度、高度为一半，便于用明确坐标验证组件边界。
    // 双声道使用不同 RGB 和 alpha，交换声道或覆盖透明度都会使断言失败。
    MMM::Config::BackgroundSpectrumConfig config;
    config.bandCount     = 10;
    config.widthRatio    = 1.0F;
    config.heightRatio   = 0.5F;
    config.baselineRatio = 0.8F;
    config.opacity       = 0.25F;
    config.leftBarColor  = { 0.1F, 0.2F, 0.3F, 0.4F };
    config.rightBarColor = { 0.6F, 0.7F, 0.8F, 0.8F };

    // 数据和配置均提供十个频带，本例不涉及频带数量截断或重采样。
    // 仅首个频带非零，其余频带保持静音，预期只生成左右两根柱形。
    // 左右幅度不同，但本例只要求向上生长，不断言柱高的精确幅度映射。
    MMM::Audio::BackgroundSpectrumLevels levels;
    levels.bandCount = 10U;
    levels.left[0]   = 0.5F;
    levels.right[0]  = 0.75F;

    // 幅度远大于静音阈值且小于一，几何检查不会被阈值过滤或限幅主导。
    // 显式启用组件；布局由 placement 决定，而非由背景图片内容决定。
    // 保留默认缩放与横向锚点，只调整纵向锚点，形成可计算的中心位置。
    MMM::Config::CanvasComponentPlacement placement =
        MMM::Config::DEFAULT_BACKGROUND_SPECTRUM_PLACEMENT;
    placement.visible = true;
    placement.anchorY = 0.55F;

    // 背景尺寸与视口一致，前置图元固定占四个顶点，便于定位频谱顶点。
    MMM::Logic::System::Batcher batcher(&snapshot);
    // 手动提交背景四边形作为前置批次，单独验证频谱切换纹理后的顺序。
    // 两层共用 Batcher，能捕获忘记切换纯色纹理而错误合批的情况。
    batcher.setTexture(MMM::Logic::TextureID::Background);
    batcher.pushFilledQuad(0.0F,
                           180.0F,
                           320.0F,
                           180.0F,
                           snapshot.bgSize,
                           MMM::Config::BackgroundFillMode::Stretch,
                           glm::vec4{ 1.0F });
    // 不调用背景系统入口，避免混入配置可见性判断和音频管理器的数据来源。
    MMM::Logic::System::BackgroundSpectrumRenderSystem::render(
        batcher, 320.0F, 180.0F, config, placement, levels);
    batcher.flush();

    // 背景加两根柱形共三个四边形；两根纯色柱形应合入第二条命令。
    // 静音频带没有占位四边形，否则顶点和索引总数都会超过本例的预期。
    // 频谱整体只登记一个组件交互区域，不能为每根柱形分别登记。
    if ( snapshot.vertices.size() != 12U || snapshot.indices.size() != 18U ||
         snapshot.cmds.size() != 2U ||
         snapshot.canvasComponentInstances.size() != 1U ||
         snapshot.canvasComponentInstances.front().type !=
             MMM::Config::CanvasComponentType::BackgroundSpectrum ||
         snapshot.cmds[0].customTextureId !=
             static_cast<uint32_t>(MMM::Logic::TextureID::Background) ||
         snapshot.cmds[1].customTextureId !=
             static_cast<uint32_t>(MMM::Logic::TextureID::None) ) {
        XERROR("Stereo spectrum was not layered directly above background");
        return false;
    }

    // 组件中心 Y 为 180×0.55=99，高度为 180×0.5=90，因此上下边为 54 和 144。
    // 这里验证的是整个可编辑组件，不是仅包住两根非零柱形的包围框。
    // 因而边界高度仍为 90，不随当前两侧幅度 0.5 和 0.75 缩小。
    const auto& componentBounds = snapshot.canvasComponentInstances.front();
    if ( !near(componentBounds.left, 0.0F) ||
         !near(componentBounds.top, 54.0F) ||
         !near(componentBounds.right, 320.0F) ||
         !near(componentBounds.bottom, 144.0F) ) {
        XERROR("Background spectrum did not publish editable canvas bounds");
        return false;
    }

    // 前四个顶点属于背景，随后分别是左柱和右柱的四个顶点。
    // 前面的数量断言保证下面访问安全，位置断言只要求两侧不跨越中心线。
    const float centerX         = 160.0F;
    const auto& leftBottomLeft  = snapshot.vertices[4];
    const auto& leftTopRight    = snapshot.vertices[6];
    const auto& rightBottomLeft = snapshot.vertices[8];
    const auto& rightTopRight   = snapshot.vertices[10];
    // 不固定柱宽和槽内边距数值，避免把可调整的柱间视觉间隔当作接口契约。
    // 两侧共用组件底边 144 作为基线，非零幅度应向上生长而非向下延伸。
    // RGB 保留声道颜色；alpha 分别为 0.4×0.25=0.1 和 0.8×0.25=0.2。
    // 本例读取每根柱形的一个代表顶点验证颜色，不覆盖四角颜色一致性。
    if ( leftTopRight.pos.x > centerX || rightBottomLeft.pos.x < centerX ||
         !near(leftBottomLeft.pos.y, 144.0F) ||
         !near(rightBottomLeft.pos.y, 144.0F) ||
         leftTopRight.pos.y >= leftBottomLeft.pos.y ||
         rightTopRight.pos.y >= rightBottomLeft.pos.y ||
         !near(leftBottomLeft.color.r, 0.1F) ||
         !near(leftBottomLeft.color.g, 0.2F) ||
         !near(leftBottomLeft.color.b, 0.3F) ||
         !near(leftBottomLeft.color.a, 0.1F) ||
         !near(rightBottomLeft.color.r, 0.6F) ||
         !near(rightBottomLeft.color.g, 0.7F) ||
         !near(rightBottomLeft.color.b, 0.8F) ||
         !near(rightBottomLeft.color.a, 0.2F) ) {
        XERROR("Stereo spectrum geometry ignored channel split or ratios");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行背景渲染系统回归测试。
/// @return 全部测试通过时返回 0。
/// @note 无外部资源参数，失败诊断由各场景输出首个不满足的约束。
/// @note 返回值可供测试进程判定成败，不需要图形设备或音频设备初始化。
int main()
{
    // 短路执行：先验证背景入口和资源分支，再验证建立在背景之后的频谱层。
    return testNoBeatmapSkipsBackgroundLayer() &&
                   testMissingBackgroundUsesFixedOverlay() &&
                   testConfiguredBackgroundKeepsTexture() &&
                   testStereoSpectrumRendersAboveBackground()
               ? 0
               : 1;
}
