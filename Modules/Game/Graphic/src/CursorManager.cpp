#include "graphic/CursorManager.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "imgui_internal.h"

namespace MMM::Graphic
{
CursorManager::CursorManager(vk::PhysicalDevice& phyDevice,
                             vk::Device&         logicalDevice,
                             vk::CommandPool commandPool, vk::Queue queue)
{
    reloadSkinTextures(phyDevice, logicalDevice, commandPool, queue);

    /// @brief 拖尾存活时间(秒)
    // float m_trailLifeTime = 0.3f;
    /// @bried 光标头纹理的大小
    // float m_cursorSize = 48.0f;
    /// @brief 拖尾纹理的基础大小
    // float m_trailSize = 32.0f;
    /// @brief 鼠标移动多远才生成一个新的拖尾点(防止密集堆叠)
    // float m_emitDistance = 2.0f;
}

CursorManager::~CursorManager() {}

/// @brief 重新加载软件光标相关皮肤纹理。
/// @param phyDevice Vulkan 物理设备。
/// @param logicalDevice Vulkan 逻辑设备。
/// @param commandPool 上传命令池。
/// @param queue 上传队列。
/// @warning 低频资源重载路径：皮肤热切换时调用，会等待设备空闲并替换
/// ImGui 光标纹理，禁止放入每帧绘制路径。
void CursorManager::reloadSkinTextures(vk::PhysicalDevice& phyDevice,
                                       vk::Device&         logicalDevice,
                                       vk::CommandPool     commandPool,
                                       vk::Queue           queue)
{
    auto& skin = Config::SkinManager::instance();

    (void)logicalDevice.waitIdle();

    auto cursorTexture =
        std::make_unique<VKTexture>(skin.getAssetPath("cursor"),
                                    phyDevice,
                                    logicalDevice,
                                    commandPool,
                                    queue);
    auto trailTexture =
        std::make_unique<VKTexture>(skin.getAssetPath("cursortrail"),
                                    phyDevice,
                                    logicalDevice,
                                    commandPool,
                                    queue);
    auto smokeTexture =
        std::make_unique<VKTexture>(skin.getAssetPath("cursor_smoke"),
                                    phyDevice,
                                    logicalDevice,
                                    commandPool,
                                    queue);

    m_texCursor = std::move(cursorTexture);
    m_texTrail  = std::move(trailTexture);
    m_texSmoke  = std::move(smokeTexture);
}

/// @brief 更新并绘制软件光标。
/// @param smokeLifeOverride 烟雾存活时间覆盖值，小于等于 0 时使用配置值。
/// @warning UI 热路径：每帧调用；只更新光标粒子缓存并提交固定类型 ImGui
/// 绘制命令。
void CursorManager::UpdateAndDraw(float smokeLifeOverride)
{
    ImGuiIO& io        = ImGui::GetIO();
    ImVec2   mousePos  = io.MousePos;
    float    deltaTime = io.DeltaTime;

    auto& config    = Config::AppConfig::instance();
    float dpiScale  = config.getWindowContentScale();
    auto& cursorCfg = config.getEditorSettings().softwareCursorConfig;

    // 同步配置参数
    float cursorSize    = cursorCfg.cursorSize * dpiScale;
    float trailSize     = cursorCfg.trailSize * dpiScale;
    float trailLifeTime = cursorCfg.trailLifeTime;
    float smokeSize     = cursorCfg.smokeSize * dpiScale;
    float smokeLifeTime = (smokeLifeOverride > 0.0f) ? smokeLifeOverride
                                                     : cursorCfg.smokeLifeTime;

    // 1. 生成新的点
    bool isMoving =
        m_trailPoints.empty() ||
        ImLengthSqr(ImVec2(mousePos.x - m_trailPoints.front().pos.x,
                           mousePos.y - m_trailPoints.front().pos.y)) >
            (m_emitDistance * m_emitDistance);

    if ( isMoving ) {
        m_trailPoints.push_front({ mousePos, 1.0f });
        m_smokePoints.push_front({ mousePos, 1.0f });  // 同时生成烟雾
    }

    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    while ( m_smokePoints.size() > 256 ) m_smokePoints.pop_back();

    // 2. 绘制烟雾 (最底层)
    for ( int i = (int)m_smokePoints.size() - 1; i >= 0; --i ) {
        TrailPoint& p = m_smokePoints[i];
        p.life -= deltaTime / smokeLifeTime;
        if ( p.life <= 0.0f ) {
            m_smokePoints.pop_back();
            continue;
        }

        // 烟雾逻辑：随时间变淡，且缓慢变大
        int   alpha = (int)(p.life * 255.0f * m_smokeOpacity);
        ImU32 color = IM_COL32(255, 255, 255, alpha);

        // 扩张公式：初始大小 * (1.0 + (1.0 - 剩余寿命) * 扩张系数)
        float currentSize =
            smokeSize * (1.0f + (1.0f - p.life) * m_smokeExpansion);
        float hs = currentSize * 0.5f;

        if ( m_texSmoke ) {
            drawList->AddImage(m_texSmoke->getImTextureID(),
                               ImVec2(p.pos.x - hs, p.pos.y - hs),
                               ImVec2(p.pos.x + hs, p.pos.y + hs),
                               ImVec2(0, 0),
                               ImVec2(1, 1),
                               color);
        }
    }

    // 3. 绘制拖尾 (中间层)
    for ( int i = (int)m_trailPoints.size() - 1; i >= 0; --i ) {
        TrailPoint& p = m_trailPoints[i];
        p.life -= deltaTime / trailLifeTime;
        if ( p.life <= 0.0f ) {
            m_trailPoints.pop_back();
            continue;
        }

        int   alpha       = (int)(p.life * 255.0f);
        ImU32 color       = IM_COL32(255, 255, 255, alpha);
        float currentSize = trailSize * (0.3f + 0.7f * p.life);  // 拖尾会收缩
        float hs          = currentSize * 0.5f;

        if ( m_texTrail ) {
            drawList->AddImage(m_texTrail->getImTextureID(),
                               ImVec2(p.pos.x - hs, p.pos.y - hs),
                               ImVec2(p.pos.x + hs, p.pos.y + hs),
                               ImVec2(0, 0),
                               ImVec2(1, 1),
                               color);
        }
    }

    // 4. 绘制主光标头 (最顶层)
    if ( m_texCursor ) {
        float hs = cursorSize * 0.5f;
        drawList->AddImage(m_texCursor->getImTextureID(),
                           ImVec2(mousePos.x - hs, mousePos.y - hs),
                           ImVec2(mousePos.x + hs, mousePos.y + hs));
    }
}

}  // namespace MMM::Graphic
