#pragma once

#include "box3d/box3d.h"
#include "box3d/collision.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace clicknet
{
// B3DF is a little-endian, meter-based level file. IDs are the four uint32 GUID
// words written by the Unreal exporter; version 1 records have no ID.
struct ImportedShape
{
    b3BodyId bodyId = b3_nullBodyId;
    std::array<uint32_t, 4> exportId{};
    uint8_t type = 0; // 0 box, 2 capsule, 3 heightfield
    bool dynamic = false;
};

// Owns the Box3D world and the heightfield data referenced by its shapes.
// Destroy movers before their World. LoadFile replaces the entire current world.
class World
{
public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    bool LoadFile(const std::string& path, std::string& error);
    bool LoadBytes(const std::vector<uint8_t>& bytes, std::string& error);
    void Step(float seconds, int substeps = 4);
    b3WorldId Id() const { return worldId_; }
    const std::vector<ImportedShape>& Shapes() const { return shapes_; }
    uint64_t LevelHash() const { return levelHash_; }

private:
    void Reset();
    b3WorldId worldId_ = b3_nullWorldId;
    std::vector<b3HeightFieldData*> heightFields_;
    std::vector<ImportedShape> shapes_;
    uint64_t levelHash_ = 0;
};

struct MoverState
{
    b3Pos position{};
    b3Vec3 velocity{};
    bool grounded = false;
};

// A per-player capsule mover. Simulate sets the proxy's target. Call World::Step
// after all players have simulated to advance dynamic bodies and proxy positions.
class Mover
{
public:
    Mover(World& world, const b3Capsule& capsule, b3Pos start,
          uint64_t categoryBits = 1ull << 2);
    Mover(b3WorldId worldId, const b3Capsule& capsule, b3Pos start,
          uint64_t categoryBits = 1ull << 2);
    ~Mover();
    Mover(const Mover&) = delete;
    Mover& operator=(const Mover&) = delete;

    bool Simulate(b3Vec3 desiredPlanarVelocity, bool jump, float seconds,
                  float jumpSpeed = 5.0f);
    const MoverState& State() const { return state_; }
    b3BodyId ProxyBodyId() const { return proxyBodyId_; }
    void ResetState(const MoverState& state);

private:
    b3WorldId worldId_ = b3_nullWorldId;
    b3Capsule capsule_{};
    uint64_t categoryBits_;
    b3BodyId proxyBodyId_ = b3_nullBodyId;
    MoverState state_{};
};
} // namespace clicknet
