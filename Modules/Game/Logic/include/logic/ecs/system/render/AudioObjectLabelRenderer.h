#pragma once

#include <glm/glm.hpp>

#include <string_view>

namespace MMM::Logic::System
{

struct Batcher;

/// @brief 获取音频物件标签使用的皮肤颜色。
/// @return `bgm_tracks.text` 颜色；皮肤未配置时返回内置浅色。
/// @warning 主画布热路径：只允许读取已加载皮肤缓存，不得访问文件系统。
glm::vec4 audioObjectLabelColor();

/// @brief 在固定水平裁剪范围内绘制单行 ASCII 文本。
/// @param batcher 目标批处理器。
/// @param text 文本。
/// @param x 文本区域左边界。
/// @param y 文本区域上边界。
/// @param fontPixelHeight 字体像素高度。
/// @param maxWidth 最大可用宽度。
/// @param color 文字颜色。
/// @param centerHorizontally 文本可完整显示时是否水平居中。
/// @warning 主画布热路径：只处理有界短文本，不得加载字体或分配 GPU 资源。
void renderCanvasAsciiText(Batcher& batcher, std::string_view text, float x,
                           float y, float fontPixelHeight, float maxWidth,
                           glm::vec4 color, bool centerHorizontally = false);

/// @brief 在固定水平裁剪范围内绘制可循环滚动的单行 ASCII 文本。
/// @param batcher 目标批处理器。
/// @param text 文本。
/// @param x 文本区域左边界。
/// @param y 文本区域上边界。
/// @param fontPixelHeight 字体像素高度。
/// @param maxWidth 最大可用宽度。
/// @param color 文字颜色。
/// @param monotonicSeconds 单调时钟秒数。
/// @param centerHorizontally 文本可完整显示时是否水平居中。
/// @warning 主画布热路径：只绘制至多两份有界短文本，不得分配堆内存、
/// 创建独立裁剪命令或访问文件系统。
void renderMarqueeCanvasAsciiText(Batcher& batcher, std::string_view text,
                                  float x, float y, float fontPixelHeight,
                                  float maxWidth, glm::vec4 color,
                                  double monotonicSeconds,
                                  bool   centerHorizontally = false);

/// @brief 绘制与自动采样一致的音频资源标签。
/// @param batcher 目标批处理器。
/// @param audioResourceId 音频资源标识。
/// @param volume 物件音量。
/// @param laneLeftX 当前轨道左边界。
/// @param objectTopY 物件本体上边界。
/// @param laneWidth 当前轨道宽度。
/// @param objectScaleY 物件纵向缩放，用于同步字号。
/// @param color 文字颜色。
/// @param monotonicSeconds 单调时钟秒数。
/// @warning 主画布热路径：只绘制至多两份有界短文本，不得分配堆内存、
/// 创建独立裁剪命令或访问文件系统。
void renderAudioObjectLabel(Batcher& batcher, std::string_view audioResourceId,
                            float volume, float laneLeftX, float objectTopY,
                            float laneWidth, float objectScaleY,
                            glm::vec4 color, double monotonicSeconds);

}  // namespace MMM::Logic::System
