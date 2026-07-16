#include "graphic/system/SystemTheme.h"
#include "log/colorful-log.h"

#if defined(_WIN32)
#    include <windows.h>
#elif defined(MMM_ENABLE_XDG_PORTAL_THEME)
#    include <gio/gio.h>
#    include <memory>
#    include <string_view>
#endif

namespace MMM::Graphic
{
#if defined(__APPLE__)
/// @brief 通过 AppKit 查询 macOS 当前亮暗外观。
/// @return macOS 当前系统主题。
SystemTheme queryMacOSSystemTheme();
#endif

namespace
{
#if defined(_WIN32)
/// @brief 查询 Windows 为应用配置的亮暗颜色模式。
/// @return 注册表值可用时返回 Light 或 Dark，否则返回 Unknown。
SystemTheme queryWindowsSystemTheme()
{
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
    if ( status != ERROR_SUCCESS || valueType != REG_DWORD ) {
        return SystemTheme::Unknown;
    }
    return appUsesLightTheme == 0 ? SystemTheme::Dark : SystemTheme::Light;
}
#elif defined(MMM_ENABLE_XDG_PORTAL_THEME)
/// @brief 释放 GLib 主上下文所有权。
struct GMainContextDeleter final {
    /// @brief 释放主上下文。
    /// @param context 待释放的上下文。
    void operator()(GMainContext* context) const
    {
        if ( context ) g_main_context_unref(context);
    }
};

/// @brief 释放 GDBusProxy 的 GObject 引用。
struct GDBusProxyDeleter final {
    /// @brief 释放代理引用。
    /// @param proxy 待释放的 Portal 代理。
    void operator()(GDBusProxy* proxy) const
    {
        if ( proxy ) g_object_unref(proxy);
    }
};

/// @brief 释放 GVariant 引用。
struct GVariantDeleter final {
    /// @brief 释放 Variant 引用。
    /// @param value 待释放的 Variant。
    void operator()(GVariant* value) const
    {
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
constexpr int PORTAL_INITIAL_READ_TIMEOUT_MS = 250;

/// @brief 获取进程唯一的 Linux 系统主题状态。
/// @return 可复用的 Portal 监听状态。
LinuxSystemThemeState& linuxSystemThemeState()
{
    static LinuxSystemThemeState state;
    return state;
}

/// @brief 将 Portal color-scheme 无符号值转换为内部主题。
/// @param value Portal 返回值，允许包含一层或多层 Variant 包装。
/// @return 1 映射为 Dark，2 映射为 Light，其余返回 Unknown。
SystemTheme systemThemeFromPortalValue(GVariant* value)
{
    if ( !value ) return SystemTheme::Unknown;

    GVariant*                                  current = value;
    std::unique_ptr<GVariant, GVariantDeleter> unboxed;
    for ( int depth = 0;
          depth < 3 && g_variant_is_of_type(current, G_VARIANT_TYPE_VARIANT);
          ++depth ) {
        unboxed.reset(g_variant_get_variant(current));
        current = unboxed.get();
    }

    if ( !current || !g_variant_is_of_type(current, G_VARIANT_TYPE_UINT32) ) {
        return SystemTheme::Unknown;
    }

    switch ( g_variant_get_uint32(current) ) {
    case 1: return SystemTheme::Dark;
    case 2: return SystemTheme::Light;
    default: return SystemTheme::Unknown;
    }
}

/// @brief 从 Portal ReadOne 或旧版 Read 返回元组中提取主题。
/// @param result D-Bus 方法返回元组。
/// @return 元组中的系统主题。
SystemTheme systemThemeFromPortalResult(GVariant* result)
{
    if ( !result || g_variant_n_children(result) != 1 ) {
        return SystemTheme::Unknown;
    }
    std::unique_ptr<GVariant, GVariantDeleter> value(
        g_variant_get_child_value(result, 0));
    return systemThemeFromPortalValue(value.get());
}

/// @brief 释放 GError 并清空调用方指针。
/// @param error 待释放的 GLib 错误指针。
void clearGError(GError*& error)
{
    if ( error ) {
        g_error_free(error);
        error = nullptr;
    }
}

/// @brief 同步读取一次 Portal 主题，兼容仅提供旧 Read 方法的实现。
/// @param proxy 已连接的 Settings Portal 代理。
/// @return Portal 当前系统主题。
/// @warning 启动低频路径：单次方法等待上限为 250ms，运行期不重复调用。
SystemTheme readInitialPortalTheme(GDBusProxy* proxy)
{
    if ( !proxy ) return SystemTheme::Unknown;

    GError*                                    error = nullptr;
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
        return systemThemeFromPortalResult(result.get());
    }

    clearGError(error);
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
        if ( error ) {
            XWARN("Failed to read system theme from XDG Settings Portal: {}",
                  error->message);
        }
        clearGError(error);
        return SystemTheme::Unknown;
    }
    return systemThemeFromPortalResult(result.get());
}

/// @brief 接收 Portal SettingChanged 信号并更新缓存主题。
/// @param proxy 发出信号的 Settings 代理。
/// @param senderName 信号发送者名称。
/// @param signalName D-Bus 信号名称。
/// @param parameters 信号参数元组。
/// @param userData LinuxSystemThemeState 指针。
void onPortalSettingChanged(GDBusProxy* proxy, const gchar* senderName,
                            const gchar* signalName, GVariant* parameters,
                            gpointer userData)
{
    (void)proxy;
    (void)senderName;
    if ( g_strcmp0(signalName, "SettingChanged") != 0 || !parameters ||
         g_variant_n_children(parameters) != 3 || !userData ) {
        return;
    }

    std::unique_ptr<GVariant, GVariantDeleter> namespaceValue(
        g_variant_get_child_value(parameters, 0));
    std::unique_ptr<GVariant, GVariantDeleter> keyValue(
        g_variant_get_child_value(parameters, 1));
    std::unique_ptr<GVariant, GVariantDeleter> themeValue(
        g_variant_get_child_value(parameters, 2));
    if ( !g_variant_is_of_type(namespaceValue.get(), G_VARIANT_TYPE_STRING) ||
         !g_variant_is_of_type(keyValue.get(), G_VARIANT_TYPE_STRING) ) {
        return;
    }

    const char* settingNamespace =
        g_variant_get_string(namespaceValue.get(), nullptr);
    const char* settingKey = g_variant_get_string(keyValue.get(), nullptr);
    if ( PORTAL_APPEARANCE_NAMESPACE != settingNamespace ||
         PORTAL_COLOR_SCHEME_KEY != settingKey ) {
        return;
    }

    auto* state  = static_cast<LinuxSystemThemeState*>(userData);
    state->theme = systemThemeFromPortalValue(themeValue.get());
}

/// @brief 初始化 Linux Settings Portal 代理及主题缓存。
/// @return 初始化后的主题状态。
/// @warning 启动低频路径：最多进行一次 D-Bus 代理连接和同步初始读取。
LinuxSystemThemeState& initializeLinuxSystemTheme()
{
    auto& state = linuxSystemThemeState();
    if ( state.initialized ) return state;
    state.initialized = true;
    state.context.reset(g_main_context_new());
    if ( !state.context ) {
        XWARN("Failed to create GMainContext for system theme monitoring");
        return state;
    }

    GError* error = nullptr;
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
    g_main_context_pop_thread_default(state.context.get());

    if ( !state.proxy ) {
        if ( error ) {
            XWARN("Failed to connect XDG Settings Portal: {}", error->message);
        }
        clearGError(error);
        return state;
    }

    g_signal_connect(state.proxy.get(),
                     "g-signal",
                     G_CALLBACK(onPortalSettingChanged),
                     &state);
    state.theme = readInitialPortalTheme(state.proxy.get());
    return state;
}

/// @brief 非阻塞派发 Linux Portal 已到达的主题变更信号。
/// @return 派发完成后的缓存主题。
/// @warning 运行期低频检查：单次最多派发 8 个事件，禁止改为阻塞迭代。
SystemTheme refreshLinuxSystemTheme()
{
    auto& state = initializeLinuxSystemTheme();
    if ( !state.context || !state.proxy ) return state.theme;

    constexpr int MAX_DISPATCHES_PER_REFRESH = 8;
    for ( int dispatch = 0; dispatch < MAX_DISPATCHES_PER_REFRESH;
          ++dispatch ) {
        if ( !g_main_context_iteration(state.context.get(), FALSE) ) break;
    }
    return state.theme;
}
#endif
}  // namespace

SystemTheme getSystemTheme()
{
#if defined(_WIN32)
    return queryWindowsSystemTheme();
#elif defined(__APPLE__)
    return queryMacOSSystemTheme();
#elif defined(MMM_ENABLE_XDG_PORTAL_THEME)
    return initializeLinuxSystemTheme().theme;
#else
    return SystemTheme::Unknown;
#endif
}

SystemTheme refreshSystemTheme()
{
#if defined(_WIN32)
    return queryWindowsSystemTheme();
#elif defined(__APPLE__)
    return queryMacOSSystemTheme();
#elif defined(MMM_ENABLE_XDG_PORTAL_THEME)
    return refreshLinuxSystemTheme();
#else
    return SystemTheme::Unknown;
#endif
}
}  // namespace MMM::Graphic
