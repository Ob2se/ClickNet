#include "../Plugins/Box3D/Source/ThirdParty/ClickNetShared/ClickNetWire.h"
#include <cassert>

int main()
{
    const clicknet::wire::PlayerAppearance appearance{12345, 678, 1};
    auto appearanceBytes = clicknet::wire::Encode(appearance);
    clicknet::wire::PlayerAppearance decodedAppearance;
    assert(clicknet::wire::Decode(appearanceBytes.data(), appearanceBytes.size(), decodedAppearance));
    assert(decodedAppearance.serverTick == appearance.serverTick);
    assert(decodedAppearance.playerId == appearance.playerId);
    assert(decodedAppearance.value == appearance.value);
    assert(!clicknet::wire::Decode(appearanceBytes.data(), appearanceBytes.size() - 1, decodedAppearance));
    appearanceBytes[2] = 6;
    assert(!clicknet::wire::Decode(appearanceBytes.data(), appearanceBytes.size(), decodedAppearance));
    clicknet::wire::PlayerState airborne{};
    airborne.pz = 10;
    airborne.vz = 5;
    float height = airborne.pz, velocity = airborne.vz;
    for (int step = 1; step <= 60; ++step)
    {
        velocity -= 9.81f / 60.0f;
        height += velocity / 60.0f;
        const auto predicted = clicknet::wire::PredictPlayerState(airborne, step / 60.0f);
        assert(std::abs(predicted.pz - height) < 0.0001f);
        assert(std::abs(predicted.vz - velocity) < 0.0001f);
    }
    airborne.grounded = true;
    airborne.vz = 0;
    assert(clicknet::wire::PredictPlayerState(airborne, 2.0f).pz == airborne.pz);
    clicknet::wire::PlayerStateBatch batch;
    for (std::uint32_t id = 1; id <= clicknet::wire::MaxPlayerStatesPerBatch; ++id)
    {
        clicknet::wire::PlayerState state{};
        state.playerId = id;
        state.serverTick = 123;
        state.receivedInputTick = 100 + id;
        state.inputAckSequence = 500 + id;
        state.inputAckBits = 0x8000000000000005ull;
        state.px = static_cast<float>(id);
        state.grounded = id % 2 != 0;
        batch.states.push_back(state);
    }
    const auto bytes = clicknet::wire::Encode(batch);
    assert(bytes.size() == 5 + batch.states.size() * 45);
    assert(bytes.size() <= 1200);
    clicknet::wire::PlayerStateBatch decoded;
    assert(clicknet::wire::Decode(bytes.data(), bytes.size(), decoded));
    assert(decoded.states.size() == batch.states.size());
    for (std::size_t i = 0; i < decoded.states.size(); ++i)
    {
        assert(decoded.states[i].playerId == batch.states[i].playerId);
        assert(decoded.states[i].px == batch.states[i].px);
        assert(decoded.states[i].receivedInputTick == batch.states[i].receivedInputTick);
        assert(decoded.states[i].inputAckSequence == 0);
        assert(decoded.states[i].inputAckBits == 0);
        assert(decoded.states[i].grounded == batch.states[i].grounded);
    }
    assert(!clicknet::wire::Decode(bytes.data(), bytes.size() - 1, decoded));
    const auto ownerBytes = clicknet::wire::Encode(batch.states.front());
    clicknet::wire::PlayerState owner;
    assert(clicknet::wire::Decode(ownerBytes.data(), ownerBytes.size(), owner));
    assert(owner.inputAckSequence == batch.states.front().inputAckSequence);
    assert(owner.inputAckBits == batch.states.front().inputAckBits);
    auto invalid = bytes;
    invalid[2] = 5;
    assert(!clicknet::wire::Decode(invalid.data(), invalid.size(), decoded));
    invalid = bytes;
    invalid[4] = 0;
    assert(!clicknet::wire::Decode(invalid.data(), invalid.size(), decoded));
    invalid[4] = static_cast<std::uint8_t>(clicknet::wire::MaxPlayerStatesPerBatch + 1);
    assert(!clicknet::wire::Decode(invalid.data(), invalid.size(), decoded));
    batch.states.push_back({});
    assert(clicknet::wire::Encode(batch).empty());
}
