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
        // 调用方显式提供的隔离目录拥有最高优先级，不能被测试默认值覆盖。
        const char* configRoot = std::getenv("MMM_CONFIG_ROOT");
        // 非空值表示 CTest 或人工调用已经建立独立配置根。
        if ( configRoot && configRoot[0] != '\0' ) return;

#ifdef _WIN32
        // Windows 使用线程安全的 CRT 环境更新接口。
        const int result = _putenv_s("MMM_CONFIG_ROOT", MMM_TEST_CONFIG_ROOT);
#else
        // POSIX 路径允许覆盖继承到进程的空或缺失变量。
        const int result = setenv("MMM_CONFIG_ROOT", MMM_TEST_CONFIG_ROOT, 1);
#endif
        // 配置隔离失败时禁止继续测试，避免回退到真实用户配置目录。
        if ( result != 0 ) std::abort();
    }
};

/// @brief 仅链接进测试可执行文件的启动期配置隔离守卫。
const TestConfigIsolation TEST_CONFIG_ISOLATION;

}  // namespace
