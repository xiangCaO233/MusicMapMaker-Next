#include "graphic/system/SystemTheme.h"
#include "log/colorful-log.h"

#if defined(_WIN32)
#    include <windows.h>
#elif defined(MMM_ENABLE_XDG_PORTAL_THEME)
#    include <gio/gio.h>
#    include <memory>
#    include <string_view>
#endif

/// @file
/// @brief 实现 Windows 注册表、macOS AppKit 与 Linux XDG Portal
/// 的系统主题查询入口。
///
/// 首次查询允许执行平台初始化；运行期刷新只读取轻量平台状态或非阻塞派发
/// 已到达的 Portal 信号。所有平台失败都收敛为 `Unknown`，由上层决定默认主题，
/// 本模块不会直接改写 ImGui 样式或用户配置。

namespace MMM::Graphic
{
#if defined(__APPLE__)
/// @brief 通过 AppKit 查询 macOS 当前亮暗外观。
/// @return macOS 当前系统主题。
/// @note 实现在 Objective-C++ 翻译单元中，以隔离 AppKit 头文件。
SystemTheme queryMacOSSystemTheme();
#endif

namespace
{
#if defined(_WIN32)
/// @brief 查询 Windows 为应用配置的亮暗颜色模式。
/// @return 注册表值可用时返回 Light 或 Dark，否则返回 Unknown。
/// @warning 低频平台查询：会同步读取当前用户注册表，不得从每帧热路径调用。
SystemTheme queryWindowsSystemTheme()
{
    // 默认值只负责初始化缓冲区；注册表读取失败仍显式返回 Unknown。
    DWORD         appUsesLightTheme = 1;
    DWORD         valueType         = 0;
    DWORD         valueSize         = sizeof(appUsesLightTheme);
    const LSTATUS status            = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        &valueType,
        &appUsesLightTheme,
        &valueSize);
    // 同时验证 API 状态和实际值类型，避免把其他注册表类型误解释为 DWORD。
    if ( status != ERROR_SUCCESS || valueType != REG_DWORD ) {
        return SystemTheme::Unknown;
    }
    // Windows 以零表示暗色，任意非零值按亮色处理。
    return appUsesLightTheme == 0 ? SystemTheme::Dark : SystemTheme::Light;
}
#elif defined(MMM_ENABLE_XDG_PORTAL_THEME)
/// @brief 释放 GLib 主上下文所有权。
struct GMainContextDeleter final {
    /// @brief 释放主上下文。
    /// @param context 待释放的上下文。
    void operator()(GMainContext* context) const
    {
        // 上下文创建失败时 unique_ptr 可能为空，删除器保持幂等。
        if ( context ) g_main_context_unref(context);
    }
};

/// @brief 释放 GDBusProxy 的 GObject 引用。
struct GDBusProxyDeleter final {
    /// @brief 释放代理引用。
    /// @param proxy 待释放的 Portal 代理。
    void operator()(GDBusProxy* proxy) const
    {
        // GDBusProxy 遵循 GObject 引用计数，由独占指针释放本地引用。
        if ( proxy ) g_object_unref(proxy);
    }
};

/// @brief 释放 GVariant 引用。
struct GVariantDeleter final {
    /// @brief 释放 Variant 引用。
    /// @param value 待释放的 Variant。
    void operator()(GVariant* value) const
    {
        // get_child_value 和同步调用结果返回的新引用均由此删除器接管。
        if ( value ) g_variant_unref(value);
    }
};

/// @brief Linux XDG Settings Portal 主题监听状态。
struct LinuxSystemThemeState final {
    /// @brief 专用于 Portal 代理信号的非阻塞主上下文。
    std::unique_ptr<GMainContext, GMainContextDeleter> context;

    /// @brief org.freedesktop.portal.Settings 代理。
    std::unique_ptr<GDBusProxy, GDBusProxyDeleter> proxy;

    /// @brief 最近一次由 Portal 返回的系统主题。
    SystemTheme theme{ SystemTheme::Unknown };

    /// @brief 是否已经完成首次 Portal 连接尝试。
    /// @note 失败同样置位，防止运行期刷新反复触发同步连接。
    bool initialized{ false };
};

/// @brief Portal Settings 服务总线名称。
constexpr std::string_view PORTAL_BUS_NAME = "org.freedesktop.portal.Desktop";

/// @brief Portal Settings 服务对象路径。
constexpr std::string_view PORTAL_OBJECT_PATH =
    "/org/freedesktop/portal/desktop";

/// @brief Portal Settings 接口名称。
constexpr std::string_view PORTAL_SETTINGS_INTERFACE =
    "org.freedesktop.portal.Settings";

/// @brief 标准外观设置命名空间。
constexpr std::string_view PORTAL_APPEARANCE_NAMESPACE =
    "org.freedesktop.appearance";

/// @brief 标准亮暗偏好设置键。
constexpr std::string_view PORTAL_COLOR_SCHEME_KEY = "color-scheme";

/// @brief Portal 同步初始读取允许的最长等待时间。
/// @note 仅首次初始化使用，后续主题变更通过非阻塞信号派发获得。
constexpr int PORTAL_INITIAL_READ_TIMEOUT_MS = 250;

/// @brief 获取进程唯一的 Linux 系统主题状态。
/// @return 可复用的 Portal 监听状态。
LinuxSystemThemeState& linuxSystemThemeState()
{
    // 函数局部静态对象把代理生命周期延长到进程退出，并避免全局初始化顺序问题。
    static LinuxSystemThemeState state;
    return state;
}

/// @brief 将 Portal color-scheme 无符号值转换为内部主题。
/// @param value Portal 返回值，允许包含一层或多层 Variant 包装。
/// @return 1 映射为 Dark，2 映射为 Light，其余返回 Unknown。
/// @details Portal 方法和信号在不同实现中可能返回嵌套 Variant；最多解包三层，
///          超出预期或最终类型不是 uint32 时拒绝猜测。
SystemTheme systemThemeFromPortalValue(GVariant* value)
{
    // 空值代表方法或信号没有携带可解析主题。
    if ( !value ) return SystemTheme::Unknown;

    // current 初始借用调用方对象；unboxed 只拥有最近一层新取得的引用。
    GVariant*                                  current = value;
    std::unique_ptr<GVariant, GVariantDeleter> unboxed;
    for ( int depth = 0;
          depth < 3 && g_variant_is_of_type(current, G_VARIANT_TYPE_VARIANT);
          ++depth ) {
        // reset 会在替换前释放上一层包装，current 随后指向新的活动值。
        unboxed.reset(g_variant_get_variant(current));
        current = unboxed.get();
    }

    // 只接受 Portal 规范规定的无符号整数类型。
    if ( !current || !g_variant_is_of_type(current, G_VARIANT_TYPE_UINT32) ) {
        return SystemTheme::Unknown;
    }

    // 0 表示无偏好，其他未来扩展值也统一保持 Unknown。
    switch ( g_variant_get_uint32(current) ) {
    case 1: return SystemTheme::Dark;
    case 2: return SystemTheme::Light;
    default: return SystemTheme::Unknown;
    }
}

/// @brief 从 Portal ReadOne 或旧版 Read 返回元组中提取主题。
/// @param result D-Bus 方法返回元组。
/// @return 元组中的系统主题。
/// @details 合法方法结果必须仅含一个值；具体 Variant 层级交给统一转换函数处理。
SystemTheme systemThemeFromPortalResult(GVariant* result)
{
    // 拒绝空值和意外元组形状，避免按错误索引读取第三方 Portal 实现结果。
    if ( !result || g_variant_n_children(result) != 1 ) {
        return SystemTheme::Unknown;
    }
    // child_value 返回新引用，局部 RAII 在转换结束后释放。
    std::unique_ptr<GVariant, GVariantDeleter> value(
        g_variant_get_child_value(result, 0));
    return systemThemeFromPortalValue(value.get());
}

/// @brief 释放 GError 并清空调用方指针。
/// @param error 待释放的 GLib 错误指针。
void clearGError(GError*& error)
{
    if ( error ) {
        // 释放后同步清空原指针，使 ReadOne 失败后可安全复用于 Read 回退调用。
        g_error_free(error);
        error = nullptr;
    }
}

/// @brief 同步读取一次 Portal 主题，兼容仅提供旧 Read 方法的实现。
/// @param proxy 已连接的 Settings Portal 代理。
/// @return Portal 当前系统主题。
/// @warning 启动低频路径：单次方法等待上限为 250ms，运行期不重复调用。
/// @details 优先调用新版 `ReadOne`，失败后清理错误并兼容旧版 `Read`；
///          两次调用都使用相同有限超时，不会无限等待会话总线。
SystemTheme readInitialPortalTheme(GDBusProxy* proxy)
{
    // 没有代理时不尝试构造 D-Bus 参数，直接交由上层使用默认主题。
    if ( !proxy ) return SystemTheme::Unknown;

    GError* error = nullptr;
    // ReadOne 是新接口，返回单个可能带 Variant 包装的 color-scheme 值。
    std::unique_ptr<GVariant, GVariantDeleter> result(
        g_dbus_proxy_call_sync(proxy,
                               "ReadOne",
                               g_variant_new("(ss)",
                                             PORTAL_APPEARANCE_NAMESPACE.data(),
                                             PORTAL_COLOR_SCHEME_KEY.data()),
                               G_DBUS_CALL_FLAGS_NONE,
                               PORTAL_INITIAL_READ_TIMEOUT_MS,
                               nullptr,
                               &error));
    if ( result ) {
        // 成功结果由局部 unique_ptr 持有，解析不会转移其所有权。
        return systemThemeFromPortalResult(result.get());
    }

    // ReadOne 不可用时清除其错误，避免与兼容调用的诊断混淆。
    clearGError(error);
    // 旧版 Read 使用相同参数，但部分 Portal 实现会增加 Variant 包装层。
    result.reset(
        g_dbus_proxy_call_sync(proxy,
                               "Read",
                               g_variant_new("(ss)",
                                             PORTAL_APPEARANCE_NAMESPACE.data(),
                                             PORTAL_COLOR_SCHEME_KEY.data()),
                               G_DBUS_CALL_FLAGS_NONE,
                               PORTAL_INITIAL_READ_TIMEOUT_MS,
                               nullptr,
                               &error));
    if ( !result ) {
        // 只在最终兼容调用也失败时记录一次警告，减少启动日志噪声。
        if ( error ) {
            XWARN("Failed to read system theme from XDG Settings Portal: {}",
                  error->message);
        }
        clearGError(error);
        return SystemTheme::Unknown;
    }
    // 统一结果解析兼容 Read 与 ReadOne 的包装差异。
    return systemThemeFromPortalResult(result.get());
}

/// @brief 接收 Portal SettingChanged 信号并更新缓存主题。
/// @param proxy 发出信号的 Settings 代理。
/// @param senderName 信号发送者名称。
/// @param signalName D-Bus 信号名称。
/// @param parameters 信号参数元组。
/// @param userData LinuxSystemThemeState 指针。
/// @details 仅消费目标命名空间和 `color-scheme`
/// 键；代理、发送者参数不参与筛选，
///          因为信号已经由绑定到指定 Portal 代理的回调提供。
void onPortalSettingChanged(GDBusProxy* proxy, const gchar* senderName,
                            const gchar* signalName, GVariant* parameters,
                            gpointer userData)
{
    // 显式标记未使用参数，实际来源边界由注册该回调的 proxy 决定。
    (void)proxy;
    (void)senderName;
    if ( g_strcmp0(signalName, "SettingChanged") != 0 || !parameters ||
         g_variant_n_children(parameters) != 3 || !userData ) {
        return;
    }

    // 三个子值均取得独立引用，确保后续字符串观察指针在比较期间有效。
    std::unique_ptr<GVariant, GVariantDeleter> namespaceValue(
        g_variant_get_child_value(parameters, 0));
    std::unique_ptr<GVariant, GVariantDeleter> keyValue(
        g_variant_get_child_value(parameters, 1));
    std::unique_ptr<GVariant, GVariantDeleter> themeValue(
        g_variant_get_child_value(parameters, 2));
    // 前两个字段必须是字符串；主题值允许由转换函数处理嵌套 Variant。
    if ( !g_variant_is_of_type(namespaceValue.get(), G_VARIANT_TYPE_STRING) ||
         !g_variant_is_of_type(keyValue.get(), G_VARIANT_TYPE_STRING) ) {
        return;
    }

    // 字符串指针借用各自 GVariant，并在当前回调结束前完成比较。
    const char* settingNamespace =
        g_variant_get_string(namespaceValue.get(), nullptr);
    const char* settingKey = g_variant_get_string(keyValue.get(), nullptr);
    // 忽略 Portal 上其他设置变化，避免误改主题缓存。
    if ( PORTAL_APPEARANCE_NAMESPACE != settingNamespace ||
         PORTAL_COLOR_SCHEME_KEY != settingKey ) {
        return;
    }

    // userData 指向进程唯一状态，回调只更新枚举值，不执行样式重建。
    auto* state  = static_cast<LinuxSystemThemeState*>(userData);
    state->theme = systemThemeFromPortalValue(themeValue.get());
}

/// @brief 初始化 Linux Settings Portal 代理及主题缓存。
/// @return 初始化后的主题状态。
/// @warning 启动低频路径：最多进行一次 D-Bus 代理连接和同步初始读取。
/// @details 初始化失败也会保留 `initialized=true`，运行期不重复建立连接；
///          专用 GMainContext 只由刷新入口以非阻塞方式迭代。
LinuxSystemThemeState& initializeLinuxSystemTheme()
{
    auto& state = linuxSystemThemeState();
    // 已成功或失败尝试过初始化时直接复用现有状态。
    if ( state.initialized ) return state;
    // 在任何可能失败的资源创建前置位，保证失败路径不会形成重试风暴。
    state.initialized = true;
    // 独立上下文隔离 Portal 信号，不接管应用其他 GLib 事件源。
    state.context.reset(g_main_context_new());
    if ( !state.context ) {
        XWARN("Failed to create GMainContext for system theme monitoring");
        return state;
    }

    GError* error = nullptr;
    // 同步创建代理时临时把专用上下文设为线程默认，使信号源绑定到该上下文。
    g_main_context_push_thread_default(state.context.get());
    state.proxy.reset(
        g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
                                      G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                      nullptr,
                                      PORTAL_BUS_NAME.data(),
                                      PORTAL_OBJECT_PATH.data(),
                                      PORTAL_SETTINGS_INTERFACE.data(),
                                      nullptr,
                                      &error));
    // 无论代理创建是否成功，都恢复调用线程原有的默认上下文栈。
    g_main_context_pop_thread_default(state.context.get());

    if ( !state.proxy ) {
        // 连接失败只记录诊断并保留 Unknown，后续刷新保持非阻塞。
        if ( error ) {
            XWARN("Failed to connect XDG Settings Portal: {}", error->message);
        }
        clearGError(error);
        return state;
    }

    // 回调 userData 指向静态状态，其生命周期长于代理信号连接。
    g_signal_connect(state.proxy.get(),
                     "g-signal",
                     G_CALLBACK(onPortalSettingChanged),
                     &state);
    // 代理就绪后执行唯一一次有限超时初始读取，填充首次查询结果。
    state.theme = readInitialPortalTheme(state.proxy.get());
    return state;
}

/// @brief 非阻塞派发 Linux Portal 已到达的主题变更信号。
/// @return 派发完成后的缓存主题。
/// @warning 运行期低频检查：单次最多派发 8 个事件，禁止改为阻塞迭代。
/// @details 每次只处理已经到达专用上下文的有限事件；没有待处理事件时立即返回。
SystemTheme refreshLinuxSystemTheme()
{
    auto& state = initializeLinuxSystemTheme();
    // 初始化失败时返回已有 Unknown，不在刷新路径重建资源。
    if ( !state.context || !state.proxy ) return state.theme;

    // 单次预算防止大量 D-Bus 信号长期占用调用线程。
    constexpr int MAX_DISPATCHES_PER_REFRESH = 8;
    for ( int dispatch = 0; dispatch < MAX_DISPATCHES_PER_REFRESH;
          ++dispatch ) {
        // FALSE 明确要求非阻塞；没有就绪事件时提前结束循环。
        if ( !g_main_context_iteration(state.context.get(), FALSE) ) break;
    }
    return state.theme;
}
#endif
}  // namespace

/// @brief 获取当前系统主题，并在需要时执行一次平台初始化。
/// @return 当前平台主题；无法识别时返回 Unknown。
SystemTheme getSystemTheme()
{
#if defined(_WIN32)
    // Windows 查询成本较低但仍是注册表访问，由调用方保证低频使用。
    return queryWindowsSystemTheme();
#elif defined(__APPLE__)
    // AppKit 查询被隔离在 Objective-C++ 实现中。
    return queryMacOSSystemTheme();
#elif defined(MMM_ENABLE_XDG_PORTAL_THEME)
    // Linux 首次调用建立 Portal 代理，后续仅返回缓存值。
    return initializeLinuxSystemTheme().theme;
#else
    // 未提供平台后端的构建明确报告未知，不猜测默认亮暗色。
    return SystemTheme::Unknown;
#endif
}

/// @brief 刷新平台主题状态并返回最新结果。
/// @return 刷新后的平台主题；无法识别时返回 Unknown。
SystemTheme refreshSystemTheme()
{
#if defined(_WIN32)
    // Windows 没有持久监听对象，刷新时重新读取当前用户注册表。
    return queryWindowsSystemTheme();
#elif defined(__APPLE__)
    // macOS 直接读取应用当前有效外观。
    return queryMacOSSystemTheme();
#elif defined(MMM_ENABLE_XDG_PORTAL_THEME)
    // Linux 只派发已到达事件，不执行新的同步 Portal 方法调用。
    return refreshLinuxSystemTheme();
#else
    // 无后端平台在刷新时维持 Unknown。
    return SystemTheme::Unknown;
#endif
}
}  // namespace MMM::Graphic
