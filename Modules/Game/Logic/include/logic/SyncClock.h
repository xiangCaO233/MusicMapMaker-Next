#pragma once
#include "config/EditorConfig.h"
#include <cmath>

namespace MMM::Logic
{

/**
 * @brief 音画同步时钟
 *
 * 核心设计：
 * 1. 平滑步进 (advance)：每帧根据当前播放速度进行自由推演。
 * 2. 周期校准 (sync)：在特定周期到达时，根据音频硬件时间进行误差修正。
 */
class SyncClock
{
public:
    /**
     * @brief 平滑步进时钟（每帧调用）
     * @param dt 帧间隔 (秒)
     * @param playbackSpeed 当前播放速度倍率
     */
    void advance(double dt, float playbackSpeed)
    {
        m_visualTime += dt * static_cast<double>(playbackSpeed);
    }

    /**
     * @brief 与音频源强制校准（每 syncInterval 调用）
     * @param audioTime 当前音频硬件播放时间 (秒)
     * @param config 同步配置
     */
    void sync(double audioTime, const Config::SyncConfig& config)
    {
        double error = audioTime - m_visualTime;

        // 如果误差超过 0.5s，说明发生了大幅度跳转 (Seek)，直接重置视觉时间
        if ( std::abs(error) > 0.5 ) {
            m_visualTime = audioTime;
            return;
        }

        // 死区 (Deadzone)：如果误差小于 2ms，我们认为内部高精度的 advance(dt)
        // 是完全准确的 从而拒绝这种微小扰动带来的渲染抖动
        if ( std::abs(error) < 0.002 ) {
            return;
        }

        switch ( config.mode ) {
        case Config::SyncMode::None: m_visualTime = audioTime; break;
        case Config::SyncMode::Integral:
            // 积分制：误差乘以系数进行微调。系数越小，追赶越平滑，但响应越慢。
            m_visualTime += error * static_cast<double>(config.integralFactor);
            break;
        case Config::SyncMode::WaterTank:
            // 水箱制：维持一个固定的延迟缓冲区，牺牲绝对延迟换取极高稳定性。
            m_visualTime =
                audioTime - static_cast<double>(config.waterTankBuffer);
            break;
        }
    }

    /**
     * @brief 完全重置时钟
     * @param time 目标时间 (秒)
     */
    void reset(double time) { m_visualTime = time; }

    /**
     * @brief 获取当前视觉渲染时间
     */
    [[nodiscard]] double getVisualTime() const { return m_visualTime; }

private:
    double m_visualTime{ 0 };
};

}  // namespace MMM::Logic
