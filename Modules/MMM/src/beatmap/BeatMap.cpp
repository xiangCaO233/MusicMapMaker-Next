#include "mmm/beatmap/BeatMap.h"

#include "LoadMMMMap.hpp"
#include "LoadMalodyMap.hpp"
#include "LoadOSUMap.hpp"
#include "LoadRMMap.hpp"
#include "SaveMMMMap.hpp"
#include "SaveMalodyMap.hpp"
#include "SaveOSUMap.hpp"
#include "SaveRMMap.hpp"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include <filesystem>
#include <fstream>

namespace MMM
{

/// @brief 按文件扩展名或无扩展名内容探测加载谱面。
/// @param mapFilePath 待加载谱面文件路径。
/// @return 成功解析的谱面；路径、格式或内容无效时返回空谱面。
/// @details
/// 有扩展名文件严格分派到对应格式读取器；无扩展名文件只探测 osu! 文件头，
/// 不猜测 JSON 或二进制格式。文件系统错误通过日志和空值表达，不抛出异常。
BeatMap BeatMap::loadFromFile(std::filesystem::path mapFilePath)
{
    // 使用 error_code 查询，权限和损坏路径不会从格式入口抛出异常。
    std::error_code ec;
    if ( !std::filesystem::exists(mapFilePath, ec) ) {
        // 不区分不存在与访问失败，二者都不能安全进入格式读取器。
        XWARN("Load Map Failed: File Not Exists or Access Denied: {}",
              Config::pathToUtf8(mapFilePath));
        return {};
    }

    if ( !mapFilePath.has_extension() ) {
        // 无扩展名兼容仅识别具有明确文本签名的 osu! 格式。
        std::ifstream ifs(mapFilePath);
        // 打开失败与签名不匹配走同一未知内容诊断，不继续尝试其他格式。
        if ( ifs.is_open() ) {
            std::string firstLine;
            // 只读取首行，避免为一次探测加载整个潜在大文件。
            if ( std::getline(ifs, firstLine) ) {
                if ( firstLine.find("osu file format") != std::string::npos ) {
                    return loadOSUMap(mapFilePath);
                }
            }
        }
        XWARN(
            "Load Map Failed: Unknown File extension and content check "
            "failed.");
        return {};
    }
    // UTF-8 转换只作用于扩展名；实际路径继续以 filesystem::path 传递。
    std::string mapFileExtention = Config::pathToUtf8(mapFilePath.extension());
    if ( mapFileExtention == ".osu" ) {
        // osu! 是带节区的文本格式，由专用读取器完成字段兼容。
        return loadOSUMap(mapFilePath);
    }
    if ( mapFileExtention == ".mc" ) {
        // `.mc` 在本项目语境中是 Malody JSON 谱面，不是 Windows 消息文件。
        return loadMalodyMap(mapFilePath);
    }
    if ( mapFileExtention == ".imd" ) {
        // IMD 使用 Rhythm Master 格式读取器。
        return loadRMMap(mapFilePath);
    }
    if ( mapFileExtention == ".mmm" ) {
        // 原生 MMM 格式保留编辑器扩展字段。
        return loadMMMMap(mapFilePath);
    }
    // 未知后缀不尝试按内容猜测，避免把任意资源误当谱面解析。
    XWARN("Unsupport map file type: {}", mapFileExtention);
    return {};
}

/// @brief 按目标扩展名选择格式写出器保存谱面。
/// @param mapFilePath 目标文件路径；扩展名决定输出格式。
/// @return 对应写出器成功时返回 true，未知格式或写出失败时返回 false。
bool BeatMap::saveToFile(std::filesystem::path mapFilePath) const
{
    // 保存不从当前来源格式推断目标，允许另存为完成格式转换。
    std::string mapFileExtention = Config::pathToUtf8(mapFilePath.extension());
    if ( mapFileExtention == ".osu" ) {
        // 目标格式由后缀唯一决定，写出器负责必要的字段降级。
        return saveOSUMap(*this, mapFilePath);
    }
    if ( mapFileExtention == ".mc" ) {
        // Malody 写出器生成 JSON `.mc` 文件。
        return saveMalodyMap(*this, mapFilePath);
    }
    if ( mapFileExtention == ".imd" ) {
        // Rhythm Master 写出可能丢弃原生扩展，结果由写出器返回值报告。
        return saveRMMap(*this, mapFilePath);
    }
    if ( mapFileExtention == ".mmm" ) {
        // 原生格式作为完整领域模型的默认持久化载体。
        return saveMMMMap(*this, mapFilePath);
    }
    // 未知扩展名在创建文件前拒绝，避免留下无法识别的空产物。
    XWARN("Unsupport save map file type: {}", mapFileExtention);
    return false;
}

/// @brief 重建按时间排序的顶层玩家物件引用视图。
/// @details
/// 视图借用各类型拥有型容器中的对象，不拥有生命周期；调用方修改容器结构后
/// 必须再次同步。Polyline 子节点不作为顶层项重复加入，排序提供确定性次键。
void BeatMap::sync()
{
    // 清空旧借用引用，防止容器变更后继续持有失效地址。
    m_allNotes.clear();
    // 按分类容器顺序收集后再统一排序，不依赖各容器原有排列。
    // 普通派生容器中的 Polyline 子节点由父折线统一管理，不重复进入顶层视图。
    for ( auto& note : m_noteData.notes ) {
        // m_allNotes 保存引用包装器，底层对象仍由分类容器拥有。
        if ( !note.m_isSubNote ) m_allNotes.push_back(std::ref(note));
    }
    // 添加所有长条物件
    for ( auto& hold : m_noteData.holds ) {
        // Hold 子节点与普通子节点遵循相同的顶层排除规则。
        if ( !hold.m_isSubNote ) m_allNotes.push_back(std::ref(hold));
    }
    // 添加所有滑键物件
    for ( auto& flick : m_noteData.flicks ) {
        // Flick 子节点仅通过所属 Polyline 参与序列化与渲染。
        if ( !flick.m_isSubNote ) m_allNotes.push_back(std::ref(flick));
    }
    // Polyline 容器本身只保存根对象，因此全部纳入顶层排序。
    for ( auto& poly : m_noteData.polylines ) {
        // 根折线本身进入统一视图，其内嵌节点不会在其他循环重复加入。
        m_allNotes.push_back(std::ref(poly));
    }

    // 时间使用容差视为同组，再以轨道和类型生成跨平台稳定顺序。
    std::stable_sort(m_allNotes.begin(),
                     m_allNotes.end(),
                     [](const std::reference_wrapper<Note>& a_ref,
                        const std::reference_wrapper<Note>& b_ref) {
                         const Note& a = a_ref.get();
                         const Note& b = b_ref.get();
                         // 超出容差才比较浮点时间，避免微小写出误差重排同拍物件。
                         if ( std::abs(a.m_timestamp - b.m_timestamp) > 1e-4 )
                             return a.m_timestamp < b.m_timestamp;
                         // 同时刻先按轨道排列，便于保存器和测试得到确定输出。
                         if ( a.m_track != b.m_track )
                             return a.m_track < b.m_track;
                         // 最终类型次键消除同时间同轨对象的容器遍历差异。
                         return a.m_type < b.m_type;
                     });
}

/// @brief 构造空谱面领域对象。
BeatMap::BeatMap() {}

/// @brief 释放谱面拥有的各类物件与元数据。
BeatMap::~BeatMap() {}
}  // namespace MMM
