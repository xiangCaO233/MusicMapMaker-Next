#include <cstdlib>

namespace
{

/// @brief 在测试 main 执行前隔离默认配置目录，保护个人配置和 ImGui 布局。
class TestConfigIsolation
{
public:
    /// @brief 未显式指定隔离根时使用构建目录内的目标专属目录。
    /// @details CTest 会显式注入隔离根；直接运行测试二进制时由此补齐。
    TestConfigIsolation()
    {
        const char* configRoot = std::getenv("MMM_CONFIG_ROOT");
        if ( configRoot && configRoot[0] != '\0' ) return;

#ifdef _WIN32
        const int result = _putenv_s("MMM_CONFIG_ROOT", MMM_TEST_CONFIG_ROOT);
#else
        const int result = setenv("MMM_CONFIG_ROOT", MMM_TEST_CONFIG_ROOT, 1);
#endif
        // 配置隔离失败时禁止继续测试，避免回退到真实用户配置目录。
        if ( result != 0 ) std::abort();
    }
};

/// @brief 仅链接进测试可执行文件的启动期配置隔离守卫。
const TestConfigIsolation TEST_CONFIG_ISOLATION;

}  // namespace
