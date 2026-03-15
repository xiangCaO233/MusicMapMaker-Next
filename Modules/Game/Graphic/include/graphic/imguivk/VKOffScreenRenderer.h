#pragma once

namespace MMM::Graphic
{

class VKOffScreenRenderer
{
public:
    VKOffScreenRenderer();
    VKOffScreenRenderer(VKOffScreenRenderer&&)                 = default;
    VKOffScreenRenderer(const VKOffScreenRenderer&)            = default;
    VKOffScreenRenderer& operator=(VKOffScreenRenderer&&)      = default;
    VKOffScreenRenderer& operator=(const VKOffScreenRenderer&) = default;
    ~VKOffScreenRenderer();

private:
};

}  // namespace MMM::Graphic
