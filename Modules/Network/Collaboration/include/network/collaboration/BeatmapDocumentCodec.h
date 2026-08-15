#pragma once

#include "mmm/beatmap/BeatmapMutationObserver.h"
#include "network/collaboration/CollaborationTypes.h"

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

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
    /// @brief 可在后台准备、随后常量时间安装的物件编码基线。
    class ObjectEncodingBaseline;

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

    /// @brief 在后台从完整谱面准备玩家物件编码基线。
    /// @param beatmap 已物化且在调用期间保持只读的谱面。
    /// @return 标识合法时返回可移动基线，否则返回空。
    /// @warning 后台任务路径：会完整扫描并编码玩家物件，禁止在逻辑或 UI
    /// 热路径调用。
    [[nodiscard]] static std::shared_ptr<ObjectEncodingBaseline>
    prepareObjectEncodingBaseline(const ::MMM::BeatMap& beatmap);

    /// @brief 安装后台准备好的玩家物件编码基线。
    /// @param baseline 待安装的物件基线。
    /// @warning 逻辑线程远端提交确认路径：只移动已准备容器，不遍历谱面。
    void synchronizeObjectEncodingBaseline(
        std::shared_ptr<ObjectEncodingBaseline> baseline);

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

    /// @brief 计算当前文档相对旧可见文档发生变化的根物件稳定标识。
    /// @param previous 上一次已经交付给逻辑线程的可见文档。
    /// @return 标识完整且唯一时返回增删改标识；否则返回空并要求完整替换。
    /// @warning 协作后台合并路径调用；只比较内存 JSON，不物化领域对象。
    [[nodiscard]] std::optional<std::vector<std::string>>
    changedObjectIdentitiesComparedTo(
        const BeatmapDocumentCodec& previous) const;

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
