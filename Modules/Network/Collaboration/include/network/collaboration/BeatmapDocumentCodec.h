#pragma once

#include "mmm/beatmap/BeatmapMutationObserver.h"
#include "network/collaboration/CollaborationTypes.h"

#include <expected>
#include <memory>
#include <span>

namespace MMM
{
class BeatMap;
}

namespace MMM::Network::Collaboration
{
/// @brief 协作谱面文档编解码错误。
enum class BeatmapDocumentError : std::uint8_t {
    EmptyPayload,
    InvalidPayload,
    MissingSnapshot,
    InvalidDocument,
};

/// @brief 成功应用增量后的文档变化信息。
struct BeatmapPatchResult {
    /// @brief 本次负载实际覆盖的数据类别。
    ::MMM::BeatmapMutationFlags flags{ ::MMM::BeatmapMutationFlags::None };
    /// @brief 本次负载是否为可独立恢复的完整快照。
    bool isSnapshot{ false };
};

/// @brief 把 BeatMap 转换为与进程内 ECS 无关的快照和分类增量。
class BeatmapDocumentCodec
{
public:
    /// @brief 创建空协作文档。
    BeatmapDocumentCodec();
    /// @brief 释放内部 JSON 文档状态。
    ~BeatmapDocumentCodec();

    BeatmapDocumentCodec(const BeatmapDocumentCodec&)            = delete;
    BeatmapDocumentCodec& operator=(const BeatmapDocumentCodec&) = delete;
    BeatmapDocumentCodec(BeatmapDocumentCodec&&)                 = delete;
    BeatmapDocumentCodec& operator=(BeatmapDocumentCodec&&)      = delete;

    /// @brief 编码完整快照或只包含变化类别的增量负载。
    /// @param beatmap 当前完整谱面。
    /// @param flags 增量包含的数据类别；快照模式会自动覆盖全部类别。
    /// @param snapshot 是否编码完整快照。
    /// @return 成功时返回 CBOR 二进制负载。
    [[nodiscard]] std::expected<ByteBuffer, BeatmapDocumentError> encode(
        const ::MMM::BeatMap& beatmap, ::MMM::BeatmapMutationFlags flags,
        bool snapshot);

    /// @brief 将本地增量编码基线同步为逻辑线程当前实际谱面。
    /// @param beatmap 已完成远端权威合并后的本地谱面。
    /// @warning 只能由持有该编码器外部同步锁的低频谱面同步路径调用。
    void synchronizeEncodingBaseline(const ::MMM::BeatMap& beatmap);

    /// @brief 将房主已排序的快照或分类增量应用到本地规范文档。
    /// @param payload CBOR 二进制负载。
    /// @return 成功时返回负载类别。
    [[nodiscard]] std::expected<BeatmapPatchResult, BeatmapDocumentError> apply(
        std::span<const std::uint8_t> payload);

    /// @brief 不修改本地文档地校验负载并提取其实际数据类别。
    /// @param payload 待授权的快照或分类增量负载。
    /// @return 负载结构合法时返回类别与快照标志。
    /// @warning 房主收到低频编辑请求时调用；会执行有界内存解压和 CBOR
    /// 解析，但不会物化 BeatMap 或修改编解码基线。
    [[nodiscard]] static std::expected<BeatmapPatchResult, BeatmapDocumentError>
    inspect(std::span<const std::uint8_t> payload);

    /// @brief 从当前规范文档重建可交给逻辑层的 BeatMap。
    /// @return 尚未收到完整快照或文档不合法时返回空。
    [[nodiscard]] std::shared_ptr<::MMM::BeatMap> materialize() const;

    /// @brief 复制当前规范文档供后台重放本地待确认增量。
    /// @return 尚未收到完整快照时返回空。
    /// @warning 协作后台合并路径调用；只复制内存文档，不执行 CBOR 编解码或
    /// 压缩，仍应仅在确实存在待重放增量时使用。
    [[nodiscard]] std::unique_ptr<BeatmapDocumentCodec> cloneDocument() const;

    /// @brief 把当前规范文档重新编码为完整快照。
    /// @return 尚未收到完整快照时返回 MissingSnapshot。
    [[nodiscard]] std::expected<ByteBuffer, BeatmapDocumentError>
    encodeCurrentSnapshot() const;

    /// @brief 查询当前是否已经收到可独立恢复的完整快照。
    [[nodiscard]] bool hasDocument() const;

    /// @brief 清空当前规范文档。
    void reset();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
}  // namespace MMM::Network::Collaboration
