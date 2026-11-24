#pragma once

#include "config/StagedFile.h"

namespace MMM::Internal
{

/// @brief 谱面写出器使用与配置相同的同卷原子提交机制。
/// @details
/// 非谱面后缀避免工程扫描发现暂存内容。资源引用的计算始终使用正式路径。 Android
/// 对授权文档的发布由外层保存入口确认；本地提交只完成镜像文件写入。
using BeatmapOutputFile = Config::StagedOutputFile;

}  // namespace MMM::Internal
