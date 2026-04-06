#pragma once

#include "ui/ISubView.h"
#include <filesystem>

namespace MMM::UI
{

class FileManagerView : public ISubView
{
public:
    FileManagerView(const std::string& subViewName);
    FileManagerView(FileManagerView&&)                 = default;
    FileManagerView(const FileManagerView&)            = default;
    FileManagerView& operator=(FileManagerView&&)      = delete;
    FileManagerView& operator=(const FileManagerView&) = delete;
    ~FileManagerView() override                        = default;

    // 内部绘制逻辑 (Clay/ImGui)
    void onUpdate(LayoutContext& layoutContext,
                  UIManager*     sourceManager) override;

private:
    void drawDirectoryRecursive(const std::filesystem::path& path);
    void openFolderPicker();

    std::filesystem::path m_currentRoot;
};

}  // namespace MMM::UI
