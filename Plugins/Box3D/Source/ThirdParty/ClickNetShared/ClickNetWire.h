#pragma once

// Small, engine-independent, little-endian protocol shared by the Unreal
// client and the standalone server. Positions and velocities are in meters.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace clicknet::wire
{
constexpr std::uint8_t Version = 7;
enum class Type : std::uint8_t { Input = 1, Welcome = 2, PlayerState = 3, BodyState = 4, PlayerName = 5, DespawnPlayer = 6, PlayerStateBatch = 7, PlayerAppearance = 8 };
constexpr std::size_t MaxPlayerStatesPerBatch = 24;
constexpr std::size_t MaxInputsPerPacket = 16;

struct Input
{
    std::uint32_t sequence = 0;
    std::uint32_t tick = 0;
    float moveX = 0, moveY = 0; // World-space planar input, each in [-1, 1].
    float yaw = 0;              // Unreal/Box3D Z-up radians.
    bool jump = false;
};
struct InputPacket { std::vector<Input> commands; };

struct Welcome
{
    std::uint32_t playerId = 0;
    std::uint64_t levelHash = 0; // FNV-1a of the loaded .box3d bytes.
};

struct PlayerState
{
    std::uint32_t serverTick = 0;
    std::uint32_t acknowledgedInputTick = 0;
    std::uint32_t receivedInputTick = 0;
    std::uint32_t inputAckSequence = 0;
    std::uint64_t inputAckBits = 0;
    std::uint32_t playerId = 0;
    float px = 0, py = 0, pz = 0;
    float vx = 0, vy = 0, vz = 0;
    float yaw = 0;
    bool grounded = false;
};
struct PlayerStateBatch { std::vector<PlayerState> states; };

inline PlayerState PredictPlayerState(const PlayerState& state, float seconds)
{
    const float time = seconds > 0.0f ? seconds : 0.0f;
    PlayerState predicted = state;
    predicted.px += state.vx * time;
    predicted.py += state.vy * time;
    predicted.pz += state.vz * time;
    if (!state.grounded)
    {
        constexpr float gravity = -9.81f;
        // Match the mover's 60 Hz semi-implicit integration at fixed-step
        // boundaries, with a continuous trajectory between rendered frames.
        predicted.pz += 0.5f * gravity * time * (time + 1.0f / 60.0f);
        predicted.vz += gravity * time;
    }
    return predicted;
}

struct BodyState
{
    std::uint32_t serverTick = 0;
    std::array<std::uint32_t, 4> exportId{};
    float px = 0, py = 0, pz = 0;
    float qx = 0, qy = 0, qz = 0, qw = 1;
    float vx = 0, vy = 0, vz = 0;
    float wx = 0, wy = 0, wz = 0;
};

struct PlayerName
{
    std::string value; // UTF-8, at most 48 bytes.
};

struct PlayerAppearance
{
    std::uint32_t serverTick = 0;
    std::uint32_t playerId = 0;
    std::uint8_t value = 0;
};


struct DespawnPlayer { std::uint32_t playerId = 0; std::uint32_t serverTick = 0; };

inline bool ValidPlayerName(const std::string& value)
{
    if (value.size() > 48) return false;
    for (std::size_t i = 0; i < value.size();)
    {
        const auto first = static_cast<std::uint8_t>(value[i]);
        if (first < 0x20 || first == 0x7f) return false;
        if (first < 0x80) { ++i; continue; }
        int count = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { count = 2; codepoint = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 3; codepoint = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 4; codepoint = first & 0x07; minimum = 0x10000; }
        else return false;
        if (i + count > value.size()) return false;
        for (int j = 1; j < count; ++j)
        {
            const auto next = static_cast<std::uint8_t>(value[i + j]);
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
        i += count;
    }
    return true;
}

inline Type MessageType(const void* data, std::size_t size)
{
    if (size < 4 || !data) return static_cast<Type>(0);
    const auto* p = static_cast<const std::uint8_t*>(data);
    if (p[0] != 'C' || p[1] != 'N' || p[2] != Version) return static_cast<Type>(0);
    return static_cast<Type>(p[3]);
}

class Writer
{
public:
    explicit Writer(Type type) : bytes{'C', 'N', Version, static_cast<std::uint8_t>(type)} {}
    void U8(std::uint8_t value) { bytes.push_back(value); }
    void U32(std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i) U8(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    void U64(std::uint64_t value)
    {
        for (int i = 0; i < 8; ++i) U8(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    void F32(float value)
    {
        static_assert(sizeof(float) == sizeof(std::uint32_t), "Wire protocol needs 32-bit floats");
        std::uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        U32(bits);
    }
    std::vector<std::uint8_t> bytes;
};

class Reader
{
public:
    Reader(const void* data, std::size_t size, Type type)
        : p(static_cast<const std::uint8_t*>(data)), count(size), valid(MessageType(data, size) == type), offset(4) {}
    bool U8(std::uint8_t& value)
    {
        if (!valid || offset >= count) return valid = false;
        value = p[offset++];
        return true;
    }
    bool U32(std::uint32_t& value)
    {
        value = 0;
        for (int i = 0; i < 4; ++i)
        {
            std::uint8_t byte;
            if (!U8(byte)) return false;
            value |= std::uint32_t(byte) << (8 * i);
        }
        return true;
    }
    bool U64(std::uint64_t& value)
    {
        value = 0;
        for (int i = 0; i < 8; ++i)
        {
            std::uint8_t byte;
            if (!U8(byte)) return false;
            value |= std::uint64_t(byte) << (8 * i);
        }
        return true;
    }
    bool F32(float& value)
    {
        std::uint32_t bits;
        if (!U32(bits)) return false;
        std::memcpy(&value, &bits, sizeof(value));
        return valid = std::isfinite(value);
    }
    bool Done() const { return valid && offset == count; }
private:
    const std::uint8_t* p;
    std::size_t count;
    bool valid;
    std::size_t offset;
};

inline std::vector<std::uint8_t> Encode(const InputPacket& x)
{
    if (x.commands.empty() || x.commands.size() > MaxInputsPerPacket) return {};
    Writer w(Type::Input);
    w.U8(static_cast<std::uint8_t>(x.commands.size()));
    for (const auto& input : x.commands)
    {
        w.U32(input.sequence); w.U32(input.tick);
        w.F32(input.moveX); w.F32(input.moveY); w.F32(input.yaw); w.U8(input.jump ? 1 : 0);
    }
    return std::move(w.bytes);
}
inline std::vector<std::uint8_t> Encode(const Welcome& x)
{
    Writer w(Type::Welcome);
    w.U32(x.playerId); w.U64(x.levelHash);
    return std::move(w.bytes);
}

inline std::vector<std::uint8_t> Encode(const PlayerAppearance& x)
{
    Writer w(Type::PlayerAppearance);
    w.U32(x.serverTick); w.U32(x.playerId);
    w.U8(x.value);
    return std::move(w.bytes);
}

inline void WritePlayerState(Writer& w, const PlayerState& x, bool includeInputAck = false)
{
    w.U32(x.serverTick); w.U32(x.acknowledgedInputTick); w.U32(x.playerId);
    w.U32(x.receivedInputTick);
    if (includeInputAck) { w.U32(x.inputAckSequence); w.U64(x.inputAckBits); }
    w.F32(x.px); w.F32(x.py); w.F32(x.pz);
    w.F32(x.vx); w.F32(x.vy); w.F32(x.vz); w.F32(x.yaw);
    w.U8(x.grounded ? 1 : 0);
}
inline std::vector<std::uint8_t> Encode(const PlayerState& x)
{
    Writer w(Type::PlayerState);
    w.bytes.reserve(61);
    WritePlayerState(w, x, true);
    return std::move(w.bytes);
}
inline std::vector<std::uint8_t> Encode(const PlayerStateBatch& x)
{
    if (x.states.empty() || x.states.size() > MaxPlayerStatesPerBatch) return {};
    Writer w(Type::PlayerStateBatch);
    w.bytes.reserve(5 + 45 * x.states.size());
    w.U8(static_cast<std::uint8_t>(x.states.size()));
    for (const PlayerState& state : x.states) WritePlayerState(w, state);
    return std::move(w.bytes);
}
inline std::vector<std::uint8_t> Encode(const BodyState& x)
{
    Writer w(Type::BodyState);
    w.bytes.reserve(76);
    w.U32(x.serverTick);
    for (auto part : x.exportId) w.U32(part);
    w.F32(x.px); w.F32(x.py); w.F32(x.pz);
    w.F32(x.qx); w.F32(x.qy); w.F32(x.qz); w.F32(x.qw);
    w.F32(x.vx); w.F32(x.vy); w.F32(x.vz);
    w.F32(x.wx); w.F32(x.wy); w.F32(x.wz);
    return std::move(w.bytes);
}
inline std::vector<std::uint8_t> Encode(const PlayerName& x)
{
    Writer w(Type::PlayerName);
    const std::string& value = x.value;
    if (!ValidPlayerName(value)) return {};
    w.U8(static_cast<std::uint8_t>(value.size()));
    for (const unsigned char byte : value) w.U8(byte);
    return std::move(w.bytes);
}
inline std::vector<std::uint8_t> Encode(const DespawnPlayer& x)
{
    Writer w(Type::DespawnPlayer);
    w.U32(x.playerId); w.U32(x.serverTick);
    return std::move(w.bytes);
}

inline bool Decode(const void* data, std::size_t size, InputPacket& x)
{
    Reader r(data, size, Type::Input); std::uint8_t count = 0;
    if (!r.U8(count) || count == 0 || count > MaxInputsPerPacket) return false;
    InputPacket decoded;
    for (std::uint8_t i = 0; i < count; ++i)
    {
        Input input; std::uint8_t jump = 0;
        if (!r.U32(input.sequence) || !r.U32(input.tick) || !r.F32(input.moveX)
            || !r.F32(input.moveY) || !r.F32(input.yaw) || !r.U8(jump)
            || jump > 1 || input.sequence == 0 || input.tick == 0
            || input.moveX < -1 || input.moveX > 1 || input.moveY < -1 || input.moveY > 1
            || (!decoded.commands.empty() && input.sequence <= decoded.commands.back().sequence)) return false;
        input.jump = jump != 0;
        decoded.commands.push_back(input);
    }
    if (!r.Done()) return false;
    x = std::move(decoded);
    return true;
}
inline bool Decode(const void* data, std::size_t size, Welcome& x)
{
    Reader r(data, size, Type::Welcome);
    return r.U32(x.playerId) && r.U64(x.levelHash) && r.Done();
}
inline bool ReadPlayerState(Reader& r, PlayerState& x, bool includeInputAck = false)
{
    std::uint8_t grounded;
    if (!r.U32(x.serverTick) || !r.U32(x.acknowledgedInputTick) || !r.U32(x.playerId)
        || !r.U32(x.receivedInputTick)
        || (includeInputAck && (!r.U32(x.inputAckSequence) || !r.U64(x.inputAckBits)))
        || !r.F32(x.px) || !r.F32(x.py) || !r.F32(x.pz)
        || !r.F32(x.vx) || !r.F32(x.vy) || !r.F32(x.vz) || !r.F32(x.yaw)
        || !r.U8(grounded) || grounded > 1) return false;
    x.grounded = grounded != 0;
    return true;
}
inline bool Decode(const void* data, std::size_t size, PlayerState& x)
{
    Reader r(data, size, Type::PlayerState);
    return ReadPlayerState(r, x, true) && r.Done();
}
inline bool Decode(const void* data, std::size_t size, PlayerStateBatch& x)
{
    Reader r(data, size, Type::PlayerStateBatch);
    std::uint8_t count = 0;
    if (!r.U8(count) || count == 0 || count > MaxPlayerStatesPerBatch) return false;
    x.states.clear();
    x.states.reserve(count);
    for (std::uint8_t i = 0; i < count; ++i)
    {
        PlayerState state{};
        if (!ReadPlayerState(r, state)) return false;
        x.states.push_back(state);
    }
    return r.Done();
}
inline bool Decode(const void* data, std::size_t size, BodyState& x)
{
    Reader r(data, size, Type::BodyState);
    if (!r.U32(x.serverTick)) return false;
    for (auto& part : x.exportId) if (!r.U32(part)) return false;
    return r.F32(x.px) && r.F32(x.py) && r.F32(x.pz)
        && r.F32(x.qx) && r.F32(x.qy) && r.F32(x.qz) && r.F32(x.qw)
        && r.F32(x.vx) && r.F32(x.vy) && r.F32(x.vz)
        && r.F32(x.wx) && r.F32(x.wy) && r.F32(x.wz) && r.Done();
}
inline bool Decode(const void* data, std::size_t size, PlayerName& x)
{
    Reader r(data, size, Type::PlayerName);
    std::uint8_t length = 0;
    if (!r.U8(length) || length > 48) return false;
    x.value.clear();
    for (std::uint8_t i = 0; i < length; ++i)
    {
        std::uint8_t byte = 0;
        if (!r.U8(byte)) return false;
        x.value.push_back(static_cast<char>(byte));
    }
    return r.Done() && ValidPlayerName(x.value);
}

inline bool Decode(const void* data, std::size_t size, PlayerAppearance& x)
{
    Reader r(data, size, Type::PlayerAppearance);
    return r.U32(x.serverTick) && r.U32(x.playerId) && r.U8(x.value) && r.Done();
}

inline bool Decode(const void* data, std::size_t size, DespawnPlayer& x)
{
    Reader r(data, size, Type::DespawnPlayer);
    return r.U32(x.playerId) && r.U32(x.serverTick) && r.Done();
}
} // namespace clicknet::wire
