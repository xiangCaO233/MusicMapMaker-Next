#pragma once

namespace MMM::Logic
{
struct RenderSnapshot;
}  // namespace MMM::Logic

namespace MMM::Canvas
{

/// @brief Calculates the target display time for marquee edge auto-scroll.
/// @param snapshot Current UI render snapshot.
/// @param viewportHeight Current canvas viewport height in pixels.
/// @param mouseY Local mouse Y coordinate in pixels.
/// @param deltaTime Current UI frame delta time in seconds.
/// @param isAccelerated Whether Shift acceleration should be applied.
/// @param scrolled Output flag indicating whether auto-scroll is needed.
/// @return Target display time in seconds after auto-scroll.
/// @warning UI hot path: called every frame during marquee dragging; only
/// reads the immutable snapshot and visual config.
double marqueeAutoScrollTargetTime(const Logic::RenderSnapshot& snapshot,
                                   float viewportHeight, float mouseY,
                                   float deltaTime, bool isAccelerated,
                                   bool& scrolled);

}  // namespace MMM::Canvas
