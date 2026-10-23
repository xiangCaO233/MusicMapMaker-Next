#include "config/AppConfig.h"
#include "ConfigSnapshotWriter.h"
#include "config/AppPaths.h"
#include "config/AudioPresets.h"
#include "config/CreatorIdentity.h"
#include "config/LayoutPresets.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iomanip>

namespace MMM::Config
{
/// @brief 两类客户端预设的长期存储，不参与实时配置字段访问。
struct AppConfig::PresetState {
    /// @brief 独立于项目快照，防止切换项目覆盖个人听音方案。
    AudioPresets m_audioPresets;
    /// @brief 客户端布局集合，保存时捕获当前实时布局。
    // const 保存入口可更新派生的预设身份，但不会改变实时视觉布局。
    mutable LayoutPresets m_layoutPresets;
};


/// @brief 获取进程级应用配置管理器。
/// @return 生命周期覆盖应用主流程的唯一配置实例。
AppConfig& AppConfig::instance()
{
    // C++ 保证函数内静态对象只初始化一次，所有模块共享同一配置状态。
    static AppConfig instance;
    return instance;
}

/// @brief 创建默认编辑器配置并生成本机协作者稳定标识。
AppConfig::AppConfig()
    : m_presets(std::make_unique<PresetState>())
    , m_collaborationParticipantId(makeCollaborationStableId())
{
    // reset 只覆盖 EditorConfig，不会清除构造列表刚生成的稳定标识。
    reset();
}


/// @brief 在完整类型可见处释放预设库。
AppConfig::~AppConfig() = default;

/// @brief 借用随单例存活的预设集合，不复制库或转移所有权。
/// @return 原预设对象引用，生命周期由配置单例保证。
/// @warning 界面可逐帧访问；只解引用稳定私有状态，不分配或加锁。
AudioPresets& AppConfig::getAudioPresets()
{
    return m_presets->m_audioPresets;
}

/// @brief 借用随单例存活的预设集合，不复制库或转移所有权。
/// @return 原预设对象引用，生命周期由配置单例保证。
/// @warning 界面可逐帧访问；只解引用稳定私有状态，不分配或加锁。
const AudioPresets& AppConfig::getAudioPresets() const
{
    return m_presets->m_audioPresets;
}

/// @brief 借用随单例存活的预设集合，不复制库或转移所有权。
/// @return 原预设对象引用，生命周期由配置单例保证。
/// @warning 界面可逐帧访问；只解引用稳定私有状态，不分配或加锁。
LayoutPresets& AppConfig::getLayoutPresets()
{
    return m_presets->m_layoutPresets;
}

/// @brief 借用随单例存活的预设集合，不复制库或转移所有权。
/// @return 原预设对象引用，生命周期由配置单例保证。
/// @warning 界面可逐帧访问；只解引用稳定私有状态，不分配或加锁。
const LayoutPresets& AppConfig::getLayoutPresets() const
{
    return m_presets->m_layoutPresets;
}

/// @brief 将编辑器设置恢复到类型定义的默认值。
/// @warning 低频配置路径：获取互斥锁，只能在初始化或显式重置时调用。
void AppConfig::reset()
{
    // 整体替换保证新增字段自动采用 EditorConfig 的成员默认值。
    std::lock_guard<std::mutex> lock(m_mutex);
    m_editorConfig             = EditorConfig();
    m_presets->m_audioPresets  = AudioPresets();
    m_presets->m_layoutPresets = LayoutPresets();
}

/// @brief 从指定或默认 JSON 文件加载编辑器配置与协作者稳定标识。
/// @param path 显式配置路径；为空时使用 AppPaths 的用户配置文件。
/// @return 文件存在、可读且能解析为配置对象时返回 true。
/// @warning 低频文件系统路径：执行存在性检查、JSON 解析并可能迁移旧配置。
bool AppConfig::load(const std::filesystem::path& path)
{
    // 只有默认路径允许查找旧版文件并在成功后自动迁移。
    bool useDefaultPath = path.empty();
    // 显式路径通常用于测试或导入，必须严格按调用方目标读取。
    std::filesystem::path finalPath =
        useDefaultPath ? getDefaultConfigPath() : path;

    // error_code 版本把权限和路径错误留在可恢复启动流程中。
    std::error_code configExistsError;
    if ( !std::filesystem::exists(finalPath, configExistsError) &&
         useDefaultPath ) {
        // 新路径查询错误也进入旧路径探测，最终存在性检查会给出统一结果。
        // 仅在新默认文件缺失时读取旧工作目录布局，避免覆盖已迁移配置。
        std::filesystem::path legacyPath = AppPaths::legacyUserConfigFilePath();
        // 旧路径查询使用独立错误码，不与新路径失败状态混用。
        std::error_code legacyExistsError;
        if ( std::filesystem::exists(legacyPath, legacyExistsError) ) {
            // 先从旧文件恢复内存，函数末尾再保存到当前默认位置。
            finalPath = legacyPath;
            XINFO("Using legacy config for migration: {}",
                  pathToUtf8(finalPath));
        }
        // 旧路径错误或缺失时保留新默认路径，由下方缺失分支使用默认值。
    }

    // 默认和显式路径统一在这里验证，缺失时保留当前内存默认值。
    std::error_code finalExistsError;
    if ( !std::filesystem::exists(finalPath, finalExistsError) ) {
        // 不调用 reset，允许调用方在加载失败时继续使用当前内存配置。
        XINFO("Config file not found: {}. Using default values.",
              pathToUtf8(finalPath));
        return false;
    }

    // 流对象生命周期只覆盖本次解析，不在持锁区执行磁盘读取。
    std::ifstream file(finalPath);
    if ( !file.is_open() ) {
        // 存在但不可读与缺失分开记录，便于定位权限和文件占用问题。
        XERROR("Failed to open config file for reading: {}",
               pathToUtf8(finalPath));
        return false;
    }

    // 禁用异常解析；语法错误、非对象根或底层 I/O 错误都按加载失败处理。
    nlohmann::json j = nlohmann::json::parse(file, nullptr, false);
    if ( j.is_discarded() || !j.is_object() || file.bad() ) {
        // 根数组和标量不符合 EditorConfig 契约，不能交给 from_json 部分恢复。
        XERROR("Failed to parse config file: {}", pathToUtf8(finalPath));
        return false;
    }

    // 协作者标识独立于 EditorConfig 序列化，先以可选字符串读取并规范化。
    std::string serializedCollaborationParticipantId;
    if ( const auto identity = j.find("collaborationParticipantId");
         identity != j.end() && identity->is_string() ) {
        // 非字符串类型视同旧版缺失字段，不尝试隐式 JSON 数值转换。
        serializedCollaborationParticipantId = identity->get<std::string>();
    }
    // 规范化统一大小写并拒绝旧文件中的损坏长度或非十六进制字符。
    const auto collaborationParticipantId =
        normalizeCollaborationStableId(serializedCollaborationParticipantId);
    // 记录是否需要补写新字段，避免加载后再次从 JSON 推断迁移条件。
    const bool generatedCollaborationParticipantId =
        collaborationParticipantId.empty();
    bool layoutMigrated = false;
    {
        // JSON 转换和身份切换作为单个配置状态提交，读取方不会看到半更新值。
        std::lock_guard<std::mutex> lock(m_mutex);
        // EditorConfig 的 from_json 自行处理缺失字段与范围约束。
        m_editorConfig = j.get<EditorConfig>();
        // 升级时把现有全部键数布局收为默认项，不覆盖用户已有编辑结果。
        m_presets->m_layoutPresets = LayoutPresets();

        if ( const auto presets = j.find("layoutPresets");
             presets != j.end() ) {
            // 损坏的预设集合保留刚捕获的旧布局，避免启动时重置现场。
            from_json(*presets, m_presets->m_layoutPresets);
            // 已存在的用户方案以自身完整内容恢复；默认现场仍须检测旧版改动。
            if ( m_presets->m_layoutPresets.m_selected != "默认" )
                applyLayoutPreset(m_presets->m_layoutPresets.current(),
                                  m_editorConfig.visual);
        }
        // 旧版本仅有 visual：实际布局不同才创建用户项，未修改仍使用默认。
        m_presets->m_layoutPresets.captureCurrent(m_editorConfig.visual);
        layoutMigrated =
            !j.contains("layoutPresets") ||
            j["layoutPresets"] != nlohmann::json(m_presets->m_layoutPresets);
        // 旧配置迁移时保留用户已有音量，新配置以所选预设为准。
        m_presets->m_audioPresets = AudioPresets();
        if ( const auto presets = j.find("audioPresets"); presets != j.end() ) {
            m_presets->m_audioPresets = presets->get<AudioPresets>();
            applyAudioPreset(m_presets->m_audioPresets.current(),
                             m_editorConfig.settings);
        } else {
            captureAudioPreset(m_presets->m_audioPresets.current(),
                               m_editorConfig.settings);
            // 迁移后的实际分类音量也受新范围约束，避免显示与听感不一致。
            applyAudioPreset(m_presets->m_audioPresets.current(),
                             m_editorConfig.settings);
        }
        if ( !generatedCollaborationParticipantId ) {
            // 有效持久化身份优先，确保跨启动与协作重连保持一致。
            m_collaborationParticipantId = collaborationParticipantId;
        } else if ( m_collaborationParticipantId.empty() ) {
            // 构造期通常已有身份；该分支防御未来清空状态后加载旧配置。
            m_collaborationParticipantId = makeCollaborationStableId();
        }
    }

    XINFO("Config loaded successfully from: {}", pathToUtf8(finalPath));
    // 旧路径或缺失身份需要迁移到当前默认文件；显式测试路径不自动改写。
    if ( useDefaultPath && (finalPath != getDefaultConfigPath() ||
                            generatedCollaborationParticipantId) ) {
        // save 失败已自行记录日志，已成功加载的内存配置仍可供本次会话使用。
        save();
    } else if ( layoutMigrated ) {
        // 首次迁移立即持久化到原配置路径，下一次启动不重复建立用户项。
        save(finalPath);
    }
    return true;
}

/// @brief 将当前配置快照写入指定或默认 JSON 文件。
/// @param path 显式输出路径；为空时使用 AppPaths 的用户配置文件。
/// @return 目录准备、临时文件写入和最终替换均成功时返回 true。
/// @warning 低频文件系统路径：执行目录创建和文件替换，不得从热路径调用。
bool AppConfig::save(const std::filesystem::path& path) const
{
    return saveConfigSnapshot(path.empty() ? getDefaultConfigPath() : path,
                              snapshotForSave());
}

/// @brief 从 UI 明确修改边沿收存当前方案，并异步保存最新完整配置。
/// @warning 仅在布局实际修改时调用；磁盘写入交给后台，空闲帧不调用。
/// @note 创建用户项和选中在本线程立即完成，写盘期间可继续拖动。
void AppConfig::saveLayoutChanges(const std::filesystem::path& path)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if ( !m_presets->m_layoutPresets.captureCurrent(m_editorConfig.visual) )
            return;
    }
    // 队列覆盖尚未写出的旧快照，绝不让旧请求覆盖新布局。
    queueConfigSnapshot(path.empty() ? getDefaultConfigPath() : path,
                        snapshotForSave());
}

/// @brief 生成配置的不可变保存快照，工作线程不读取实时配置引用。
/// @return 含客户端预设集合的完整持久化对象。
/// @warning 仅由低频保存或明确布局修改调用，不在空闲帧构造 JSON。
nlohmann::json AppConfig::snapshotForSave() const
{
    // 先在锁内生成完整 JSON 值快照，磁盘写入期间不阻塞配置读取。
    nlohmann::json j;
    {
        // 编辑器配置与稳定身份同时复制，磁盘文件不会混合两个时刻的值。
        std::lock_guard<std::mutex> lock(m_mutex);
        j = m_editorConfig;
        // 保存快照收取当前调整，只更新所选项，其他预设保持原样。
        // 音频使用副本收取混音；布局同步更新派生选择，保护代码默认基准。
        // 活动设置是音量的最新来源，保存前不能只序列化库内旧值。
        // 效果器字段不在 EditorSettings 中，因此捕获函数必须保留它们。
        // 布局集合留在客户端根节点，不写入谱面项目或导出数据。
        // 保存也是旧调用入口的兜底，默认被改动时先建立当前自定义项。
        m_presets->m_layoutPresets.captureCurrent(m_editorConfig.visual);
        j["layoutPresets"] = m_presets->m_layoutPresets;
        auto presets       = m_presets->m_audioPresets;
        captureAudioPreset(presets.current(), m_editorConfig.settings);
        j["audioPresets"]               = presets;
        j["collaborationParticipantId"] = m_collaborationParticipantId;
    }

    return j;
}

/// @brief 将 UTF-8 项目路径置于最近列表首位、去重并按上限裁剪。
/// @param path 用户刚成功打开的项目目录 UTF-8 文本。
/// @warning 低频项目操作路径：修改配置后会立即执行一次磁盘保存。
void AppConfig::addRecentProject(
    const std::string& path)  // path 必须为 UTF-8 编码
{
    {
        // 列表修改保持在单个临界区，读取方不会看到去重与插入之间的状态。
        std::lock_guard<std::mutex> lock(m_mutex);
        auto&                       list = m_editorConfig.recentProjects;
        // 上限在同一锁内读取，列表裁剪与对应设置属于一致配置快照。
        int limit = m_editorConfig.settings.recentProjectsLimit;

        // 先移除全部相同路径，使重新打开的项目只在首位保留一次。
        list.erase(std::remove(list.begin(), list.end(), path), list.end());

        // 最新项目插入首位，现有项目相对顺序保持不变。
        list.insert(list.begin(), path);

        // 超出用户配置上限时只裁剪尾部最旧记录。
        if ( list.size() > static_cast<size_t>(limit) ) {
            // 配置解析已约束 limit 非负，此处转换为 size_t 后安全比较。
            list.resize(static_cast<size_t>(limit));
        }
    }

    // 释放锁后再保存；save 会自行获取互斥量生成快照，避免递归死锁。
    // 保存失败由内部日志记录，最近项目仍已在当前内存配置中生效。
    save();
}

/// @brief 获取当前平台和配置覆盖对应的默认用户配置路径。
/// @return AppPaths 解析的 user_config.json 路径。
std::filesystem::path AppConfig::getDefaultConfigPath() const
{
    // 路径策略集中在 AppPaths，配置管理器不复制环境覆盖与平台分支。
    return AppPaths::userConfigFilePath();
}

}  // namespace MMM::Config
