#pragma once

namespace MMM::MMM::Graphic::UI
{

class Brush
{
public:
    Brush()                        = default;
    Brush(Brush&&)                 = default;
    Brush(const Brush&)            = default;
    Brush& operator=(Brush&&)      = default;
    Brush& operator=(const Brush&) = default;
    ~Brush()                       = default;

private:
};
}  // namespace MMM::MMM::Graphic::UI
