#ifndef MMM_GAMELOOP_H
#define MMM_GAMELOOP_H

#include <expected>
#include <string>
#include <string_view>

namespace MMM
{
namespace Graphic
{
class VKContext;
}
class GameLoop
{
public:
    /*
     * vk上下文引用
     * */
    static std::expected<std::reference_wrapper<Graphic::VKContext>,
                         std::string>
        vkContext;

    /*
     * 游戏循环实例
     * */
    static GameLoop& instance();

    /*
     * 开始游戏循环实例
     * */
    int start(std::string_view windwo_title);

private:
    GameLoop();
    ~GameLoop();
    GameLoop(GameLoop&&)                 = delete;
    GameLoop(const GameLoop&)            = delete;
    GameLoop& operator=(GameLoop&&)      = delete;
    GameLoop& operator=(const GameLoop&) = delete;
};
}  // namespace MMM
#endif  // !MMM_GAMELOOP_H
