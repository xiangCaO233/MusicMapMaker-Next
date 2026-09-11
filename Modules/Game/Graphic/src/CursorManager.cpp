#include "graphic/CursorManager.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "graphic/imguivk/VKTexture.h"
#include "imgui_internal.h"

#include <algorithm>

/// @file
/// @brief 实现软件光标皮肤资源切换和基于 ImGui 前景层的粒子绘制。
///
/// 低频重载阶段负责 Vulkan 纹理创建；每帧阶段只读取已上传纹理、更新有限粒子队列
/// 并提交 ImGui 绘制命令，从而避免把文件系统或 GPU 资源创建带入 UI 热路径。

namespace MMM::Graphic
{
namespace
{
/// @brief 鼠标按下时软件光标整体缩小倍率。
constexpr float CURSOR_PRESSED_SCALE = 0.92f;

/// @brief 光标点击动画相对全局 UI 过渡速度的倍率。
constexpr float CURSOR_CLICK_TRANSITION_SPEED_MULTIPLIER = 10.0f;

/// @brief 计算 ease-out cubic 缓动值。
/// @param value 线性进度。
/// @return 缓动后的进度。
float easeOutCubic(float value)
{
    // 输入先约束到标准进度范围，避免异常帧间隔放大动画结果。
    const float t   = std::clamp(value, 0.0f, 1.0f);
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

/// @brief 向目标值推进光标按压动画进度。
/// @param current 当前进度。
/// @param target 目标进度。
/// @param deltaTime 本帧间隔时间。
/// @param speed 每秒推进速度。
/// @return 推进后的进度。
/// @warning UI 热路径：每帧只进行常量时间浮点计算。
float approachPressAmount(float current, float target, float deltaTime,
                          float speed)
{
    // 负帧间隔或负速度均按零处理，使状态只朝目标单调推进。
    const float step = std::max(0.0f, deltaTime) * std::max(0.0f, speed);
    if ( current < target ) {
        // 上升分支夹到目标值，避免大步长越过按下状态。
        return std::min(target, current + step);
    }
    // 释放分支同样夹到目标值，保证归一化进度不会反向越界。
    return std::max(target, current - step);
}

}  // namespace

/// @brief 构造管理器并同步准备当前皮肤的三类光标纹理。
/// @param phyDevice Vulkan 物理设备。
/// @param logicalDevice Vulkan 逻辑设备。
/// @param commandPool 纹理上传命令池。
/// @param queue 纹理上传队列。
/// @warning 构造会同步上传皮肤纹理并等待设备空闲，只能在初始化路径执行。
CursorManager::CursorManager(vk::PhysicalDevice& phyDevice,
                             vk::Device&         logicalDevice,
                             vk::CommandPool commandPool, vk::Queue queue)
{
    // 构造结束后对象即可直接进入每帧绘制，不暴露半初始化纹理状态。
    reloadSkinTextures(phyDevice, logicalDevice, commandPool, queue);
}

/// @brief 在 VKTexture 完整类型可见处销毁所有软件光标纹理。
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
    // 皮肤管理器统一解析当前主题中的逻辑资源名称。
    auto& skin = Config::SkinManager::instance();

    // 旧纹理可能仍被已提交命令引用，替换前必须等待设备完成访问。
    (void)logicalDevice.waitIdle();

    // 三类候选纹理先在局部对象中完整创建，避免逐成员替换期间混用新旧皮肤。
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

    // 所有构造步骤完成后再一次性发布到每帧绘制所读取的成员。
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
    // 鼠标位置和帧间隔来自同一 ImGui 帧，保持粒子积分时基一致。
    ImGuiIO& io        = ImGui::GetIO();
    ImVec2   mousePos  = io.MousePos;
    float    deltaTime = io.DeltaTime;

    // 尺寸随窗口内容缩放，生命周期保持秒单位，不受 DPI 影响。
    auto&       config    = Config::AppConfig::instance();
    float       dpiScale  = config.getWindowContentScale();
    auto&       cursorCfg = config.getEditorSettings().softwareCursorConfig;
    const float pressTarget =
        ImGui::IsMouseDown(ImGuiMouseButton_Left) ? 1.0f : 0.0f;
    // 按压状态逐帧逼近目标，避免点击时光标尺寸瞬变。
    m_pressAmount = approachPressAmount(
        m_pressAmount,
        pressTarget,
        deltaTime,
        config.getEditorSettings().aesthetics.animationTransitionSpeed() *
            CURSOR_CLICK_TRANSITION_SPEED_MULTIPLIER);
    const float pressScale =
        1.0f - (1.0f - CURSOR_PRESSED_SCALE) * easeOutCubic(m_pressAmount);

    // 每帧读取可热更新配置，并把视觉尺寸统一换算为当前屏幕像素。
    float cursorSize    = cursorCfg.cursorSize * dpiScale * pressScale;
    float trailSize     = cursorCfg.trailSize * dpiScale * pressScale;
    float trailLifeTime = cursorCfg.trailLifeTime;
    float smokeSize     = cursorCfg.smokeSize * dpiScale * pressScale;
    float smokeLifeTime = (smokeLifeOverride > 0.0f) ? smokeLifeOverride
                                                     : cursorCfg.smokeLifeTime;

    // 只有离最新采样点达到阈值才发射粒子，限制静止光标的无效增长。
    bool isMoving =
        m_trailPoints.empty() ||
        ImLengthSqr(ImVec2(mousePos.x - m_trailPoints.front().pos.x,
                           mousePos.y - m_trailPoints.front().pos.y)) >
            (m_emitDistance * m_emitDistance);

    if ( isMoving ) {
        // 队首固定为最新点，反向绘制时自然形成从旧到新的覆盖顺序。
        m_trailPoints.push_front({ mousePos, 1.0f });
        // 烟雾与拖尾共享发射位置，但之后使用独立寿命和尺寸曲线。
        m_smokePoints.push_front({ mousePos, 1.0f });
    }

    // ImGui 为每个 viewport 维护独立前景层，光标必须提交到鼠标所在视口。
    ImGuiViewport* cursorViewport =
        ImGui::FindViewportByID(io.MouseHoveredViewport);
    // 后端尚未报告悬浮视口时退回主视口，保证光标仍有稳定绘制目标。
    if ( !cursorViewport ) cursorViewport = ImGui::GetMainViewport();
    ImDrawList* drawList = ImGui::GetForegroundDrawList(cursorViewport);

    // 烟雾上限保护长寿命配置下的队列规模，优先丢弃最旧粒子。
    while ( m_smokePoints.size() > 256 ) m_smokePoints.pop_back();

    // 从最旧粒子向最新粒子绘制，使新烟雾自然覆盖衰减尾部。
    for ( int i = (int)m_smokePoints.size() - 1; i >= 0; --i ) {
        TrailPoint& p = m_smokePoints[i];
        // life 使用归一化比例，寿命配置只影响每帧递减速度。
        p.life -= deltaTime / smokeLifeTime;
        if ( p.life <= 0.0f ) {
            // 逆序遍历时过期项位于队尾，可以常量时间移除而不使索引失效。
            m_smokePoints.pop_back();
            continue;
        }

        // 透明度随剩余寿命线性衰减，基础不透明度避免烟雾遮挡画布。
        int   alpha = (int)(p.life * 255.0f * m_smokeOpacity);
        ImU32 color = IM_COL32(255, 255, 255, alpha);

        // 已消耗寿命驱动扩张，使烟雾在淡出过程中逐渐变大。
        float currentSize =
            smokeSize * (1.0f + (1.0f - p.life) * m_smokeExpansion);
        float hs = currentSize * 0.5f;

        if ( m_texSmoke ) {
            // 纹理以粒子位置为中心绘制，颜色只调制整体 Alpha。
            drawList->AddImage(m_texSmoke->getImTextureID(),
                               ImVec2(p.pos.x - hs, p.pos.y - hs),
                               ImVec2(p.pos.x + hs, p.pos.y + hs),
                               ImVec2(0, 0),
                               ImVec2(1, 1),
                               color);
        }
    }

    // 拖尾在烟雾之后绘制，保持清晰轮廓而不被低透明烟雾覆盖。
    for ( int i = (int)m_trailPoints.size() - 1; i >= 0; --i ) {
        TrailPoint& p = m_trailPoints[i];
        p.life -= deltaTime / trailLifeTime;
        if ( p.life <= 0.0f ) {
            // 采样点按时间排序，过期点连续集中在队尾。
            m_trailPoints.pop_back();
            continue;
        }

        int   alpha = (int)(p.life * 255.0f);
        ImU32 color = IM_COL32(255, 255, 255, alpha);
        // 拖尾随寿命衰减至初始尺寸的 30%，避免尾端突然消失。
        float currentSize = trailSize * (0.3f + 0.7f * p.life);
        float hs          = currentSize * 0.5f;

        if ( m_texTrail ) {
            // 与烟雾共用中心锚定方式，保证按压缩放时三层视觉一致。
            drawList->AddImage(m_texTrail->getImTextureID(),
                               ImVec2(p.pos.x - hs, p.pos.y - hs),
                               ImVec2(p.pos.x + hs, p.pos.y + hs),
                               ImVec2(0, 0),
                               ImVec2(1, 1),
                               color);
        }
    }

    // 主光标最后绘制，确保任何拖尾粒子都不会盖住当前指针位置。
    if ( m_texCursor ) {
        // 使用半边长构造中心对齐矩形，使纹理中心严格跟随鼠标热点。
        float hs = cursorSize * 0.5f;
        drawList->AddImage(m_texCursor->getImTextureID(),
                           ImVec2(mousePos.x - hs, mousePos.y - hs),
                           ImVec2(mousePos.x + hs, mousePos.y + hs));
    }
}

}  // namespace MMM::Graphic
