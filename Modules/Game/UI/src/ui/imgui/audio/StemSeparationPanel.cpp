#include "StemSeparationPanel.h"

#ifndef __ANDROID__

#    include "StemModelCatalog.h"
#    include "StemSeparationJob.h"
#    include "audio/AudioManager.h"
#    include "audio/StemAudition.h"
#    include "audio/StemPlaybackStorage.h"
#    include "config/Utf8Path.h"
#    include "runtime/AppThreadPool.h"
#    include "ui/utils/NativeFileDialog.h"
#    include "ui/utils/UIWidgetUtils.h"

#    include <ice/thread/ThreadPool.hpp>
#    include <imgui.h>

#    include <algorithm>
#    include <array>
#    include <cfloat>
#    include <chrono>
#    include <cstddef>
#    include <cstdint>
#    include <expected>
#    include <filesystem>
#    include <future>
#    include <memory>
#    include <optional>
#    include <stop_token>
#    include <string>
#    include <string_view>
#    include <system_error>
#    include <utility>
#    include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 管理同一项目主音轨的分离与谱面播放模式。
/// @details 分轨状态按源文件共享；控制器关闭后，使用该主轨的谱面仍可播放。
/// @details 目录条目与音源缓存采用不同身份。模型目录描述可执行环境，
/// 同一模型生成的声部则按项目音源保存；卸载模型不使已有声部立即失效。
/// 模型选择和页签状态属于窗口；播放模式、通道设置和声部目录属于共享音源。
/// 文件层负责模型所有权和缓存有效性校验，窗口只根据最近一次结果提供操作。
/// 在渲染线程统一消费 future，后台任务不直接调用 ImGui 或写入窗口成员。
class MainTrackStemPanel final : public StemSeparationPanel
{
public:
    /// @brief 停止本窗口尚未完成的作业。
    /// @details 作业持有自己的路径及停止令牌，释放控制器时无需等待。
    /// @details 模型缓存维护与窗口无关，已经确认的清理继续由文件线程完成。
    /// 尚未确认的目录操作仅在本实例保存，销毁时不会产生删除请求。
    ~MainTrackStemPanel() override
    {
        m_separationStop.request_stop();
        m_deploymentStop.request_stop();
    }

    /// @brief 绑定当前主轨，在同一控制器内绘制分离与混音。
    /// @warning UI 热路径。路径只在实际切换资源时更新；
    /// 后台结果仅在就绪时取回，窗口折叠不会阻塞进度。
    /// @param root 项目根目录，和资源相对路径一起决定逻辑音源。
    /// @param relativePath 控制器宿主提供的项目内 UTF-8 路径。
    /// @param dpiScale 当前宿主内容缩放，应用于模型目录和混音控件的尺寸。
    /// @details 模型绑定是应用级状态，重绑主轨只重置该音源的播放与缓存视图。
    /// 停止旧分离与切换展示是两个独立动作，文件线程不持有面板地址。
    void render(const std::filesystem::path& root,
                const std::string& relativePath, float dpiScale) override
    {
        // 重命名或资源重载时，任何尚未发布的分轨都只属于旧输入。
        if ( root != m_root || relativePath != m_relativePath ) {
            // 停止信号可能晚于推理完成，因此还要标记本次结果不可用。
            m_separationStop.request_stop();
            m_discardLoad  = true;
            m_root         = root;
            m_relativePath = relativePath;
            m_sourcePath =
                Config::pathToUtf8(root / Config::utf8ToPath(relativePath));
            m_stemSource =
                Audio::AudioManager::instance().getStemPlaybackSource(
                    m_sourcePath);
            // 非活动谱面或尚未打开的主轨也可从面板恢复；同源后台准备自动去重。
            auto& runtime = Runtime::AppThreadPool::instance();
            if ( auto* files = runtime.getFileThreadPool();
                 files && runtime.get() && m_stemSource ) {
                files->enqueue([source = m_stemSource,
                                path   = m_sourcePath,
                                decode = runtime.get()] {
                    source->restore(path, *decode);
                });
            }
            // 后台恢复可能在首次绘制之后到达。先清掉上个音源的页签记忆，
            // 之后由已发布的播放模式与标签栏重新建立对应关系。
            m_restorePlaybackTab = true;
            m_lastPlaybackTab.reset();
            // 旧作业只持有原绑定的快照，切换后不再显示它们的状态。
            // 缓存列表、查询和动作都只对应当前主轨。模型安装与清理结果
            // 仍由本窗口接收，因为这些操作在换歌之后依然针对同一个目录。
            // 释放本地 future 不要求后台停工，闭包从提交时就有独立的路径。
            m_cacheInfo    = {};
            m_cacheQuery   = {};
            m_cacheAction  = {};
            m_refreshCache = true;
            m_cacheError.clear();
            m_error.clear();
        }
        // 就绪的部署或删除可能改变本机绑定，先消费结果再绘制和读取共享序号。
        // 面板折叠只影响可见性，不应使已经运行的文件作业无人接收完成状态。
        // 先收取结果，成功后分轨页可用；模式仍由用户切换。
        pollJobs();
        const bool expanded = ImGui::CollapsingHeader("声部分离");
        if ( expanded ) {
            // 重新展开可读取其他音轨控制器新部署的模型。
            if ( !m_expanded ||
                 m_bindingsRevision != stemModelBindingsRevision() )
                restoreBindings();
            drawWorkspace(dpiScale);
        } else {
            m_cacheVisible = false;
        }
        drawModelRemovalConfirmation(dpiScale);
        m_expanded = expanded;
    }

private:
    /// @brief 展开面板时刷新可见绑定，首次选择一个可用模型。
    /// @warning 仅展开边沿或共享绑定序号变化时读取配置和校验目录。
    /// @details 共享序号用于通知窗口刷新；各窗口仍持有独立的当前模型选择。
    /// 保留未改绑目录对应的已选模型，避免因其他行变化打断当前工作。
    /// @post 已变更绑定所对应的活动清单与解释器均失效，磁盘占用等待重新扫描。
    void restoreBindings()
    {
        // 读取最新目录只刷新目录状态，不把用户手工选中的模型自动切走。
        m_bindingsRevision = stemModelBindingsRevision();
        auto paths         = readStemModelBindings();
        if ( m_selectedCatalog &&
             paths[*m_selectedCatalog] != m_bindings[*m_selectedCatalog] ) {
            // 另一个控制器卸载或改绑后，旧环境不能再次发起推理。
            m_selectedCatalog.reset();
            m_model.reset();
            m_boundEnvironment.reset();
        }
        // 路径先更新，大小快照整体失效。否则目录行会短暂把另一部署的大小
        // 与刚加载的路径配在一起，也可能使用旧扫描的部署归属判断删除方式。
        m_bindings            = std::move(paths);
        m_modelStorage        = {};
        m_refreshModelStorage = true;
        if ( !m_environment ) {
            // 自定义模型与目录中各项使用不同设置文件，旧 MSST 入口不做迁移。
            m_environment = loadStemSeparationEnvironment();
            // 绑定文件只保存路径。启动时仍要解析当前清单，以免目录搬迁后
            // 自动选中已经不能使用的模型。
            // 已绑定的首个模型可直接使用，手动导入仍保留单独运行设置。
            // 一处失效不会阻止其余模型恢复；先选首个有效条目。
            for ( std::size_t i = 0; i < m_bindings.size(); ++i ) {
                if ( m_bindings[i].empty() ) continue;
                auto bound =
                    bindStemModel(STEM_MODEL_CATALOG[i], m_bindings[i]);
                if ( bound ) {
                    // 启动时恢复绑定不写盘，其他窗口的设置保持原有顺序。
                    useBoundModel(i, std::move(*bound), false);
                    break;
                }
            }
        }
    }

    /// @brief 消费部署和分离结果，保留当前主轨的有效输出。
    /// @warning UI 热路径。只轮询已提交任务，禁止等待后台工作。
    /// @details 分离和缓存操作绑定到音源，模型安装与清理绑定到目录条目。
    /// 主轨变更后，模型任务的结果仍有效；被丢弃的旧音源结果不更新当前页。
    /// @post 每个已就绪的 future 只消费一次，新的操作可以在下一次绘制提交。
    void pollJobs()
    {
        using namespace std::chrono_literals;
        if ( m_load.valid() &&
             m_load.wait_for(0s) == std::future_status::ready ) {
            // 任务提交后路径发生变化时只清空 future，不播放旧主轨。
            auto result = m_load.get();
            if ( !m_discardLoad ) {
                // 即使发生部分文件删除，也刷新占用，避免把旧统计继续当作实际大小。
                // 长期绑定是否改变由共享绑定版本通知，其状态不由错误消息推断。
                if ( !result ) m_error = result.error();
                // 返回成功但未持久化时，后台已经发布了可播放结果；
                // 这里仅提示磁盘写入失败，内存中的声部可以照常试听。
                else if ( !*result )
                    m_error = "分离结果可在本次运行使用，但写入本机缓存失败。";
                m_refreshCache = true;
            }
        }
        if ( m_cacheQuery.valid() &&
             m_cacheQuery.wait_for(0s) == std::future_status::ready ) {
            // 空可选值代表查询失败，成功但没有目录由空容器表达。
            // 两种状态分别进入错误提示和空列表，便于判断能否手动刷新。
            m_cacheInfo = m_cacheQuery.get();
            if ( !m_cacheInfo && m_cacheError.empty() )
                m_cacheError = "无法读取分轨缓存。";
        }
        if ( m_cacheAction.valid() &&
             m_cacheAction.wait_for(0s) == std::future_status::ready ) {
            // 同一个 future 承接载入和清理；操作类型保留到取回结果，
            // 这样普通系统错误才能对应到用户刚才按下的按钮。
            const auto error = m_cacheAction.get();
            if ( error == std::errc::device_or_resource_busy )
                m_cacheError = "此音轨有其他缓存或分离任务，请稍后重试。";
            else if ( error )
                m_cacheError = m_activatingCache
                                   ? "无法使用该分轨缓存，请检查文件是否完整。"
                                   : "删除分轨缓存失败，请检查目录权限后重试。";
            else {
                // 载入可能换模型，删除也可能退回原版。重新读取已发布目录，
                // 让下次绘制根据新目录生成标签。
                m_labelCatalog.reset();
                m_restorePlaybackTab = true;
            }
            m_refreshCache = true;
        }
        if ( m_modelStorageScan.valid() &&
             m_modelStorageScan.wait_for(0s) == std::future_status::ready ) {
            // 重新打开目录或收到绑定变化时，下一批查询按新路径重建整表。
            // 已完成的旧任务仍需取走，才能释放唯一的扫描提交位置。
            auto storage = m_modelStorageScan.get();
            // 换绑后丢弃旧扫描，避免显示路径和大小来自不同的部署目录。
            if ( !m_refreshModelStorage ) m_modelStorage = std::move(storage);
        }
        if ( m_modelRemoval.valid() &&
             m_modelRemoval.wait_for(0s) == std::future_status::ready ) {
            auto result = m_modelRemoval.get();
            if ( !result ) m_error = result.error();
            m_refreshModelStorage = true;
        }
        // 部署不依赖主轨路径。面板折叠时也可保存已就绪的模型。
        if ( m_deployment.valid() &&
             m_deployment.wait_for(0s) == std::future_status::ready ) {
            // 取走 future 后可重新发起任务。发布时使用提交那一刻的行号，
            // 目录表当前的选择不会改变持久化键。
            auto deployed = m_deployment.get();
            if ( deployed )
                useBoundModel(m_deployingIndex, std::move(*deployed), true);
            else
                // 错误留在分离页，用户可重试或改绑目录。
                m_error = deployed.error();
        }
    }

    /// @brief 分离结果是否已落盘，失败原因在 UI 线程显示。
    /// @details 有值时表示后台已发布混音，布尔值仅区分是否写出了设置位置。
    /// 没有值时只回传错误说明，界面不根据错误推断或改写当前播放模式。
    using LoadResult = std::expected<bool, std::string>;

    /// @brief 切换原版、分轨播放和模型设置。
    /// @warning UI 热路径；仅在播放页改变时发布新状态。
    /// @param dpiScale 混音表和设置页共用的窗口内容倍率。
    /// @details 目录存在时才有分轨播放页；设置页用于管理模型与缓存，
    /// 不参与原版／分轨的互斥选择，也不停止当前播放。
    /// 同一帧选择页签和绘制混音可读取各自发布的不可变状态。
    void drawWorkspace(float dpiScale)
    {
        if ( !m_stemSource ) return;
        const auto current = m_stemSource->mix();
        // 同源的其他窗口也可能删除缓存，释放本窗口保留的目录引用。
        if ( !current->catalog ) m_labelCatalog.reset();
        // 同一个源可能有不止一个控制器。另一处切换后同步当前播放页；
        // 设置页没有选中的播放页，因此仍停在原来的设置界面。
        if ( m_lastPlaybackTab && *m_lastPlaybackTab != current->useStems )
            m_restorePlaybackTab = true;
        // 用源路径隔离 ImGui 的上一次选页，避免切换主音轨时沿用另一首歌的标签。
        ImGui::PushID(m_sourcePath.c_str());
        bool cacheVisible = false;
        if ( ImGui::BeginTabBar("StemWorkspace") ) {
            if ( ImGui::BeginTabItem(
                     "原版播放",
                     nullptr,
                     playbackTabFlags(false, current->useStems)) ) {
                selectPlaybackMode(false, *current);
                ImGui::EndTabItem();
            }
            // 没有可用分轨目录时省略整个标签，其永久选择由音源恢复流程处理。
            // 已有原版页保持可用，清理当前缓存后无需保留失效的混音控件。
            if ( current->catalog &&
                 ImGui::BeginTabItem(
                     "分轨播放",
                     nullptr,
                     playbackTabFlags(true, current->useStems)) ) {
                selectPlaybackMode(true, *current);
                drawTrackControls(dpiScale);
                ImGui::EndTabItem();
            }
            if ( ImGui::BeginTabItem("分离设置") ) {
                // 返回设置页后，下次进入播放页算作新的模式选择。
                m_lastPlaybackTab.reset();
                drawSetup(*current, dpiScale);
                cacheVisible = true;
                if ( current->catalog )
                    ImGui::TextUnformatted("分离完成，可切换到分轨播放。");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        // 写入发生在各页绘制结束之后，让设置页能观察上一帧的可见性，
        // 因而只有重新进入时才需要因可见性变化提交磁盘统计。
        m_cacheVisible = cacheVisible;
        ImGui::PopID();
    }

    /// @brief 控制器重新绑定时恢复已发布的播放页。
    /// @warning UI 热路径；只读取模式标量，不复制目录。
    /// @param stems 当前正在提交给页签栏的播放页类型。
    /// @param current 音源已发布的播放模式。
    /// @return 恢复尚未完成且两者一致时请求选中，普通帧尊重用户导航。
    ImGuiTabItemFlags playbackTabFlags(bool stems, bool current) const
    {
        return m_restorePlaybackTab && current == stems
                   ? ImGuiTabItemFlags_SetSelected
                   : ImGuiTabItemFlags_None;
    }

    /// @brief 选中播放页时只改变同源主音轨的模式，设置页不改变声音。
    /// @warning UI 热路径；只有明确的模式变化才复制并发布配置。
    /// @param stems 用户进入的播放页所代表的模式。
    /// @param current 当前绘制帧观察到的共享混音。
    /// @details 模式恢复和用户选择共用页签入口，需要等目标页实际成为活动页
    /// 才清除恢复标志，避免初次打开时用默认页覆写持久化设置。
    void selectPlaybackMode(bool stems, const Audio::StemPlaybackMix& current)
    {
        if ( m_restorePlaybackTab ) {
            // ImGui 可能先绘制默认标签再处理 SetSelected，此帧保留已存模式。
            if ( stems == current.useStems ) {
                m_restorePlaybackTab = false;
                m_lastPlaybackTab    = stems;
            }
            return;
        }
        // 页签静止时不覆盖另一处对同一来源提交的设置。
        if ( m_lastPlaybackTab == stems ) return;
        // 记录的是本地已经呈现的页签，不代表一定发生了一次音频事务。
        // 当前模式相同或有效目录刚消失时无需向音频层发布。
        m_lastPlaybackTab = stems;
        if ( stems == current.useStems || !current.catalog ) return;
        // 只修改播放模式，保留当前缓存身份和通道控制，切回分轨时可继续
        // 使用先前的电平、静音与独听设置，而不必重新创建声部目录。
        auto edited     = current;
        edited.useStems = stems;
        if ( m_stemSource->publishSettings(std::move(edited)) )
            m_stemSource->queueSettingsSave();
    }

    /// @brief 排列模型、音源及当前作业，不在混音区重复显示部署控件。
    /// @details 同一窗口一次只准备一个任务。运行时可取消当前部署或推理。
    /// 部署结果只参与模型选择，不更换本次分离的来源。
    /// @warning UI 热路径，仅在用户操作时提交命令。
    /// @param current 用于说明缓存是否正被音源使用的混音快照。
    /// @param dpiScale 传递给模型目录的布局参数。
    /// @details 缓存错误独立于部署、绑定和推理错误，避免一次后台失败
    /// 覆盖另一块区域仍需显示的操作结果。
    void drawSetup(const Audio::StemPlaybackMix& current, float dpiScale)
    {
        ImGui::Spacing();
        drawCacheControls(current);
        ImGui::Separator();
        // 后台任务持有已经冻结的输入，禁用操作可避免进度与新选择相互矛盾。
        // 取消按钮留在禁用区域外，无需中断播放即可结束文件任务。
        ImGui::BeginDisabled(m_load.valid() || m_deployment.valid() ||
                             m_cacheAction.valid() || m_modelRemoval.valid());
        drawSeparationControls(dpiScale);
        ImGui::EndDisabled();

        // 安装和推理的取消信号独立，各自只影响当前作业。
        // 安装包含依赖解析和下载，不把单一阶段的字节数作为总进度。
        // 取消只请求停止。对应 future 仍保持忙状态，直到工作线程退出，
        // 避免部署目录仍在写入时又允许该行开始新的部署或删除。
        if ( m_deployment.valid() ) {
            ImGui::TextUnformatted("正在下载权重并安装独立环境…");
            ImGui::SameLine();
            if ( FeedbackSmallButton("取消部署") )
                m_deploymentStop.request_stop();
        }
        if ( m_load.valid() ) {
            ImGui::TextUnformatted("正在分离主音轨…");
            if ( FeedbackButton("取消分离") ) m_separationStop.request_stop();
        }
        if ( m_modelRemoval.valid() ) ImGui::TextUnformatted("正在移除模型…");
        // 统一在触发入口下方报告结果，失败时不清理已有效的模型或音轨。
        if ( !m_error.empty() ) ImGui::TextWrapped("%s", m_error.c_str());
    }

    /// @brief 展示当前音轨的所有模型缓存，由后台任务提供统计结果。
    /// @warning UI 热路径；仅在可见性变化、任务结束或按下刷新时提交查询。
    /// @param current 只用于比较当前缓存身份，不作为目录扫描输入。
    /// @details 保留最近一次成功读取的列表供渲染，忙状态由 future 表达。
    /// 手动刷新和动作结束均设置同一个请求位，便于把重叠刷新合成一次。
    void drawCacheControls(const Audio::StemPlaybackMix& current)
    {
        if ( (!m_cacheVisible || m_refreshCache) && !m_cacheQuery.valid() &&
             !m_cacheAction.valid() ) {
            m_refreshCache = false;
            updateCacheInfo();
        }

        // 查询和修改串行化，防止在删除或更换结果期间展示中间目录状态。
        // 此处仅冻结相关入口；主轨播放继续使用已发布的状态。
        ImGui::TextUnformatted("分轨缓存");
        const bool busy = m_cacheAction.valid() || m_cacheQuery.valid();
        ImGui::BeginDisabled(busy);
        if ( FeedbackSmallButton("刷新缓存信息") ) {
            m_cacheError.clear();
            m_refreshCache = true;
        }
        ImGui::EndDisabled();
        if ( m_cacheQuery.valid() ) ImGui::TextDisabled("正在统计…");
        if ( m_cacheAction.valid() )
            ImGui::TextDisabled("%s",
                                m_activatingCache ? "正在载入…" : "正在删除…");
        if ( !m_cacheError.empty() )
            ImGui::TextWrapped("%s", m_cacheError.c_str());
        // 查询完成前没有列表快照，等候提示已足够；空列表表示查询成功，
        // 不能用它替代读取失败或仍在排队的状态。
        if ( !m_cacheInfo ) return;
        if ( m_cacheInfo->empty() )
            ImGui::TextDisabled("当前音轨暂无分轨缓存。");
        for ( const auto& cache : *m_cacheInfo ) {
            // 同名模型的缓存目录仍有独立的控件和确认窗口。
            ImGui::PushID(cache.modelKey.c_str());
            drawCacheEntry(cache, current, busy || m_load.valid());
            ImGui::PopID();
        }
    }

    /// @brief 以二进制单位显示目录中文件的总大小。
    /// @warning 缓存与模型目录共用展示格式，不做文件访问。
    /// @param bytes 由文件层计算的原始字节数，避免面板对目录再次遍历。
    /// @details 小量用精确字节数，大量用二进制单位便于在不同模型行横向比较。
    /// 转为浮点后再换算，保留显示所需的小数位。
    static void drawDiskUsage(std::uintmax_t bytes)
    {
        if ( bytes < 1024 )
            ImGui::Text("占用：%llu B", static_cast<unsigned long long>(bytes));
        else if ( bytes < 1024 * 1024 )
            ImGui::Text("占用：%.2f KiB", static_cast<double>(bytes) / 1024);
        else if ( bytes < 1024 * 1024 * 1024 )
            ImGui::Text("占用：%.2f MiB",
                        static_cast<double>(bytes) / (1024 * 1024));
        else
            ImGui::Text("占用：%.2f GiB",
                        static_cast<double>(bytes) / (1024 * 1024 * 1024));
    }

    /// @brief 展示一个模型的本机文件，并将操作限制在该模型。
    /// @warning UI 热路径；只使用后台已取得的统计，不访问文件系统。
    /// @param cache 当前行的独立缓存键、目录和使用资格。
    /// @param current 决定该行是否在用、删除是否会回退播放模式。
    /// @param busy 调用方合成的忙状态，包含缓存任务和当前音源的推理。
    /// @details 已过期的结果仍可清理，但禁止直接用于源文件已变化的音轨。
    void drawCacheEntry(const Audio::StemPlaybackCacheInfo& cache,
                        const Audio::StemPlaybackMix& current, bool busy)
    {
        constexpr const char* confirmation = "删除此模型缓存";
        const bool            selected =
            current.catalog && current.cacheModelKey == cache.modelKey;
        ImGui::Separator();
        ImGui::TextWrapped("%s", cache.modelName.c_str());
        if ( selected ) ImGui::TextDisabled("当前使用");
        ImGui::TextWrapped("位置：%s", cache.directory.c_str());
        drawDiskUsage(cache.bytes);
        if ( !cache.reusable )
            ImGui::TextDisabled("此缓存无法用于当前版本的原音频。");

        // 已选缓存无须重复载入。忙时也禁止开始删除，防止一次刷新后的
        // 旧按钮命中把正在准备的结果从磁盘移除。
        ImGui::BeginDisabled(busy || selected || !cache.reusable);
        if ( FeedbackSmallButton("使用此缓存") )
            startCacheAction(cache.modelKey, true);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        if ( FeedbackSmallButton(confirmation) ) ImGui::OpenPopup(confirmation);
        ImGui::EndDisabled();

        // 删除确认复用行内缓存键的作用域；同名模型可以保有不同结果。
        // 若该条目正在使用，额外说明切回原版是清理后的即时影响。
        if ( ImGui::BeginPopupModal(
                 confirmation, nullptr, ImGuiWindowFlags_AlwaysAutoResize) ) {
            ImGui::TextWrapped("删除「%s」的分轨结果和调音台设置？",
                               cache.modelName.c_str());
            if ( selected ) ImGui::TextUnformatted("当前音轨将切回原版播放。");
            if ( FeedbackButton("确认删除") ) {
                startCacheAction(cache.modelKey, false);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( FeedbackButton("取消") ) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    /// @brief 查询磁盘状态，文件作业不借用界面或模型配置。
    /// @warning 低频提交；目录遍历由文件线程处理。
    /// @details 队列闭包固定提交时的音源路径；切换音轨时 UI 丢弃原 future，
    /// 工作线程仍可正常收尾，且不会把旧路径结果填到新音源列表。
    /// 找不到执行器时，维持任务未开始的状态并提示本次查询无法提交。
    void updateCacheInfo()
    {
        if ( auto* files =
                 Runtime::AppThreadPool::instance().getFileThreadPool() )
            m_cacheQuery = files->enqueue([path = m_sourcePath] {
                return Audio::readStemPlaybackCache(path);
            });
        if ( !m_cacheQuery.valid() ) m_cacheError = "无法提交缓存查询。";
    }

    /// @brief 后台删除或复用当前音轨的一个模型结果。
    /// @warning 仅控件触发；关闭窗口后作业仍持有原播放源。
    /// @param modelKey 落盘缓存的稳定身份，不能用可重复的模型显示名代替。
    /// @param activate 为真时准备分轨播放，否则只清理指定结果。
    /// @details 载入需使用解码池准备声部，删除只走文件池。
    /// 音源由闭包持有，后续重新绑定控制器不会改变已提交任务的操作对象。
    void startCacheAction(const std::string& modelKey, bool activate)
    {
        m_cacheError.clear();
        m_activatingCache = activate;
        auto& runtime     = Runtime::AppThreadPool::instance();
        auto* decode      = runtime.get();
        if ( auto* files = runtime.getFileThreadPool();
             files && (!activate || decode) )
            // 只有载入分支解引用解码池；清理仍可在解码池未就绪时运行。
            m_cacheAction = files->enqueue([source = m_stemSource,
                                            path   = m_sourcePath,
                                            modelKey,
                                            activate,
                                            decode] {
                return activate ? source->activateCachedPlayback(
                                      path, modelKey, *decode)
                                : source->deleteCachedPlayback(path, modelKey);
            });
        if ( !m_cacheAction.valid() ) m_cacheError = "无法提交缓存操作。";
    }

    /// @brief 只在用户点击后调用原生选择器，接管 UTF-8 路径后释放 NFD 内存。
    /// @param filter 可选的文件类型限制，空值允许模型环境和音频的各平台扩展名。
    /// @return 已拷贝的路径，取消或对话框失败时返回空。
    /// @warning 选择对话框可等待用户，禁止从普通渲染帧无条件调用。
    std::string pickFile(const nfdu8filteritem_t* filter = nullptr)
    {
        nfdu8char_t* path = nullptr;
        const auto   status =
            NativeFileDialog::openFile(&path, filter, filter ? 1 : 0, nullptr);
        // 取消是正常交互，继续保留此前的路径；只有 NFD 错误需要提示。
        // 额外检查空指针，向上游始终返回可安全处理的字符串。
        if ( status != NFD_OKAY || !path ) {
            if ( status == NFD_ERROR ) m_error = "无法打开文件选择器。";
            return {};
        }
        // NFD 使用自己的分配方式。复制之后立即使用对应的释放 API。
        std::string selected(path);
        NFD_FreePathU8(path);
        return selected;
    }

    /// @brief 切换到目录里的模型，必要时写入该模型的长期绑定。
    /// @details 外部 MSST 入口配置单独维护。目录模型总是使用其自身解释器。
    /// @param index 目录稳定顺序中的模型位置。
    /// @param bound 已冻结的模型计划、解释器和目录。
    /// @param persist 首次绑定或部署成功时写盘；切换既有条目时为 false。
    /// @warning 写盘仅发生在用户首次绑定或部署完成时。
    /// @post 活动清单、解释器以及目录行索引共同指向同一部署。
    /// @details 磁盘写入失败时，已解析的运行计划仍可供当前控制器使用。
    /// 占用统计对应实际路径，换绑后需重新扫描。
    void useBoundModel(std::size_t index, BoundStemModel bound, bool persist)
    {
        // 环境与模型一起替换，保持下一次推理的清单和解释器归属一致。
        m_error.clear();
        m_model            = std::move(bound.model);
        m_selectedCatalog  = index;
        m_boundEnvironment = std::move(bound.environment);
        // 不能继续使用不同目录的归属信息，否则移除按钮可能错误地
        // 将手动绑定的外部目录当作应用管理的安装。
        if ( m_bindings[index] != bound.directory ) {
            m_modelStorage[index].reset();
            m_refreshModelStorage = true;
        }
        m_bindings[index] = std::move(bound.directory);
        // 若配置目录不可写，保持本次会话可用，提示重启时会丢失新绑定。
        if ( persist && !saveStemModelBinding(STEM_MODEL_CATALOG[index],
                                              m_bindings[index]) ) {
            m_error = "模型已就绪，但无法保存目录绑定。";
        }
    }

    /// @brief 弹出目录选择器，交由对应流程处理已部署或待部署的位置。
    /// @param index 目录行号，在原生选择器打开期间保持稳定。
    /// @param deploy 为 true 时选择部署父目录，否则定位完整模型目录。
    /// 文件池拿到目录后的错误通过部署结果回传，选择失败则直接显示。
    /// @warning 用户点击触发，选择器可等待用户输入。
    /// @details 绑定读取已有清单并在原路径使用；部署将工作交给后台安装器。
    /// 两条路径共用一个原生选择器，但只有部署需要创建新的停止令牌。
    /// @post 取消选择或绑定校验失败时，不改变当前模型及其解释器。
    void chooseModelDirectory(std::size_t index, bool deploy)
    {
        // 部署与绑定均使用目录选择器，模型清单的路径结构由后端管理。
        nfdu8char_t* path   = nullptr;
        const auto   status = NativeFileDialog::pickFolder(&path, nullptr);
        // 用户取消直接返回，原绑定及当前混音均保留。
        if ( status != NFD_OKAY || !path ) {
            if ( status == NFD_ERROR ) m_error = "无法打开目录选择器。";
            return;
        }
        // 原生缓冲区在选择完成后由 NFD 释放；工作池拿到自己的 UTF-8 副本。
        std::string selected(path);
        NFD_FreePathU8(path);
        if ( !deploy ) {
            // 绑定只核对目录文件，不重装依赖也不执行用户环境中的 Python。
            auto bound = bindStemModel(STEM_MODEL_CATALOG[index], selected);
            if ( bound )
                useBoundModel(index, std::move(*bound), true);
            else
                m_error = bound.error();
            return;
        }
        // 选择器可能长时间打开，只在确认路径后才查找文件执行器。
        auto* pool = Runtime::AppThreadPool::instance().getFileThreadPool();
        if ( !pool ) {
            m_error = "文件后台服务尚未就绪。";
            return;
        }
        // 保存行索引用于完成后显示和绑定，后台只捕获值与稳定条目。
        m_deployingIndex = index;
        m_deploymentStop = std::stop_source{};
        m_error.clear();
        // 不让文件线程拿到窗口引用，窗口销毁只需发布停止信号。
        m_deployment = pool->enqueue([entry  = STEM_MODEL_CATALOG[index],
                                      parent = std::move(selected),
                                      stop   = m_deploymentStop.get_token()] {
            return deployStemModel(entry, parent, stop);
        });
        // 停机阶段可以拒绝投递，空 future 不能驱动部署进度。
        if ( !m_deployment.valid() ) m_error = "无法提交部署任务。";
    }

    /// @brief 绘制同一模型的标识、环境和声部说明，标题可重新选中已有绑定。
    /// @warning UI 热路径；只有用户选择模型时才重新验证文件。
    /// @details 行正文同时用于计算右侧操作的垂直对齐，因此占用未知时
    /// 也提交一行状态文本，便于区分尚在扫描与统计失败。
    /// @param index 由目录顺序确定，仅用于索引本帧的绑定、状态与标签。
    /// @pre 调用方已压入稳定模型 ID，并定位在说明单元格。
    void drawCatalogDetails(std::size_t index)
    {
        const auto& entry     = STEM_MODEL_CATALOG[index];
        const bool  bound     = !m_bindings[index].empty();
        const bool  selected  = m_selectedCatalog == index;
        const float available = ImGui::GetContentRegionAvail().x;
        // 优先用完整名；只有确实放不下才用目录内预留的短标签。
        const char* label =
            entry.catalogName && ImGui::CalcTextSize(entry.name).x > available
                ? entry.catalogName
                : entry.name;
        const auto labelSize = ImGui::CalcTextSize(label);
        if ( bound ) {
            const float width = std::min(available, labelSize.x);
            if ( FeedbackSelectable(
                     label, selected, 0, { width, labelSize.y }) ) {
                // 重新读取绑定，兼容用户在 MMM 之外更新模型计划。
                auto model = bindStemModel(entry, m_bindings[index]);
                if ( model )
                    useBoundModel(index, std::move(*model), false);
                else
                    m_error = model.error();
            }
        } else {
            ImGui::TextWrapped("%s", label);
        }
        if ( label == entry.catalogName )
            ImGui::SetItemTooltip("%s", entry.name);

        // 当前选择比绑定状态更具体，名称较长时状态另起一行，
        // 避免辅助信息挤掉主标签或超出实际列宽。
        const char* status = selected ? "当前模型"
                             : bound  ? "已绑定"
                                      : "未部署";
        const float gap    = ImGui::GetStyle().ItemSpacing.x;
        if ( labelSize.y <= ImGui::GetTextLineHeight() &&
             labelSize.x + gap + ImGui::CalcTextSize(status).x <= available )
            ImGui::SameLine(0.0F, gap);
        ImGui::TextDisabled("%s", status);
        if ( bound ) ImGui::SetItemTooltip("%s", m_bindings[index].c_str());

        if ( bound ) {
            if ( const auto& storage = m_modelStorage[index] )
                drawDiskUsage(storage->bytes);
            else
                ImGui::TextDisabled(
                    "%s",
                    m_modelStorageScan.valid() ? "正在统计占用…" : "占用未知");
        }
        ImGui::TextWrapped("%s", entry.stems);
        // 弱化补充说明，主信息和正文仍使用当前皮肤的对比度。
        auto descriptionColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        descriptionColor.w *= 0.8F;
        ImGui::PushStyleColor(ImGuiCol_Text, descriptionColor);
        ImGui::TextWrapped("%s", entry.strengths);
        ImGui::PopStyleColor();
    }

    /// @brief 等宽操作按钮按可用宽度并排或堆叠，避免窄面板裁剪绑定入口。
    /// @warning UI 热路径；只有按钮激活时进入原生目录选择器。
    /// @param index 操作所针对的模型行。
    /// @param minWidth 单个操作可完整显示较长标签的最小宽度。
    /// @param height 该行统一采用的按钮高度。
    /// @param detailsHeight 从左侧实际绘制结果取得；窄布局传零避免额外偏移。
    /// @param dpiScale 用于在高 DPI 主题下维持可见的边框厚度。
    /// @details 操作轮廓只在本行按钮有效，表内说明和其他音频控件继承原主题。
    void drawCatalogActions(std::size_t index, float minWidth, float height,
                            float detailsHeight, float dpiScale)
    {
        const float available =
            std::max(1.0F, ImGui::GetContentRegionAvail().x);
        const float gap           = ImGui::GetStyle().ItemSpacing.x;
        const bool  inlineButtons = available >= minWidth * 2.0F + gap;
        const float width =
            inlineButtons ? (available - gap) * 0.5F : available;
        // 宽面板以左侧说明的实际换行高度居中，窄面板在说明下另起一行。
        const bool bound = !m_bindings[index].empty();
        const int  rows  = (inlineButtons ? 1 : 2) + (bound ? 1 : 0);
        // 先确定实际按钮行数，再按整组高度居中；删除或解绑占满
        // 下一行，不能把它的空间当成部署和绑定两键的间隔。
        const float actionsHeight =
            rows * height + (rows - 1) * ImGui::GetStyle().ItemSpacing.y;
        if ( detailsHeight > actionsHeight )
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                                 (detailsHeight - actionsHeight) * 0.5F);
        // 默认按钮底色可能与斑马纹的一行相同。用主题前景色叠出统一轮廓，
        // 让边界在两种行底色上都可辨，同时保留悬浮、按下和禁用的原有反馈。
        auto border = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        border.w *= 0.32F;
        ImGui::PushStyleColor(ImGuiCol_Border, border);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,
                            std::max(ImGui::GetStyle().FrameBorderSize,
                                     std::max(1.0F, dpiScale)));
        // 统一文字中心，并限制主题横向内边距。ImGui 的文字起点至少在
        // 左侧内边距之后，过大的内边距会在窄按钮里把长标签推向右边。
        const auto  framePadding = ImGui::GetStyle().FramePadding;
        const float labelWidth   = ImGui::CalcTextSize("绑定已有").x;
        const float paddingX     = std::min(
            framePadding.x, std::max(0.0F, (width - labelWidth) * 0.5F));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(paddingX, framePadding.y));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5F, 0.5F));
        if ( FeedbackButton("部署", { width, height }) )
            chooseModelDirectory(index, true);
        if ( inlineButtons ) ImGui::SameLine(0.0F, gap);
        if ( FeedbackButton("绑定已有", { width, height }) )
            chooseModelDirectory(index, false);
        if ( bound ) drawModelRemovalAction(index, { available, height });
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
    }

    /// @brief 记录用户点击的模型与删除范围，供目录外的模态确认。
    /// @warning UI 热路径，只有点击时复制绑定路径。
    /// @param index 正在绘制的绑定目录条目。
    /// @param size 删除或者解除绑定占用的整行按钮尺寸。
    /// @pre 调用方只为已绑定的行调用，磁盘扫描未完成时禁止建立确认请求。
    /// @details 目录缺少应用归属凭证时只允许解绑；确认文本与请求范围一致。
    void drawModelRemovalAction(std::size_t index, ImVec2 size)
    {
        const auto& storage = m_modelStorage[index];
        const bool  managed = storage && storage->managed;
        ImGui::BeginDisabled(m_modelStorageScan.valid());
        if ( FeedbackButton(managed ? "删除模型" : "解除绑定", size) ) {
            // 确认期间如果收到其他窗口改绑，仍保留用户点击时的目录。
            m_modelConfirmIndex         = index;
            m_modelConfirmPath          = m_bindings[index];
            m_modelConfirmRemoveFiles   = managed;
            m_modelConfirmOpenRequested = true;
        }
        ImGui::EndDisabled();
    }

    /// @brief 在音频控制器的视图层级统一确认删除和解除绑定。
    /// @warning UI 热路径；仅在有待确认操作时布局，不读取磁盘。
    /// @param dpiScale 当前音频控制器的内容倍率，用于统一窗口留白和操作行高度。
    /// @details 通过固定内部 ID 共用确认窗口，可见标题随将要执行的操作变化。
    /// 用户看到的路径和删除范围均来自点击快照，随后变化的扫描结果不改动它们。
    /// @post 取消清除待确认行，不提交清理，也不修改模型绑定。
    void drawModelRemovalConfirmation(float dpiScale)
    {
        if ( !m_modelConfirmIndex ) return;
        const char* title = m_modelConfirmRemoveFiles
                                ? "删除模型###StemModelRemoval"
                                : "解除模型绑定###StemModelRemoval";
        // 在模型表外打开并绘制，滚动、切列和收起目录不会改变确认框的 ID。
        if ( std::exchange(m_modelConfirmOpenRequested, false) )
            FeedbackOpenPopup(title);
        if ( !ImGui::IsPopupOpen(title) ) {
            m_modelConfirmIndex.reset();
            return;
        }

        const float                    font = ImGui::GetFontSize();
        Utils::CenteredModalPopupScope modal(dpiScale);
        // 宽度随字号缩放并由统一样式夹取到视口安全区，高度随内容换行。
        if ( !modal.begin(title,
                          nullptr,
                          ImGuiWindowFlags_NoMove,
                          { font * 26.0F, 0.0F }) )
            return;

        // 目录与影响分段显示，长路径绕行不会挤窄说明和底部按钮。
        // 受视口高度限制时，保留模态的原生纵向滚动以访问确认行。
        ImGui::TextWrapped("%s", STEM_MODEL_CATALOG[*m_modelConfirmIndex].name);
        ImGui::Spacing();
        ImGui::TextDisabled("模型目录");
        ImGui::TextWrapped("%s", m_modelConfirmPath.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped(
            "%s",
            m_modelConfirmRemoveFiles
                ? "将删除此模型的权重、独立环境和分离脚本。已生成的分轨缓存会保"
                  "留。"
                : "将解除此模型的目录绑定。目录里的文件会保留。");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 按钮随窄视口收缩，正常宽度下等宽靠右；默认焦点留给取消。
        // 使用模态实际内容区计算按钮宽度，避免 DPI 和窗口留白重复抵扣。
        // 内容滚动时宽度也会自然计入原生滚动条占用。
        const float available = ImGui::GetContentRegionAvail().x;
        const float gap       = ImGui::GetStyle().ItemSpacing.x;
        const float width =
            std::max(1.0F, std::min(font * 6.0F, (available - gap) * 0.5F));
        const float height =
            std::max(32.0F * dpiScale, ImGui::GetFrameHeight());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(0.0F, available - 2.0F * width - gap));
        const bool cancel = FeedbackButton("取消", { width, height });
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        const bool confirm =
            FeedbackButton(m_modelConfirmRemoveFiles ? "确认删除" : "确认解除",
                           { width, height });
        // 先提交后台任务再清掉 UI 请求，确保闭包已经复制确认所用的目录。
        // 若同帧收到两个按钮信号，以取消为准，避免目录操作误提交。
        if ( confirm && !cancel ) startModelRemoval(*m_modelConfirmIndex);
        if ( cancel || confirm ) {
            m_modelConfirmIndex.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /// @brief 为模型表的一次刷新提交独立磁盘扫描。
    /// @warning 从目录打开或绑定变化触发，渲染不遍历文件。
    /// @details 一次遍历全部已绑定行，将容量和归属标志一起发布给界面；
    /// 未绑定行或读取失败的行保持空值，使按钮使用保守的解绑语义。
    /// @pre 调用方已检查上一轮扫描和删除都不在运行。
    void scanModelStorage()
    {
        auto* files = Runtime::AppThreadPool::instance().getFileThreadPool();
        if ( !files ) return;
        m_refreshModelStorage = false;
        // 路径数组按值捕获，避免文件线程扫描途中读取 UI 修改后的路径。
        // 共享模型文件锁的使用由各项统计入口负责。
        m_modelStorageScan = files->enqueue([bindings = m_bindings] {
            ModelStorageRows rows;
            for ( std::size_t index = 0; index < bindings.size(); ++index ) {
                if ( !bindings[index].empty() )
                    rows[index] = inspectStemModelStorage(
                        STEM_MODEL_CATALOG[index], bindings[index]);
            }
            return rows;
        });
    }

    /// @brief 固定确认时的绑定位置，后台会再次核对其仍然有效。
    /// @warning 仅按钮确认触发，窗口关闭也不会取消已提交的卸载。
    /// @param index 待确认条目索引，只用于读取不可变的目录元数据。
    /// @details 工作线程既核对长期绑定是否仍匹配，又按冻结的范围处理文件。
    /// 模型删除或解绑不依赖当前主音轨，也不需要解码线程。
    void startModelRemoval(std::size_t index)
    {
        m_error.clear();
        if ( auto* files =
                 Runtime::AppThreadPool::instance().getFileThreadPool() )
            m_modelRemoval =
                files->enqueue([entry     = STEM_MODEL_CATALOG[index],
                                directory = m_modelConfirmPath,
                                managed   = m_modelConfirmRemoveFiles] {
                    return removeStemModelBinding(entry, directory, managed);
                });
        // 提交失败时仍由确认框退出，用户可以检查文件服务后重新选择，
        // 本帧没有任何模型记录或文件因队列不可用而被改动。
        if ( !m_modelRemoval.valid() ) m_error = "无法提交模型移除任务。";
    }

    /// @brief 模型以分隔行呈现。宽面板右侧放操作，窄面板放在说明下方。
    /// @warning UI 热路径；目录高度、留白和断点只使用字体、DPI 和可用空间。
    /// @param dpiScale 当前面板内容缩放，用于维持物理触达面积和表格留白。
    /// @details 行说明使用伸展列，按钮列保留按标签测得的宽度。
    /// 窄窗口把操作放回说明下方，使技术描述可换行且操作仍能完整显示。
    void drawCatalog(float dpiScale)
    {
        if ( m_refreshModelStorage && !m_modelStorageScan.valid() &&
             !m_modelRemoval.valid() )
            scanModelStorage();
        const auto& style    = ImGui::GetStyle();
        const float font     = ImGui::GetFontSize();
        const float paddingX = std::max(12.0F * dpiScale, font * 0.65F);
        const float paddingY = std::max(10.0F * dpiScale, font * 0.5F);
        const float gap      = std::max(8.0F * dpiScale, style.ItemSpacing.x);
        const float minButtonWidth =
            ImGui::CalcTextSize("绑定已有").x + paddingX * 2.0F;
        const float buttonHeight = std::max(32.0F * dpiScale, font + paddingY);
        const float actionWidth  = 2.0F * minButtonWidth + gap;
        // 右侧预留完整按钮宽度，左侧仍至少容纳足够的说明文字才拆成两列。
        const bool wide =
            ImGui::GetContentRegionAvail().x >=
            actionWidth + font * 21.0F + 4.0F * paddingX + style.ScrollbarSize;
        constexpr auto flags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_PadOuterX | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp;
        // 固定视口可让目录独立滚动，保留下方的分离操作入口。
        const float tableHeight = 17.0F * ImGui::GetTextLineHeightWithSpacing();
        ImGui::Spacing();
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, { paddingX, paddingY });
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            { gap, std::max(5.0F * dpiScale, font * 0.25F) });
        if ( ImGui::BeginTable("StemModelCatalog",
                               wide ? 2 : 1,
                               flags,
                               { 0.0F, tableHeight }) ) {
            // 只在宽布局注册右列，单列模式下不保留空操作列的内边距。
            ImGui::TableSetupColumn("模型", ImGuiTableColumnFlags_WidthStretch);
            if ( wide )
                ImGui::TableSetupColumn("操作",
                                        ImGuiTableColumnFlags_WidthFixed,
                                        actionWidth + 1.0F);
            for ( std::size_t index = 0; index < STEM_MODEL_CATALOG.size();
                  ++index ) {
                // ID 与索引对应的模型身份固定，列布局切换不会将一次按下
                // 匹配到其他模型行的同名按钮。
                ImGui::PushID(STEM_MODEL_CATALOG[index].id);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const float detailsTop = ImGui::GetCursorScreenPos().y;
                drawCatalogDetails(index);
                const float detailsHeight =
                    ImGui::GetItemRectMax().y - detailsTop;
                if ( wide )
                    ImGui::TableSetColumnIndex(1);
                else
                    ImGui::Spacing();
                drawCatalogActions(index,
                                   minButtonWidth,
                                   buttonHeight,
                                   wide ? detailsHeight : 0.0F,
                                   dpiScale);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar(2);
        ImGui::Spacing();
    }

    /// @brief 展示高级外部模型导入选项，兼容已有的用户自定义 MSST 工作流。
    /// @details 后端配置仅应用于自定义清单，与内置条目环境独立。
    /// @warning 渲染热路径，只有按钮事件和实际配置变更才触发文件操作。
    /// @pre 面板展开时已加载自定义运行环境。
    /// @details 清单、Python 和执行脚本是独立入口，编辑其中一个无需重选其他项。
    /// 自定义设备选项只保存到本机旧版模型环境，不改变已绑定的内置环境。
    void drawExternalModelControls()
    {
        // 自定义引擎保留对旧配置的编辑入口，目录模型的环境随各自绑定管理。
        auto& environment = *m_environment;
        // 自定义清单允许多模型协作。沿用公开包格式，不要求迁到目录部署。
        if ( FeedbackButton("选择外部模型") ) {
            const nfdu8filteritem_t filter{ "MMM model package", "json" };
            if ( const auto file = pickFile(&filter); !file.empty() ) {
                auto model = readStemSeparationModel(file);
                if ( model ) {
                    // 清掉目录模型的运行环境，手工清单使用此区域的配置。
                    m_model = std::move(*model);
                    m_selectedCatalog.reset();
                    m_boundEnvironment.reset();
                    m_error.clear();
                } else {
                    m_error = model.error();
                }
            }
        }
        // 按交互批量提交，展开配置区域本身不写盘。
        bool changed = false;
        if ( FeedbackButton("选择 Python") ) {
            if ( auto file = pickFile(); !file.empty() ) {
                environment.python = std::move(file);
                changed            = true;
            }
        }
        ImGui::TextWrapped("%s", environment.python.c_str());
        // 自定义运行入口只接受脚本。目录模型的脚本由安装器选择。
        if ( FeedbackButton("选择 MSST inference.py") ) {
            const nfdu8filteritem_t filter{ "Python", "py" };
            if ( auto file = pickFile(&filter); !file.empty() ) {
                environment.backend = std::move(file);
                changed             = true;
            }
        }
        if ( !environment.backend.empty() )
            ImGui::TextWrapped("%s", environment.backend.c_str());
        // 旧 MSST 配置允许显式指定设备。目录内置权重使用独立 CPU 环境。
        changed |= FeedbackCheckbox("使用 CPU", &environment.cpu);
        // 每帧合并环境改动后至多保存一次；失败时保留内存值以便继续编辑。
        if ( changed && !saveStemSeparationEnvironment(environment) ) {
            m_error = "无法保存分离运行环境。";
        }
    }

    /// @brief 按目录、音源和作业的顺序组织分离面板。
    /// @details 是否可分离由当前模型、音源及有效后端共同决定。
    /// @warning 只显示缓存状态；分离及模型部署均提交后台执行。
    /// @param dpiScale 用于模型表及其操作按钮的窗口内容倍率。
    /// @details 内置条目在自身的环境中推理。手工清单按高级区配置选择后端，
    /// 两者不读取对方的解释器设置。
    void drawSeparationControls(float dpiScale)
    {
        // 目录是默认入口。已知模型的声部在目录行中呈现，无需再重复一份列表。
        if ( ImGui::CollapsingHeader("模型目录",
                                     ImGuiTreeNodeFlags_DefaultOpen) ) {
            drawCatalog(dpiScale);
        }
        // 运行时解释器只属于高级配置，普通目录模型绑定后即可使用。
        if ( ImGui::CollapsingHeader("高级 · 自定义模型") )
            drawExternalModelControls();

        ImGui::Spacing();
        if ( m_model ) {
            ImGui::TextWrapped("当前模型  %s", m_model->name.c_str());
            // 自定义清单没有对应的目录行，才从解析结果展示声部名。
            if ( !m_selectedCatalog ) {
                for ( const auto& stem : m_model->stems )
                    ImGui::BulletText("%s", stem.c_str());
            }
        } else {
            ImGui::TextDisabled("请部署或绑定一个模型。");
        }
        // 音源已经由宿主窗口的资源身份确定，界面只负责选择模型。
        // 解析模型包时可能还没设置旧的 MSST 入口，避免提交无后端的请求。
        const auto& environment =
            m_boundEnvironment ? *m_boundEnvironment : *m_environment;
        ImGui::BeginDisabled(!m_model || environment.backend.empty());
        if ( FeedbackButton("分离主音轨") ) startSeparation();
        ImGui::EndDisabled();
    }

    /// @brief 固定本次模型和音源快照，启动后台推理和解码。
    /// @pre 控制器关联有效的项目主轨，且模型已解析。
    /// @details 每个主轨控制器一次只提交一项分离，结果就绪后才接入混音。
    /// @warning 点击触发。后台作业不借用窗口对象，关闭窗口仅丢弃待发布结果。
    /// @details 文件池负责推理生命周期及缓存事务，解码池服务于结果准备。
    /// 活动模型的共享文件锁覆盖推理到发布，避免过程中移除解释器或模型权重。
    /// 同一音源的缓存锁独立于模型锁，不同音源可读取同一已安装模型。
    void startSeparation()
    {
        auto& runtime = Runtime::AppThreadPool::instance();
        auto* files   = runtime.getFileThreadPool();
        auto* decode  = runtime.get();
        if ( !files || !decode ) {
            m_error = "音频后台服务尚未就绪。";
            return;
        }
        m_error.clear();
        m_discardLoad = false;
        // 新请求只能由分离页发起，上一组音轨在结果到达时才切换。
        // 在提交时才组合根与相对路径，取消或重命名不改后台已持有的路径。
        // 工作线程只持有值快照和应用级池指针，不观察 UI 的后续选择。
        // 每个任务获得新的停止状态，上一任务的取消不会污染下一次启动。
        m_separationStop = std::stop_source{};
        m_load = files->enqueue([model       = *m_model,
                                 source      = m_sourcePath,
                                 environment = m_boundEnvironment
                                                   ? *m_boundEnvironment
                                                   : *m_environment,
                                 stop        = m_separationStop.get_token(),
                                 playback    = m_stemSource,
                                 decode]() -> LoadResult {
            // 按模型文件、音源缓存的顺序尝试取得锁。竞争发生时立即反馈，
            // 不让等待同一资源的文件作业耗尽执行槽。
            const auto modelFiles = tryLockStemModelFiles();
            if ( !modelFiles )
                return std::unexpected("正在卸载模型，请稍后分离。");
            // 同源控制器不能在推理或发布期间删除缓存，也不能并行重复推理。
            const auto lock = playback->tryLockCache();
            if ( !lock ) return std::unexpected("此音轨已有分离或清理任务。");
            auto result = runStemSeparation(model,
                                            source,
                                            environment.python,
                                            environment.backend,
                                            environment.cpu,
                                            *decode,
                                            stop,
                                            playback->mix()->useStems);
            if ( !result ) return std::unexpected(std::move(result.error()));
            if ( stop.stop_requested() ) return std::unexpected("已取消分离。");
            // 推理耗时期间用户可能切换播放模式。结果接入时读取最新值，
            // 避免刚生成的声部强行切走此刻正在播放的原版主轨。
            result->useStems  = playback->mix()->useStems;
            const bool stored = !result->settingsPath.empty();
            // 发布也属于事务，防止删除后安装仍滞留在 future 内的旧结果。
            if ( !playback->publish(std::move(*result)) )
                return std::unexpected("无法准备分轨播放。");
            playback->queueSettingsSave();
            return stored;
        });
        if ( !m_load.valid() ) m_error = "无法提交分离任务。";
    }

    /// @brief 绘制控制谱面主轨的声部推子、启用与独听。
    /// @warning UI 热路径；空闲帧只借用状态，控件变化才复制最新组合。
    /// @param dpiScale 声部表的行距及组间分割线厚度所用的窗口内容倍率。
    /// @details 每个声部使用目录稳定索引访问增益、静音和独听状态。
    /// 所有控件从同一快照绘制；首次编辑时生成草稿，帧尾统一发布。
    void drawTrackControls(float dpiScale)
    {
        const auto  snapshot = m_stemSource->mix();
        const auto& current  = *snapshot;
        if ( !current.catalog ) return;
        // 跟踪目录对象身份而非名称：同名的重新分离结果也可能改变声部顺序，
        // 标签必须与当前通道索引一致。
        if ( m_labelCatalog != current.catalog ) {
            // 文件名只在目录变更时转为展示文本，连续拖动不解析路径。
            m_labelCatalog = current.catalog;
            m_labels.clear();
            // 常见模型的声部使用短中文名；自定义模型保留原文件名。
            constexpr std::array<std::pair<std::string_view, std::string_view>,
                                 6>
                names{ { { "vocals", "人声" },
                         { "drums", "鼓组" },
                         { "bass", "贝斯" },
                         { "guitar", "吉他" },
                         { "piano", "钢琴" },
                         { "other", "其他" } } };
            for ( const auto& track : current.catalog->tracks() ) {
                auto name =
                    Config::pathToUtf8(Config::utf8ToPath(track.id).stem());
                const auto it =
                    std::ranges::find_if(names, [&](const auto& entry) {
                        return entry.first == name;
                    });
                if ( it != names.end() ) name = it->second;
                m_labels.push_back(std::move(name));
            }
        }
        std::optional<Audio::StemPlaybackMix> edited;
        /// @brief 在本帧首次变动时创建草稿，后续控件更新同一份组合。
        const auto edit = [&]() -> Audio::StemPlaybackMix& {
            if ( !edited ) edited = current;
            return *edited;
        };
        const auto& style = ImGui::GetStyle();
        const float width = ImGui::GetContentRegionAvail().x;
        const float em    = ImGui::GetFontSize();
        // 宽窗口两组并排；窄 Dock 直接改为单列声部，不生成内部横向滚动。
        const int   groups = width >= 44.0F * em && m_labels.size() > 1 ? 2 : 1;
        const float groupWidth  = width / static_cast<float>(groups);
        const bool  compact     = groupWidth < 18.0F * em;
        const char* muteLabel   = compact ? "M" : "静音";
        const char* soloLabel   = compact ? "S" : "独听";
        const float buttonWidth = std::max(ImGui::CalcTextSize(muteLabel).x,
                                           ImGui::CalcTextSize(soloLabel).x) +
                                  2.0F * style.FramePadding.x;
        // 保留足够宽的音量滑块；长模型文件名限制在自己的列内并提供完整提示。
        // 列宽按每组计算，保持所有声部的推子宽度一致；
        // 全名提示只在固定标签列不足以容纳实际文字时显示。
        const float nameWidth =
            std::clamp(groupWidth * 0.23F, 2.0F * em, 7.0F * em);
        ImGui::PushStyleVar(
            ImGuiStyleVar_CellPadding,
            ImVec2(style.CellPadding.x,
                   std::max(3.0F * dpiScale, style.CellPadding.y)));
        const float tableTop = ImGui::GetCursorScreenPos().y;
        if ( ImGui::BeginTable("StemMixer",
                               groups * 4,
                               ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_BordersInnerH |
                                   ImGuiTableFlags_SizingFixedFit) ) {
            for ( int group = 0; group < groups; ++group ) {
                ImGui::TableSetupColumn(
                    "##name", ImGuiTableColumnFlags_WidthFixed, nameWidth);
                ImGui::TableSetupColumn(
                    "##gain", ImGuiTableColumnFlags_WidthStretch, 1.0F);
                ImGui::TableSetupColumn(
                    "##mute", ImGuiTableColumnFlags_WidthFixed, buttonWidth);
                ImGui::TableSetupColumn(
                    "##solo", ImGuiTableColumnFlags_WidthFixed, buttonWidth);
            }
            std::optional<float> dividerX;
            for ( std::size_t index = 0; index < m_labels.size(); ++index ) {
                // 同一声部始终使用目录索引作为 ID，改变列数不会重置音量。
                ImGui::PushID(static_cast<int>(index));
                // 独听只覆盖听感状态，静音仍独立显示自身的开关值。
                // 关闭独听后可恢复之前各轨的开关，不需要回写一组被覆盖的静音。
                const bool audible = current.solo ? *current.solo == index
                                                  : current.enabled[index];
                const bool muted   = !current.enabled[index];
                const bool solo    = current.solo == index;
                ImGui::TableNextColumn();
                // 以右组第一列的实际边缘定位，兼容列取整和主题单元格内边距。
                if ( groups == 2 && index == 1 )
                    dividerX =
                        ImGui::GetCursorScreenPos().x - style.CellPadding.x;
                ImGui::AlignTextToFramePadding();
                if ( !audible )
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          style.Colors[ImGuiCol_TextDisabled]);
                const auto* name = m_labels[index].c_str();
                ImGui::TextUnformatted(name);
                if ( !audible ) ImGui::PopStyleColor();
                if ( ImGui::CalcTextSize(name).x > nameWidth )
                    ImGui::SetItemTooltip("%s", name);

                ImGui::TableNextColumn();
                // 贴合伸展列的剩余宽度，让缩窄时音量控件随本组调整。
                ImGui::SetNextItemWidth(-FLT_MIN);
                // UI 用百分数表示电平，发布时仍使用混音器的线性倍率。
                float percent = current.gains[index] * 100.0F;
                if ( ImGui::SliderFloat("##gain",
                                        &percent,
                                        0.0F,
                                        100.0F,
                                        "%.0f%%",
                                        ImGuiSliderFlags_AlwaysClamp) )
                    edit().gains[index] = percent * 0.01F;

                ImGui::TableNextColumn();
                if ( FeedbackSelectableButton(
                         muteLabel, muted, ImVec2(buttonWidth, 0)) )
                    edit().enabled[index] = muted;
                ImGui::SetItemTooltip("%s", muted ? "取消静音" : "静音");

                ImGui::TableNextColumn();
                // 独听状态用选中色提示，退出后恢复通道原有的静音组合。
                if ( FeedbackSelectableButton(
                         soloLabel, solo, ImVec2(buttonWidth, 0)) ) {
                    if ( solo )
                        edit().solo.reset();
                    else
                        edit().solo = index;
                }
                ImGui::SetItemTooltip("%s", solo ? "退出独听" : "独听");
                ImGui::PopID();
            }
            ImGui::EndTable();
            if ( dividerX ) {
                // 在表格合并绘制通道后画线，避免被斑马纹盖住或被单元格裁剪。
                const float tableBottom =
                    ImGui::GetCursorScreenPos().y - style.ItemSpacing.y;
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(*dividerX, tableTop),
                    ImVec2(*dividerX, tableBottom),
                    ImGui::GetColorU32(ImGuiCol_Text, 0.35F),
                    std::max(1.0F, dpiScale));
            }
        }
        ImGui::PopStyleVar();
        // 同一画面上的多项输入只产生一次更新，下一音频块可看到完整组态。
        // 发布成功才请求写盘，旧目录的草稿若被拒绝就不覆盖新缓存的设置。
        if ( edited && m_stemSource->publishSettings(std::move(*edited)) )
            m_stemSource->queueSettingsSave();
    }

    /// @brief 上一帧区域已展开，用于按交互刷新模型绑定。
    bool m_expanded = false;
    /// @brief 已提交的文件任务；仅在 ready 后消费其值。
    /// @details 销毁 future 不取消解码，任务也不捕获本对象。
    /// @details 结果只报告发布是否成功及是否落盘，播放数据已直接交给共享音源。
    /// 主轨切换后仍取走就绪值以回收槽位，但可忽略旧音源的界面反馈。
    std::future<LoadResult> m_load;
    /// @brief 当前源最近一次成功取得的磁盘状态。
    std::optional<std::vector<Audio::StemPlaybackCacheInfo>> m_cacheInfo;
    /// @brief 目录统计结果；未完成时保留上一份展示。
    std::future<std::optional<std::vector<Audio::StemPlaybackCacheInfo>>>
        m_cacheQuery;
    /// @brief 模型缓存操作回执；空错误码表示操作已提交。
    std::future<std::error_code> m_cacheAction;
    /// @brief 区分正在恢复缓存和正在清理缓存两种状态。
    /// @details
    /// 两种缓存操作共用错误码类型，保留意图用来选择对应的进度与错误提示。
    bool m_activatingCache = false;
    /// @brief 上一帧设置页是否可见，重新进入时需要再次统计。
    bool m_cacheVisible = false;
    /// @brief 作业改变磁盘内容后安排一次重新统计。
    bool m_refreshCache = true;
    /// @brief 查询或删除失败信息，与模型部署错误分别显示。
    /// @details
    /// 查询可能在动作错误后自动重跑，刷新列表不应擦掉仍需展示的动作错误。
    /// 与模型部署错误独立维护，避免两个文件任务争用同一条提示。
    std::string m_cacheError;
    /// @brief 当前主轨变化后丢弃尚未返回的旧轨结果。
    /// @details 旧任务仍需消费后才允许用新主轨启动分离。
    bool m_discardLoad = false;
    /// @brief 当前文件在应用会话中共用的分轨状态，关闭本面板不丢弃它。
    /// @details 当前播放源可被其他控制器和谱面共享；已提交作业另持自己的引用。
    /// @warning 绘制时借用成员，只有切换音源或提交低频任务才复制所有权。
    std::shared_ptr<Audio::StemPlaybackSource> m_stemSource;
    /// @brief 当前源的绝对路径，仅在绑定变化时计算。
    /// @details 由项目根和主轨相对路径合成的 UTF-8 身份，作为缓存查询输入。
    /// ImGui 页签也用这个值隔离同一个控制器内切换过的不同主轨。
    std::string m_sourcePath;
    /// @brief 标签缓存所属的目录，仅在目录变化时更新。
    /// @details 标签缓存以目录身份失效。目录换代时保有引用只用于判断身份，
    /// 通道的实际值总是来自本帧混音快照。
    std::shared_ptr<const Audio::StemAudition> m_labelCatalog;
    /// @brief 目录顺序对应的可见声部名称。
    /// @invariant 标签数量及顺序与最近一次生成它们的目录一致。
    /// 未生成标签时使用空容器，不从旧目录沿用可能已经变动的通道顺序。
    std::vector<std::string> m_labels;
    /// @brief 刚绑定时将标签选中状态与共享模式同步。
    bool m_restorePlaybackTab = true;
    /// @brief 上次实际绘制的播放页，避免两个同源控制器每帧争夺播放模式。
    /// @details 空值表示停在设置页或正切换主轨，可避免外部切换强制离开设置页。
    /// 有值时与该帧已发布的音源模式比较，用于恢复正确的播放页。
    std::optional<bool> m_lastPlaybackTab;
    /// @brief 最近一次操作失败的提示，可包含后台返回的本地错误。
    /// @details 拥有完整文本，避免后台 future 销毁后仍持有借用字符串。
    std::string m_error;
    /// @brief 经过校验的外部模型及声部定义。
    std::optional<StemSeparationModel> m_model;
    /// @brief 构造本次主轨路径的项目根，与资源路径共同判断来源是否变化。
    std::filesystem::path m_root;
    /// @brief 控制器主轨在当前项目中的相对路径。
    std::string m_relativePath;
    /// @brief 从本机设置恢复解释器和推理入口，首次打开时读取一次。
    std::optional<StemSeparationEnvironment> m_environment;
    /// @brief 已选用目录模型的独立解释器，空值表示使用自定义模型设置。
    /// @details 环境与模型在 UI 线程成对替换，提交推理时一起快照。
    std::optional<StemSeparationEnvironment> m_boundEnvironment;
    /// @brief 当前选中的目录行，可空以兼容用户导入模型。
    std::optional<std::size_t> m_selectedCatalog;
    /// @brief 按模型目录顺序记录长期绑定位置。
    /// @details 本机配置以稳定 ID 序列化，UI 顺序不会成为磁盘格式。
    std::array<std::string, STEM_MODEL_CATALOG.size()> m_bindings;
    /// @brief 最近观察到的共享绑定提交序号，避免重复加载配置。
    std::uint64_t m_bindingsRevision{};
    /// @brief 每条目录行的文件统计，未扫描或目录不可用时为空。
    using ModelStorageRows =
        std::array<std::optional<StemModelStorage>, STEM_MODEL_CATALOG.size()>;
    /// @brief 本面板最近一轮有效扫描的展示快照。
    /// @details
    /// 占用统计不参与播放。归属标志决定按钮能否请求删除文件，
    /// 实际是否允许删除仍由文件任务重新验证。
    ModelStorageRows m_modelStorage;
    /// @brief 模型目录访问可耗时，绘制线程仅在就绪时取回。
    /// @details 每个面板只接收一批磁盘统计；绑定变化时在取回结果后重新提交。
    /// 目录不可用的状态留在对应行的可选值内，不影响其他行显示。
    std::future<ModelStorageRows> m_modelStorageScan;
    /// @brief 绑定改变或重开目录时触发一次扫描。
    bool m_refreshModelStorage = true;
    /// @brief 删除结果与当前主轨无关，切换主轨也继续消费。
    /// @details 成功的配置更新可被其他控制器通过绑定序号观察。
    /// 文件占用在完成后重新统计，避免显示卸载之前的容量。
    std::future<std::expected<void, std::string>> m_modelRemoval;
    /// @brief 当前确认框对应的模型行。
    std::optional<std::size_t> m_modelConfirmIndex;
    /// @brief 将表内首次点击交给表外的模态入口，只消费一次。
    /// @details 该信号与待确认模型索引分开。前者只表示打开边沿，
    /// 后者一直保留到用户选择或模态关闭。
    bool m_modelConfirmOpenRequested = false;
    /// @brief 弹窗打开时冻结的目录，防止确认无意指向另一部署。
    /// @details 保留点击时的原始绑定，文件线程会对照当前配置重新确认，
    /// 防止历史弹窗在另一窗口完成改绑后影响新目录。
    std::string m_modelConfirmPath;
    /// @brief 弹窗所承诺的删除范围，不随异步统计变化。
    bool m_modelConfirmRemoveFiles = false;
    /// @brief 正在部署的目录行，完成后将结果存至该行。
    /// @details 与当前选择独立，只有部署 future 有效时参与绘制与发布。
    std::size_t m_deployingIndex = 0;
    /// @brief 文件任务的部署结果，渲染路径只轮询是否完成。
    /// @details
    /// 内容包含已验证的模型及与之配对的解释器，发布时不再次读取部署目录。
    /// 失败值只更新错误说明，原活动配置保留用于重试或切换模型。
    std::future<std::expected<BoundStemModel, std::string>> m_deployment;
    /// @brief 部署停止信号与分离任务独立，避免跨作业取消。
    /// @details 每次提交前替换，已排队任务只持有对应代次的 token。
    /// @details
    /// 命令提交时捕获本次令牌。请求停止后，安装状态仍需等任务自然收尾，
    /// 释放目录使用权再由轮询解除界面上的忙状态。
    std::stop_source m_deploymentStop;
    /// @brief 本次推理取消信号，工作线程持有自身 token。
    /// @details 这个令牌同时承接用户取消和主轨更换两种停止请求。
    /// 就绪结果是否仍有资格更新当前界面，由独立的源切换状态判断。
    std::stop_source m_separationStop;
};
}  // namespace

/// @brief 创建控制器独占的桌面声部分离区域。
std::unique_ptr<StemSeparationPanel> createStemSeparationPanel()
{
    return std::make_unique<MainTrackStemPanel>();
}
}  // namespace MMM::UI

#endif
