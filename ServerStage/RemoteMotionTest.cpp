#include "RemoteMotion.h"
#include <cassert>
#include <iostream>

int main()
{
    clicknet::wire::PlayerState base{};
    base.serverTick = 100;
    base.grounded = true;
    base.vx = 5;
    auto current = clicknet::wire::PredictPlayerState(base, 1.0f);
    current.serverTick = 160;
    assert(!RemoteMotionNeedsUpdate(base, current)); // Straight travel stays sparse.
    current.vx = 0;
    assert(RemoteMotionNeedsUpdate(base, current)); // Hit a wall.
    base.vx = 0;
    current = base;
    current.serverTick += 3;
    current.vx = 5;
    assert(RemoteMotionNeedsUpdate(base, current)); // Clear a wall with held input.
    current.serverTick = base.serverTick + 2;
    assert(!RemoteMotionNeedsUpdate(base, current)); // Bound error-triggered frequency.
    current.grounded = false;
    assert(RemoteMotionNeedsUpdate(base, current)); // Ground transition bypasses delay.
    base.grounded = false;
    base.vz = 5;
    current = clicknet::wire::PredictPlayerState(base, 0.5f);
    current.serverTick += 30;
    assert(!RemoteMotionNeedsUpdate(base, current)); // Gravity needs no extra event.
    current.vx = 5;
    assert(RemoteMotionNeedsUpdate(base, current)); // Airborne wall release.
    current = clicknet::wire::PredictPlayerState(base, 0.5f);
    current.serverTick += 30;
    current.px += 0.13f;
    assert(RemoteMotionNeedsUpdate(base, current)); // Accumulated positional drift.
    std::cout << "Remote motion tests passed\n";
}
