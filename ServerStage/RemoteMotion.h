#pragma once
#include "ClickNetWire.h"

// Compare actual motion with the trajectory observers were last notified of.
// Gravity alone is predictable; contacts and accumulated drift are not.
inline bool RemoteMotionNeedsUpdate(const clicknet::wire::PlayerState& baseline,
    const clicknet::wire::PlayerState& current)
{
    if (current.serverTick <= baseline.serverTick) return false;
    if (current.grounded != baseline.grounded) return true;
    const auto age = current.serverTick - baseline.serverTick;
    if (age < 3) return false;
    const auto predicted = clicknet::wire::PredictPlayerState(baseline, age / 60.0f);
    const float px = current.px - predicted.px, py = current.py - predicted.py,
        pz = current.pz - predicted.pz;
    const float vx = current.vx - predicted.vx, vy = current.vy - predicted.vy,
        vz = current.vz - predicted.vz;
    return px * px + py * py + pz * pz > 0.12f * 0.12f
        || vx * vx + vy * vy + vz * vz > 0.4f * 0.4f;
}
