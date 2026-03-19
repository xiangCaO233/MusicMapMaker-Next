#pragma once

#include "ui/IRenderableView.h"

namespace MMM::Canvas
{
class TestCanvas : public Graphic::UI::IRenderableView
{
public:
    TestCanvas(const std::string& name, uint32_t w, uint32_t h);
    TestCanvas(TestCanvas&&)                 = delete;
    TestCanvas(const TestCanvas&)            = delete;
    TestCanvas& operator=(TestCanvas&&)      = delete;
    TestCanvas& operator=(const TestCanvas&) = delete;
    ~TestCanvas()                            = default;

    // 接口实现
    void update() override;
    ///@brief 是否需要重新记录命令 (比如数据变了)
    bool isDirty() const override;
};

}  // namespace MMM::Canvas
