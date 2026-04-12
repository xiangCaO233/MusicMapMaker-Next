#pragma once

#include "event/core/EventBus.h"
#include "ui/ISubView.h"
#include <filesystem>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace MMM::Event
{
struct GLFWDropEvent;
}

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
    ~FileManagerView() override;

    // 内部绘制逻辑 (Clay/ImGui)
    void onUpdate(LayoutContext& layoutContext,
                  UIManager*     sourceManager) override;

private:
    void drawDirectoryRecursive(const std::filesystem::path& path,
                                UIManager*                   sourceManager);
    void openFolderPicker();

    std::filesystem::path m_currentRoot;

    struct PendingDrop {
        std::vector<std::string> paths;
        glm::vec2                pos;
    };
    std::vector<PendingDrop> m_pendingDrops;
    Event::SubscriptionID    m_dropSubId;
};

}  // namespace MMM::UI
