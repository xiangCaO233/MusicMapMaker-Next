#pragma once

#include "common/LogicCommands.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/IUIView.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace MMM::UI
{

class NewBeatmapWizard : public IUIView
{
public:
    /// @brief 构造新建谱面向导。
    NewBeatmapWizard();

    /// @brief 销毁新建谱面向导。
    virtual ~NewBeatmapWizard() = default;

    /// @brief 更新并绘制新建谱面向导弹窗。
    /// @param sourceManager 当前 UI 管理器。
    void update(UIManager* sourceManager) override;

    /// @brief 打开向导并重置输入状态。
    void open();

    /// @brief 关闭向导弹窗。
    void close();

private:
    /// @brief 新谱面的创建来源。
    enum class CreateMode {
        Blank,        ///< 创建空白谱面。
        OpenTemplate  ///< 从已打开谱面作为模板创建。
    };

    /// @brief 可作为模板的已打开谱面条目。
    struct OpenTemplateOption {
        /// @brief Session 在引擎注册表中的索引。
        int32_t sessionIndex{ -1 };

        /// @brief 模板来源画布 ID。
        std::string cameraId;

        /// @brief 模板来源画布显示名称。
        std::string displayName;

        /// @brief 模板谱面的内部名称。
        std::string internalName;

        /// @brief 模板谱面的项目内路径。
        std::filesystem::path mapPath;

        /// @brief 模板谱面对象。
        std::shared_ptr<MMM::BeatMap> beatmap;
    };

    /// @brief 重置向导字段到默认值。
    void reset();

    /// @brief 处理音频资源选择并读取音频元数据。
    /// @param path 项目内音频相对路径。
    void onAudioSelected(const std::filesystem::path& path);

    /// @brief 绘制模板创建来源选择区域。
    /// @param templateOptions 当前已打开且可作为模板的谱面列表。
    void renderTemplateSourceControls(
        const std::vector<OpenTemplateOption>& templateOptions);

    /// @brief 绘制已打开谱面的模板选择弹窗。
    /// @param templateOptions 当前已打开且可作为模板的谱面列表。
    void renderTemplatePickerPopup(
        const std::vector<OpenTemplateOption>& templateOptions);

    /// @brief 绘制模板内容复制选项弹窗。
    void renderTemplateOptionsPopup();

    /// @brief 绘制内部名称冲突警告弹窗。
    void renderDuplicateNameWarningPopup();

    /// @brief 收集已打开且可作为模板的谱面列表。
    /// @return 可选模板谱面列表。
    std::vector<OpenTemplateOption> collectOpenTemplateOptions() const;

    /// @brief 查找当前选中的模板条目。
    /// @param templateOptions 当前可选模板谱面列表。
    /// @return 找到时返回模板条目指针；否则返回 nullptr。
    const OpenTemplateOption* findSelectedTemplate(
        const std::vector<OpenTemplateOption>& templateOptions) const;

    /// @brief 选择指定模板谱面并应用资源默认值。
    /// @param option 用户选中的模板条目。
    void selectTemplate(const OpenTemplateOption& option);

    /// @brief 将模板谱面的资源与节奏基础值应用为新谱面默认值。
    /// @param beatmap 模板谱面。
    void applyTemplateResourceDefaults(const MMM::BeatMap& beatmap);

    /// @brief 在创建前将选中模板的 ECS 脏数据同步回 BeatMap。
    void syncSelectedTemplateBeatmap();

    /// @brief 根据项目资源列表解析音频路径对应的音轨 ID。
    /// @param path 待匹配的项目内音频路径。
    /// @return 匹配到的音轨 ID；未匹配时返回空字符串。
    std::string findAudioTrackIdForPath(
        const std::filesystem::path& path) const;

    /// @brief 将输入框状态同步到待创建谱面的基础元数据。
    void syncMetaFromInputs();

    /// @brief 判断当前内部名称是否已存在于项目谱面列表。
    /// @return 已存在相同内部名称时返回 true。
    bool hasInternalNameConflict() const;

    /// @brief 提交新建谱面命令。
    void submitCreateRequest();

    /// @brief 当前向导弹窗是否打开。
    bool m_isOpen = false;
    /// @brief 下一帧是否需要打开弹窗。
    bool m_shouldOpen = false;
    /// @brief 待创建谱面的基础元数据。
    MMM::BaseMapMeta m_meta;

    /// @brief 当前选择的新谱面创建方式。
    CreateMode m_createMode{ CreateMode::Blank };

    /// @brief 选中的模板来源画布 ID。
    std::string m_templateCameraId;

    /// @brief 选中的模板显示名称。
    std::string m_templateDisplayName;

    /// @brief 选中的模板谱面对象。
    std::shared_ptr<MMM::BeatMap> m_templateBeatmap;

    /// @brief 模板数据复制选项。
    Logic::BeatmapTemplateCreateOptions m_templateOptions;

    /// @brief 下一帧是否打开模板选择弹窗。
    bool m_shouldOpenTemplatePicker{ false };

    /// @brief 下一帧是否打开模板内容选项弹窗。
    bool m_shouldOpenTemplateOptions{ false };


    /// @brief 谱面内部名称输入缓冲区。
    char m_nameBuf[256] = { 0 };
    /// @brief 标题输入缓冲区。
    char m_titleBuf[256] = { 0 };
    /// @brief Unicode 标题输入缓冲区。
    char m_titleUnicodeBuf[256] = { 0 };
    /// @brief 艺术家输入缓冲区。
    char m_artistBuf[256] = { 0 };
    /// @brief Unicode 艺术家输入缓冲区。
    char m_artistUnicodeBuf[256] = { 0 };
    /// @brief 谱师输入缓冲区。
    char m_authorBuf[256] = { 0 };
    /// @brief 难度名输入缓冲区。
    char m_versionBuf[256] = { 0 };

    /// @brief 谱面默认 BPM。
    double m_bpm = 120.0;
    /// @brief 谱面轨道数。
    int m_trackCount = 4;
    /// @brief 选中的主音频相对路径。
    std::filesystem::path m_selectedAudioPath;
    /// @brief 选中的主音频资源 ID。
    std::string m_selectedAudioTrackId;
    /// @brief 选中的背景资源相对路径。
    std::filesystem::path m_selectedCoverPath;
    /// @brief 选中的封面图片路径
    std::filesystem::path m_selectedCoverImgPath;
    /// @brief 当前选中音频时长，单位为秒。
    double m_audioDuration = 0.0;
};

}  // namespace MMM::UI
