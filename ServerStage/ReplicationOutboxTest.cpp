#include "ReplicationOutbox.h"
#include "../Plugins/Box3D/Source/ThirdParty/ClickNetShared/ClickNetWire.h"
#include <cassert>
#include <unordered_map>

int main()
{
    ReplicationOutbox outbox;
    std::unordered_map<uint32_t, uint32_t> versions;
    bool removed = false;
    const auto queue = [&]
    {
        outbox.Add(clicknet::wire::Encode(clicknet::wire::PlayerAppearance{50, 10, 1}), 8,
            [&] { versions[10] = 1; });
        clicknet::wire::PlayerStateBatch batch;
        for (uint32_t id = 10; id < 34; ++id)
        {
            clicknet::wire::PlayerState state;
            state.playerId = id;
            state.serverTick = 50;
            state.grounded = true;
            batch.states.push_back(state);
        }
        auto bytes = clicknet::wire::Encode(batch);
        outbox.Add(std::move(bytes), 0, [&, states = std::move(batch.states)]
        {
            for (const auto& state : states) versions[state.playerId] = state.serverTick;
        });
        outbox.Add(clicknet::wire::Encode(clicknet::wire::DespawnPlayer{99, 50}), 8,
            [&] { removed = true; });
    };
    queue();
    // Queuing must not acknowledge anything before the transport accepts it.
    assert(versions.empty() && !removed);
    clicknet::wire::PlayerStateBatch decoded;
    assert(clicknet::wire::Decode(outbox.packets[1].bytes.data(), outbox.packets[1].bytes.size(), decoded));
    assert(decoded.states.size() == 24 && decoded.states[23].playerId == 33);
    // GNS can accept a prefix, fail a message, then skip the remaining messages.
    assert(!outbox.Commit({1, -25, 0}));
    assert(versions.size() == 1 && versions.at(10) == 1 && !removed);
    assert(outbox.packets.empty());
    queue();
    assert(outbox.Commit({2, 3, 4}));
    assert(versions.size() == 24 && versions.at(10) == 50 && versions.at(33) == 50 && removed);
    versions.clear();
    removed = false;
    queue();
    assert(!outbox.Commit({0, 0, 0}));
    assert(versions.empty() && !removed);
    assert(outbox.Commit({}));
}
