#pragma once

namespace MMM
{
namespace Graphic
{

struct Position {
    float x;
    float y;
    float z{ 0.f };
};

struct Color {
    float r;
    float g;
    float b;
    float a;
};

struct VKVertex {
    Position pos;
    Color    color;
};

}  // namespace Graphic

}  // namespace MMM
