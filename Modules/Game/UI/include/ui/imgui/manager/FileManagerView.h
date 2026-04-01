#pragma once

#include "ui/ISubView.h"

namespace MMM::UI
{

class FileManagerView : public ISubView
{
public:
    FileManagerView(const std::string& subViewName) : ISubView(subViewName) {}
    FileManagerView(FileManagerView&&)                 = default;
    FileManagerView(const FileManagerView&)            = default;
    FileManagerView& operator=(FileManagerView&&)      = delete;
    FileManagerView& operator=(const FileManagerView&) = delete;
    ~FileManagerView() override                        = default;

    // 内部绘制逻辑 (Clay/ImGui)
    void onUpdate(LayoutContext& layoutContext,
                  UIManager*     sourceManager) override;

private:
};

}  // namespace MMM::UI
