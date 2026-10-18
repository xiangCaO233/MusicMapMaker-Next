#include "config/LayoutPresets.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <utility>

namespace MMM::Config
{
namespace
{
/// @brief 文件布局白名单，禁止通过导入文件注入其他软件偏好。
/// @note 与 applyLayoutPreset 的字段集合保持一致，新增布局字段须同步两处。
constexpr const char* FIELDS[] = {
    "trackLayout",
    "canvasComponents",
    "keyCountLayouts",
    "previewConfig",
    "trackBoxLineWidth",
    "judgeline_pos",
    "noteScaleX",
    "noteScaleY",
    "noteTexturePosition",
    "debugCanvasCameraAngleDegrees",
    "debugCanvasCameraYawDegrees",
    "debugCanvasCameraDistance",
    "canvasCameraPoseEnabled",
    "canvasCameraLength",
    "canvasCameraPosition",
    "canvasCameraRotation",
    "interactionHitboxScaleX",
    "interactionHitboxScaleY",
};
/// @brief 从已有视觉序列化结果提取布局字段。
/// @note 仅在保存、切换和文件操作的低频路径创建 JSON。
/// @param visual 当前现场或方案内的布局快照。
/// @return 只含布局字段的对象，调用方负责添加格式版本。
/// @details 使用 VisualConfig 的既有序列化约定，避免同一布局出现两种文件语义。
/// 可选辅助区边界的缺省状态与显式数值都被保留。
nlohmann::json layoutJson(const VisualConfig& visual)
{
    const nlohmann::json full   = visual;
    auto                 result = nlohmann::json::object();
    // 白名单确保共享 VisualConfig 载体不会扩大预设作用范围。
    for ( const auto* field : FIELDS ) result[field] = full.at(field);
    return result;
}
/// @brief 删除与公共回退模板相同的键数覆盖，只比较实际布局差异。
/// @param visual 只读现场，不改变真实配置中的键数集合。
/// @return 可用于迁移检测的语义布局对象。
/// @note 打开布局设置可能建立默认键数条目，这本身不算自定义。
/// 相机仅从旧表示切到等价姿态表示时同样不产生用户方案。
/// @warning 仅在实际修改边沿比较，不在空闲帧构造 JSON 或遍历键数集合。
nlohmann::json comparableLayout(const VisualConfig& visual)
{
    auto  json    = layoutJson(visual);
    auto& entries = json["keyCountLayouts"];
    // 结构性缺省与显式相同值等价，防止打开面板就创建方案。
    std::erase_if(
        entries.get_ref<nlohmann::json::array_t&>(), [&](const auto& item) {
            return item.at("trackLayout") == json["trackLayout"] &&
                   item.at("canvasComponents") == json["canvasComponents"] &&
                   item.at("judgeline_pos") == json["judgeline_pos"];
        });
    // 默认相机迁移只切换表示开关，不改变任何实际位置或旋转。
    if ( visual.canvasCameraPosition == std::array<float, 3>{ 0, 0, 1 } &&
         visual.canvasCameraRotation == std::array<float, 3>{ 0, 0, 0 } &&
         visual.debugCanvasCameraAngleDegrees == 0 &&
         visual.debugCanvasCameraYawDegrees == 0 &&
         visual.debugCanvasCameraDistance == 1 )
        json["canvasCameraPoseEnabled"] = false;
    return json;
}
/// @brief 为自动创建、迁移和导入选择不会覆盖已有项的名称。
/// @param items 当前库；只读查找不改变选择。
/// @param base 自动方案的基础名称。
/// @return 唯一名称，后缀增长时截断处保持 UTF-8 字符完整。
/// @warning 仅在建立新项或导入时命名，不在普通画布更新中搜索整个库。
std::string uniqueName(const std::vector<LayoutPreset>& items,
                       const std::string&               base)
{
    std::string name = base;
    for ( size_t suffix = 2;
          std::ranges::find(items, name, &LayoutPreset::m_name) != items.end();
          ++suffix ) {
        const auto tail   = " (" + std::to_string(suffix) + ")";
        size_t     length = std::min(base.size(), 256 - tail.size());
        // 截断不能落在 UTF-8 连续字节中间，否则保存时会遇到无效文本。
        while ( length < base.size() && length > 0 &&
                (static_cast<unsigned char>(base[length]) & 0xC0) == 0x80 )
            --length;
        name = base.substr(0, length) + tail;
    }
    return name;
}
/// @brief 创建覆盖可选区域与动态列表元素的验证模板。
/// @note 空列表无法描述元素类型，因此专门放入一个类型样本。
/// @return 含有所有可选字段和列表元素样本的可信对象。
/// @details 模板从生产序列化器生成，因此字段更名会同步进入验证。
/// 主轨道与每键数轨道都补齐辅助区域，防止默认空对象漏检。
/// 逐轨组件样本同样覆盖两级容器，不将数组长度一误作固定长度。
nlohmann::json layoutSchema()
{
    VisualConfig visual;
    visual.keyCountLayouts.push_back(KeyCountLayoutConfig{});
    // 辅助区的可选数字默认不序列化，样本必须显式补齐。
    /// @brief 为区域可选字段提供有限数值样本，不修改默认布局本身。
    auto fill = [](TrackLayout& layout) {
        for ( auto* region :
              { &layout.draftLanes, &layout.annotation, &layout.bgmLanes } ) {
            region->left  = 0.0F;
            region->right = 1.0F;
            region->width = 0.1F;
        }
    };
    fill(visual.trackLayout);
    fill(visual.keyCountLayouts.front().trackLayout);
    visual.canvasComponents.kpsTracks.push_back(CanvasKpsTrackPlacement{});
    visual.keyCountLayouts.front().canvasComponents.kpsTracks.push_back(
        CanvasKpsTrackPlacement{});
    return layoutJson(visual);
}
/// @brief 递归验证字段类型及数值范围，阻止无异常构建中的 JSON 类型断言。
/// @note 未知字段拒绝；缺失字段由现有版本的默认布局补齐。
/// 动态列表限制数量，固定数组检查长度，递归深度由可信模板限制。
/// @param value 外部文件中的待验证节点。
/// @param schema 由生产类型产生的可信节点，不接收外部自定义模板。
/// @param key 当前字段名称，用于区分变长列表与固定坐标数组。
/// @return 字段结构、数值类型和转换范围均安全时为真。
/// @details 数值先检查有限性，再按消费类型检查可表示范围。
/// 模板递归不会遍历未知对象，从而限制导入结构的复杂度。
bool matches(const nlohmann::json& value, const nlohmann::json& schema,
             const std::string& key = {})
{
    if ( schema.is_object() ) {
        if ( !value.is_object() ) return false;
        for ( auto it = value.begin(); it != value.end(); ++it ) {
            const auto expected = schema.find(it.key());
            // 不把未知嵌套对象交给配置读取器，保持结构契约可审查。
            if ( expected == schema.end() ||
                 !matches(it.value(), *expected, it.key()) )
                return false;
        }
        return true;
    }
    if ( schema.is_array() ) {
        if ( !value.is_array() ) return false;
        const bool dynamic = key == "keyCountLayouts" || key == "kpsTracks";
        if ( value.size() > 1024 ||
             (!dynamic && value.size() != schema.size()) )
            return false;
        // 列表中的每个元素独立校验，不以首项合法代表整个列表。
        for ( size_t i = 0; i < value.size(); ++i )
            if ( schema.empty() || !matches(value[i], schema[dynamic ? 0 : i]) )
                return false;
        return true;
    }
    if ( schema.is_boolean() ) return value.is_boolean();
    if ( schema.is_string() )
        return value.is_string() &&
               value.get_ref<const std::string&>().size() <= 256;
    if ( schema.is_number() ) {
        if ( !value.is_number() ) return false;
        const double number = value.get<double>();
        // 整数消费者均使用 32 位字段，拒绝截断、溢出和非有限值。
        if ( !std::isfinite(number) ||
             std::abs(number) > std::numeric_limits<float>::max() )
            return false;
        if ( schema.is_number_integer() &&
             (!value.is_number_integer() ||
              number < std::numeric_limits<int>::min() ||
              number > std::numeric_limits<int>::max()) )
            return false;
        return true;
    }
    return false;
}
/// @brief 在任何 typed get 前验证单项，失败保持目标不变。
/// @note JSON 结构来自不可信文件，不能依赖配置加载器的缺省值处理类型错误。
/// @param json 单项名称与布局对象。
/// @param preset 成功时接收完整临时方案，失败不修改。
/// @return 结构合法并完成现有配置规范化时为真。
/// @details 名称使用字节上限，与客户端名称输入保持一致。
/// 读取只构造快照，不发布引擎状态，也不改变当前选择。
bool readPreset(const nlohmann::json& json, LayoutPreset& preset)
{
    if ( !json.is_object() || !json.contains("name") ||
         !json["name"].is_string() || !json.contains("layout") )
        return false;
    const auto& name = json["name"].get_ref<const std::string&>();
    if ( name.find_first_not_of(" \t\r\n") == std::string::npos ||
         name.size() > 256 || !matches(json["layout"], layoutSchema()) )
        return false;
    // 先验证完整输入，再构造临时对象，保证失败不会留下半套布局。
    preset.m_name   = name;
    preset.m_visual = json["layout"].get<VisualConfig>();
    return true;
}
/// @brief 写出单项结构，供用户配置和独立导出文件共用。
/// @param preset 已捕获实时布局的方案。
/// @return 可嵌入库文件或独立文件的单项对象。
/// @note 格式版本由外层容器维护，单项不重复保存。
nlohmann::json presetJson(const LayoutPreset& preset)
{
    return { { "name", preset.m_name },
             { "layout", layoutJson(preset.m_visual) } };
}
/// @brief 验证版本标识，不接受未来版本或数字字符串。
/// @param json 待检查的外层对象，可能是解析失败标记。
/// @return 当前读取器明确支持此版本时为真。
/// @note 不隐式转换字符串或浮点版本，避免未来协议被误读。
bool validVersion(const nlohmann::json& json)
{
    return json.is_object() && json.contains("version") &&
           json["version"].is_number_integer() && json["version"] == 1;
}
}  // namespace

/// @brief 只复制布局字段，双向复用以避免捕获和应用范围发生漂移。
/// @note 当前视觉对象仍是唯一实时权威，预设快照不参与每帧读取。
/// @param preset 待应用的稳定方案快照。
/// @param visual 接收完整布局的实时配置。
/// @details 整体替换键数集合，目标不残留上一个方案的额外键数。
/// 公共回退模板负责尚未出现的键数，避免必须逐一列举。
/// 摄像机和长度倍率同时切换，保持透视坐标与轨道空间一致。
void applyLayoutPreset(const LayoutPreset& preset, VisualConfig& visual)
{
    // 保留全部键数覆盖，而非只记录当前项目的轨道数。
    visual.trackLayout         = preset.m_visual.trackLayout;
    visual.canvasComponents    = preset.m_visual.canvasComponents;
    visual.keyCountLayouts     = preset.m_visual.keyCountLayouts;
    visual.previewConfig       = preset.m_visual.previewConfig;
    visual.trackBoxLineWidth   = preset.m_visual.trackBoxLineWidth;
    visual.judgeline_pos       = preset.m_visual.judgeline_pos;
    visual.noteScaleX          = preset.m_visual.noteScaleX;
    visual.noteScaleY          = preset.m_visual.noteScaleY;
    visual.noteTexturePosition = preset.m_visual.noteTexturePosition;
    visual.debugCanvasCameraAngleDegrees =
        preset.m_visual.debugCanvasCameraAngleDegrees;
    visual.debugCanvasCameraYawDegrees =
        preset.m_visual.debugCanvasCameraYawDegrees;
    visual.debugCanvasCameraDistance =
        preset.m_visual.debugCanvasCameraDistance;
    // 旧式观察相机与新姿态相机同时保存，切换旧方案也能完整恢复。
    // poseEnabled 决定当前实际使用哪一种，不能仅恢复位置和角度。
    visual.canvasCameraPoseEnabled = preset.m_visual.canvasCameraPoseEnabled;
    visual.canvasCameraLength      = preset.m_visual.canvasCameraLength;
    visual.canvasCameraPosition    = preset.m_visual.canvasCameraPosition;
    visual.canvasCameraRotation    = preset.m_visual.canvasCameraRotation;
    // 物件缩放改变后包围盒应沿用同一方案的交互比例。
    // 音符命中仍使用原始时间坐标，此处不移动任何谱面物件。
    visual.interactionHitboxScaleX = preset.m_visual.interactionHitboxScaleX;
    visual.interactionHitboxScaleY = preset.m_visual.interactionHitboxScaleY;
}
/// @brief 收取实时配置中的布局子集，不改变方案名称。
/// @param preset 接收布局快照的当前方案。
/// @param visual 当前 AppConfig 中的权威实时配置。
/// @warning 低频编辑提交入口，允许复制键数和逐轨组件容器。
/// @details 复用应用字段列表，防止捕获和恢复各自维护不同范围。
/// 快照不取代实时字段，普通滑条继续使用原有修改与保存路径。
void captureLayoutPreset(LayoutPreset& preset, const VisualConfig& visual)
{
    // 应用到快照的方向与正常切换相反，字段白名单保持相同。
    LayoutPreset source;
    // 这里只在明确的预设操作中复制，不属于渲染循环。
    source.m_visual = visual;
    applyLayoutPreset(source, preset.m_visual);
}
/// @brief 按代码保留的稳定名称判断只读内置方案。
/// @note 文件导入会为重名项生成新名称，不能冒充已有代码基准。
bool LayoutPreset::isBuiltIn() const
{
    return m_name == "默认" || m_name == "斜轨预览";
}
/// @brief 提供不改变持久身份的内置名称标志。
/// @warning 界面热路径：返回静态文本或成员缓冲，不构造临时字符串。
const char* LayoutPreset::displayName() const
{
    // 标志只属于展示，选择、迁移与名称冲突检查仍使用原始名称。
    if ( m_name == "默认" ) return "默认(内置)";
    if ( m_name == "斜轨预览" ) return "斜轨预览(内置)";
    return m_name.c_str();
}
/// @brief 建立只读默认方案与只读斜轨预览方案。
/// @details 普通默认值沿用 VisualConfig，首次迁移由 AppConfig 收取旧值。
/// 斜轨位置采用画布相机坐标，旋转单位为角度。
/// 左右边界为画布归一化坐标，九成宽度两侧各留百分之五。
/// 不预建键数项，确保未来任意键数都能继承该模板。
LayoutPresets::LayoutPresets()
{
    m_items.emplace_back();
    LayoutPreset slanted;
    slanted.m_name                           = "斜轨预览";
    slanted.m_visual.canvasCameraPoseEnabled = true;
    slanted.m_visual.canvasCameraPosition    = { 0.0F, -0.8F, 0.9F };
    slanted.m_visual.canvasCameraRotation    = { 45.0F, 0.0F, 0.0F };
    slanted.m_visual.canvasCameraLength      = 3.0F;
    // 不建立特定键数覆盖；所有未定制键数都使用居中九成宽模板。
    slanted.m_visual.trackLayout.left  = 0.05F;
    slanted.m_visual.trackLayout.right = 0.95F;
    m_items.push_back(std::move(slanted));
}
/// @brief 返回当前选择的可编辑方案，不创建新项。
/// @return 名称匹配项，失效名称防御性回退首项。
/// @pre 集合由本类管理且至少有一个方案。
/// @warning 只在配置操作中查找，调用方不应每帧复制返回值。
LayoutPreset& LayoutPresets::current()
{
    // 容器由构造、删除保护和反序列化保证非空。
    const auto it =
        std::ranges::find(m_items, m_selected, &LayoutPreset::m_name);
    return it == m_items.end() ? m_items.front() : *it;
}
/// @brief 只读查找当前选择，供保存副本与界面引用。
/// @return 与可编辑访问具有相同回退规则的稳定引用。
/// @pre 调用期间集合结构保持不变。
/// @note 引用不跨越复制、删除或导入引起的容器扩容。
const LayoutPreset& LayoutPresets::current() const
{
    const auto it =
        std::ranges::find(m_items, m_selected, &LayoutPreset::m_name);
    return it == m_items.end() ? m_items.front() : *it;
}
/// @brief 将当前现场收存到旧项后恢复目标项。
/// @param name 库内已存在的方案名称。
/// @param visual 当前配置，同时作为旧项来源和新项接收端。
/// @return 目标存在并已应用时为真，不存在时不修改任何项。
/// @note 收存内置现场可能插入新项，UI 选择成功后必须结束原容器枚举。
bool LayoutPresets::select(const std::string& name, VisualConfig& visual)
{
    // 收存可能创建新项并使 vector 引用失效，先保存目标名称再查找。
    const std::string target = name;
    if ( std::ranges::find(m_items, target, &LayoutPreset::m_name) ==
         m_items.end() )
        return false;
    captureCurrent(visual);
    const auto it = std::ranges::find(m_items, target, &LayoutPreset::m_name);
    applyLayoutPreset(*it, visual);
    m_selected = target;
    return true;
}
/// @brief 在实际修改边沿收存布局，保护所有代码内置方案不受编辑覆盖。
/// @param visual 当前实时布局，包括所有键数和相机姿态。
/// @return 已建立自定义项或更新所选项时为真，无变化时为假。
/// @warning 仅在用户修改或加载迁移时比较，不用于空闲帧轮询。
/// @note 语义比较忽略等价的键数默认覆盖和相机表示迁移。
bool LayoutPresets::captureCurrent(const VisualConfig& visual)
{
    // 比较发生在真实编辑边沿，默认方案不会因保存动作本身而变为用户项。
    if ( comparableLayout(current().m_visual) == comparableLayout(visual) )
        return false;
    // 自动项只在离开代码基准时建立，后续连续变化更新同一个项。
    // 与已存在的自定义名称冲突时生成后缀，绝不覆盖旧用户布局。
    if ( current().isBuiltIn() ) {
        LayoutPreset custom;
        custom.m_name = uniqueName(m_items, "自定义布局");
        captureLayoutPreset(custom, visual);
        // 当前内置项保持代码值；新副本在本次修改内立即成为活动项。
        m_selected = custom.m_name;
        m_items.push_back(std::move(custom));
    } else
        captureLayoutPreset(current(), visual);
    return true;
}
/// @brief 恢复整个代码布局，不删除用户方案或只重置当前键数。
/// @param visual 接收公共模板、全部键数集合和摄像机参数。
/// @note 先收存当前现场，防止重置丢掉尚未保存的最后一次编辑。
void LayoutPresets::resetToDefault(VisualConfig& visual)
{
    // 收存发生在恢复前，保留用户最后一帧的现场作为自定义方案。
    // 应用默认整体替换键数集合，因此不会残留其他键数的覆盖。
    captureCurrent(visual);
    m_selected = "默认";
    applyLayoutPreset(current(), visual);
}
/// @brief 以现场布局创建独立副本并立即选中。
/// @param name 新方案名称，不允许空白字符串或库内重名。
/// @param visual 尚未保存的实时布局，优先于旧快照。
/// @return 创建成功时为真，命名失败不改变当前选择。
/// @note 所有键数和逐轨组件均按值复制，之后修改互不关联。
bool LayoutPresets::duplicate(const std::string&  name,
                              const VisualConfig& visual)
{
    if ( name.find_first_not_of(" \t\r\n") == std::string::npos ||
         name.size() > 256 ||
         std::ranges::find(m_items, name, &LayoutPreset::m_name) !=
             m_items.end() )
        return false;
    captureCurrent(visual);
    LayoutPreset copy = current();
    copy.m_name       = name;
    // 先保存名称，vector 扩容后不再使用旧元素引用。
    m_selected = copy.m_name;
    m_items.push_back(std::move(copy));
    return true;
}
/// @brief 修改当前方案身份，同时更新持久选择。
/// @param name 不与其他方案冲突的新名称。
/// @return 名称合法且已替换时为真。
/// @note 布局内容不改变，因此不重新捕获实时配置。
bool LayoutPresets::rename(const std::string& name)
{
    // 两个内置项都是稳定的代码基准，只能复制后再改名。
    if ( current().isBuiltIn() ) return false;
    if ( name.find_first_not_of(" \t\r\n") == std::string::npos ||
         name.size() > 256 ||
         std::ranges::find(m_items, name, &LayoutPreset::m_name) !=
             m_items.end() )
        return false;
    current().m_name = name;
    m_selected       = name;
    return true;
}
/// @brief 删除当前项并恢复剩余首项。
/// @param visual 接收剩余方案布局的实时配置。
/// @return 删除成功时为真，最后一项始终保留。
/// @note 删除不再捕获旧项，避免将已删除现场污染到剩余项。
bool LayoutPresets::remove(VisualConfig& visual)
{
    if ( current().isBuiltIn() || m_items.size() <= 1 ) return false;
    const std::string name = current().m_name;
    std::erase_if(m_items,
                  [&](const auto& item) { return item.m_name == name; });
    // 删除项不再需要收存，直接恢复首项避免把旧值写进它。
    m_selected = m_items.front().m_name;
    applyLayoutPreset(current(), visual);
    return true;
}
/// @brief 将单个完整布局方案写入用户选择的文件。
/// @param path 已由文件选择器确认的目标路径。
/// @param preset 已收取最新实时布局的单项快照。
/// @param error 失败描述，成功时清空。
/// @return 文件成功关闭并完成输出时为真。
/// @warning 仅由用户导出触发，不在普通画布更新中执行。
bool exportLayoutPreset(const std::filesystem::path& path,
                        const LayoutPreset& preset, std::string& error)
{
    error.clear();
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if ( !output ) {
        error = "无法创建布局预设文件。";
        return false;
    }
    const nlohmann::json json = { { "format", "mmm-layout-preset" },
                                  { "version", 1 },
                                  { "preset", presetJson(preset) } };
    // 单文件携带明确格式标识，不能把客户端整份配置误作方案导入。
    output << json.dump(2) << '\n';
    // 关闭也可能失败，不能仅依据打开成功报告导出完成。
    output.close();
    if ( !output ) {
        error = "布局预设文件写入失败。";
        return false;
    }
    return true;
}
/// @brief 验证独立方案文件并将其作为新项选中。
/// @param path 用户明确选择的输入文件。
/// @param visual 只有完全验证成功后才接收导入布局。
/// @param error 返回失败原因，取消选择由界面层处理。
/// @return 添加、选中并应用新项时为真。
/// @warning 低频磁盘读取；输入大小限制不影响常态渲染。
bool LayoutPresets::importFile(const std::filesystem::path& path,
                               VisualConfig& visual, std::string& error)
{
    error = "无效或不支持的布局预设文件。";
    std::error_code ec;
    const auto      size = std::filesystem::file_size(path, ec);
    // 输入限额在读取之前检查，避免误选其他大型资源耗尽内存。
    if ( ec || size > 4 * 1024 * 1024 ) return false;
    std::ifstream input(path, std::ios::binary);
    // 权限或文件消失与语法错误均为可恢复失败，不进入引擎发布。
    if ( !input ) return false;
    const auto   json = nlohmann::json::parse(input, nullptr, false);
    LayoutPreset item;
    if ( !validVersion(json) || !json.contains("format") ||
         json["format"] != "mmm-layout-preset" || !json.contains("preset") ||
         !readPreset(json["preset"], item) )
        return false;
    // 重名以新项导入，不覆盖原方案；先捕获离开的实时状态。
    item.m_name = uniqueName(m_items, item.m_name);
    captureCurrent(visual);
    m_selected = item.m_name;
    m_items.push_back(std::move(item));
    applyLayoutPreset(current(), visual);
    error.clear();
    return true;
}
/// @brief 保存库内每一项及当前选择，不在此处捕获现场。
/// @param json 接收版本化的客户端布局集合。
/// @param presets 由保存入口收存实时配置后的库副本。
/// @note 所有键数在单项中保存，集合不按当前项目过滤。
void to_json(nlohmann::json& json, const LayoutPresets& presets)
{
    json = { { "version", 1 },
             { "selected", presets.m_selected },
             { "items", nlohmann::json::array() } };
    // 库内快照由 AppConfig 在保存前收取当前实值。
    for ( const auto& item : presets.m_items )
        json["items"].push_back(presetJson(item));
}
/// @brief 从客户端配置恢复已验证的方案集合。
/// @param json 可能来自旧版或手工修改的持久化对象。
/// @param presets 接收规范化集合，空库回退内置方案。
/// @details 每个项独立验证后才进入类型转换，重复名称保留首项。
/// 失效选择回退已恢复的首项，不指向已跳过的损坏项。
void from_json(const nlohmann::json& json, LayoutPresets& presets)
{
    // 调用方可以预先放入旧布局作为回退；验证失败不覆盖它。
    // json.get 的默认构造仍提供普通和斜轨两个内置方案。
    if ( !validVersion(json) || !json.contains("items") ||
         !json["items"].is_array() || json["items"].size() > 1024 )
        return;
    std::vector<LayoutPreset> items;
    // 无效项逐项跳过，合法项仍可恢复；重复名称只接收首项。
    for ( const auto& entry : json["items"] ) {
        LayoutPreset item;
        if ( readPreset(entry, item) &&
             std::ranges::find(items, item.m_name, &LayoutPreset::m_name) ==
                 items.end() )
            items.push_back(std::move(item));
    }
    // 全部损坏或空库与无效协议相同，保留现有可用方案。
    if ( items.empty() ) return;
    presets.m_items    = std::move(items);
    presets.m_selected = presets.m_items.front().m_name;
    if ( json.contains("selected") && json["selected"].is_string() ) {
        const auto selected = json["selected"].get<std::string>();
        if ( std::ranges::find(presets.m_items,
                               selected,
                               &LayoutPreset::m_name) != presets.m_items.end() )
            presets.m_selected = selected;
    }
    // 文件不能重定义内置方案；先保留旧版本改过的完整布局，再恢复代码基准。
    // 普通默认和斜轨预览统一迁移，未选中的旧自定义斜轨布局也不能被丢弃。
    const LayoutPresets codePresets;
    for ( const auto& builtIn : codePresets.m_items ) {
        const auto previous = std::ranges::find(
            presets.m_items, builtIn.m_name, &LayoutPreset::m_name);
        if ( previous != presets.m_items.end() &&
             comparableLayout(previous->m_visual) !=
                 comparableLayout(builtIn.m_visual) ) {
            // 只重命名旧数据；全部键数覆盖与相机姿态仍按值保留。
            // 新名称必须避让已存在的用户项，已选中的旧方案继续作为当前现场。
            const auto name  = uniqueName(presets.m_items, "自定义布局");
            previous->m_name = name;
            if ( presets.m_selected == builtIn.m_name )
                presets.m_selected = name;
        }
        // 迁移后原名可能不存在；恢复缺失内置项，也覆盖文件里等价的冗余表示。
        const auto entry = std::ranges::find(
            presets.m_items, builtIn.m_name, &LayoutPreset::m_name);
        if ( entry != presets.m_items.end() ) {
            entry->m_visual = builtIn.m_visual;
        } else if ( builtIn.m_name == "默认" ) {
            // 默认仍位于首项，保持失效选择和删除用户项时的既有回退语义。
            presets.m_items.insert(presets.m_items.begin(), builtIn);
        } else {
            presets.m_items.push_back(builtIn);
        }
    }
}
}  // namespace MMM::Config
