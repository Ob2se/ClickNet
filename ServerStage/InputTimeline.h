#pragma once
#include "ClickNetWire.h"
#include <deque>
#include <algorithm>

// Sparse commands retain the client's fixed-step timeline. Keep the command
// preceding the history window so held input can be reconstructed on replay.
class InputTimeline
{
public:
    void Add(clicknet::wire::Input input)
    {
        auto at = std::lower_bound(commands.begin(), commands.end(), input.tick,
            [](const auto& command, std::uint32_t tick) { return command.tick < tick; });
        if (at != commands.end() && at->tick == input.tick)
        {
            input.jump = input.jump || at->jump;
            *at = input;
        }
        else commands.insert(at, input);
        if (commands.size() > 256) commands.pop_front();
    }
    clicknet::wire::Input At(std::uint32_t tick) const
    {
        clicknet::wire::Input result{};
        for (const auto& command : commands)
        {
            if (command.tick > tick) break;
            result = command;
        }
        result.jump = result.jump && result.tick == tick;
        return result;
    }
    void DiscardBefore(std::uint32_t tick)
    {
        while (commands.size() > 1 && commands[1].tick <= tick) commands.pop_front();
    }
private:
    std::deque<clicknet::wire::Input> commands;
};
