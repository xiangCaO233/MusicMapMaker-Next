#include "canvas/BackgroundVideoPlayer.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"

#include <cmath>
#include <ice/thread/ThreadPool.hpp>
#include <utility>

namespace MMM::Canvas
{

BackgroundVideoPlayer::BackgroundVideoPlayer()
{
    auto* appThreadPool = Runtime::AppThreadPool::instance().get();
    if ( !appThreadPool ) {
        XERROR("Background video: Runtime thread pool is not initialized.");
        return;
    }
    const auto token = m_stopSource.get_token();
    m_workerFuture =
        appThreadPool->enqueue([this, token]() { workerLoop(token); });
}

BackgroundVideoPlayer::~BackgroundVideoPlayer()
{
    m_stopSource.request_stop();
    m_condition.notify_all();
    if ( m_workerFuture.valid() ) {
        m_workerFuture.wait();
        m_workerFuture = std::future<void>{};
    }
}

std::uint64_t BackgroundVideoPlayer::setSource(
    const std::filesystem::path& path)
{
    std::lock_guard lock(m_mutex);
    m_requestedSource = path;
    m_requestedTime   = 0.0;
    m_readyFrame.reset();
    ++m_sourceRevision;
    ++m_requestGeneration;
    ++m_requestRevision;
    m_condition.notify_one();
    return m_requestGeneration;
}

std::uint64_t BackgroundVideoPlayer::requestFrame(double targetTime,
                                                  bool   startsNewGeneration)
{
    std::lock_guard lock(m_mutex);
    if ( !std::isfinite(targetTime) ) {
        return m_requestGeneration;
    }
    m_requestedTime = targetTime;
    if ( startsNewGeneration ) {
        ++m_requestGeneration;
    }
    ++m_requestRevision;
    m_condition.notify_one();
    return m_requestGeneration;
}

bool BackgroundVideoPlayer::tryTakeLatestFrame(BackgroundVideoFrame& frame)
{
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if ( !lock.owns_lock() || !m_readyFrame ) {
        return false;
    }
    frame = std::move(*m_readyFrame);
    m_readyFrame.reset();
    return true;
}

void BackgroundVideoPlayer::workerLoop(std::stop_token stopToken)
{
    Utils::VideoFrameDecoder decoder;
    std::filesystem::path    activeSource;
    std::uint64_t            activeSourceRevision    = 0;
    std::uint64_t            handledRevision         = 0;
    std::uint64_t            lastPublishedGeneration = 0;
    double                   lastPublishedTimestamp  = 0.0;
    bool                     hasPublishedTimestamp   = false;
    bool                     lastPublishedReachedEnd = false;

    while ( !stopToken.stop_requested() ) {
        std::filesystem::path requestedSource;
        double                requestedTime     = 0.0;
        std::uint64_t         requestRevision   = 0;
        std::uint64_t         sourceRevision    = 0;
        std::uint64_t         requestGeneration = 0;
        {
            std::unique_lock lock(m_mutex);
            m_condition.wait(lock, [this, &stopToken, &handledRevision]() {
                return stopToken.stop_requested() ||
                       m_requestRevision != handledRevision;
            });
            if ( stopToken.stop_requested() ) {
                break;
            }
            requestedSource   = m_requestedSource;
            requestedTime     = m_requestedTime;
            requestRevision   = m_requestRevision;
            sourceRevision    = m_sourceRevision;
            requestGeneration = m_requestGeneration;
        }

        if ( sourceRevision != activeSourceRevision ) {
            decoder.close();
            activeSource            = requestedSource;
            activeSourceRevision    = sourceRevision;
            hasPublishedTimestamp   = false;
            lastPublishedGeneration = 0;
            lastPublishedReachedEnd = false;
            if ( !activeSource.empty() && !decoder.open(activeSource) ) {
                handledRevision = requestRevision;
                continue;
            }
        }

        if ( activeSource.empty() || !decoder.isOpen() ) {
            handledRevision = requestRevision;
            continue;
        }

        const Utils::VideoFrame* decodedFrame =
            decoder.decodeFrameAt(requestedTime);
        const bool reachedEnd = decoder.info().duration > 0.0 &&
                                requestedTime >= decoder.info().duration;
        const bool frameChanged =
            decodedFrame && (!hasPublishedTimestamp ||
                             std::abs(decodedFrame->timestamp -
                                      lastPublishedTimestamp) > 1e-6 ||
                             requestGeneration != lastPublishedGeneration ||
                             reachedEnd != lastPublishedReachedEnd);
        bool requestMayPublish = false;
        {
            std::lock_guard lock(m_mutex);
            requestMayPublish = sourceRevision == m_sourceRevision &&
                                requestGeneration == m_requestGeneration;
        }
        if ( requestMayPublish && frameChanged ) {
            BackgroundVideoFrame completedFrame{
                .frame             = *decodedFrame,
                .requestGeneration = requestGeneration,
                .reachedEnd        = reachedEnd,
            };
            std::lock_guard lock(m_mutex);
            if ( sourceRevision == m_sourceRevision &&
                 requestGeneration == m_requestGeneration ) {
                m_readyFrame            = std::move(completedFrame);
                lastPublishedTimestamp  = decodedFrame->timestamp;
                hasPublishedTimestamp   = true;
                lastPublishedGeneration = requestGeneration;
                lastPublishedReachedEnd = reachedEnd;
            }
        }
        handledRevision = requestRevision;
    }

    decoder.close();
}

}  // namespace MMM::Canvas
