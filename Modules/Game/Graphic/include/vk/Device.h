#pragma once

namespace MMM
{
namespace Graphic
{
class Device
{
public:
    Device();
    ~Device();

    Device(Device&&)                 = delete;
    Device(const Device&)            = delete;
    Device& operator=(Device&&)      = delete;
    Device& operator=(const Device&) = delete;

private:
};
}  // namespace Graphic

}  // namespace MMM
