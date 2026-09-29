#include "InputTimeline.h"
#include <cassert>

int main()
{
    InputTimeline reference, delayed;
    clicknet::wire::Input right{};
    right.tick = 1; right.moveX = 1;
    clicknet::wire::Input left = right;
    left.tick = 5; left.moveX = -1;
    clicknet::wire::Input jump = right;
    jump.tick = 8; jump.moveX = 0; jump.jump = true;
    reference.Add(right); reference.Add(left); reference.Add(jump);
    delayed.Add(right);
    // Future changes cannot affect earlier steps, and jump is a one-step event.
    assert(reference.At(4).moveX == 1);
    assert(reference.At(5).moveX == -1);
    assert(reference.At(8).jump);
    assert(!reference.At(9).jump);
    int cleanPosition = 0, delayedPosition = 0, beforeLateTurn = 0;
    for (unsigned tick = 1; tick <= 10; ++tick)
    {
        cleanPosition += static_cast<int>(reference.At(tick).moveX);
        delayedPosition += static_cast<int>(delayed.At(tick).moveX);
        if (tick == 4) beforeLateTurn = delayedPosition;
    }
    // Two commands arriving together must both survive replay on their ticks.
    delayed.Add(left); delayed.Add(jump);
    delayedPosition = beforeLateTurn;
    int jumps = 0;
    for (unsigned tick = 5; tick <= 10; ++tick)
    {
        delayedPosition += static_cast<int>(delayed.At(tick).moveX);
        jumps += delayed.At(tick).jump ? 1 : 0;
    }
    assert(cleanPosition == delayedPosition);
    assert(jumps == 1);
    delayed.DiscardBefore(7);
    assert(delayed.At(7).moveX == -1);
    assert(delayed.At(8).jump);
}
