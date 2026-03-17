#pragma once

namespace MMM::Graphic::UI
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
}  // namespace MMM::Graphic::UI
