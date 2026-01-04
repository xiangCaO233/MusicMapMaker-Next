#ifndef MMM_GAMELOOP_H
#define MMM_GAMELOOP_H

namespace MMM
{
class GameLoop
{
public:
    static GameLoop& instance();

    int start();

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
