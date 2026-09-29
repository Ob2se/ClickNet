#include "ClickNetShared.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <unordered_set>
#include <utility>

namespace clicknet
{
namespace
{
constexpr size_t maxFileBytes = 512u * 1024u * 1024u;
constexpr uint32_t maxShapes = 1000000;
constexpr uint32_t maxHeightAxis = 1025;

struct Record
{
    uint8_t type = 0, flags = 0, category = 0;
    float position[3]{}, rotation[4]{}, geometry[7]{};
    uint32_t countX = 0, countZ = 0;
    float scale[3]{}, minimum = 0, maximum = 0;
    bool clockwise = false;
    std::vector<float> heights;
    std::vector<uint8_t> materials;
    std::array<uint32_t, 4> exportId{};
};

class Reader
{
public:
    explicit Reader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
    bool U8(uint8_t& value)
    {
        if (offset_ >= bytes_.size()) return false;
        value = bytes_[offset_++];
        return true;
    }
    bool U16(uint16_t& value)
    {
        uint8_t a, b;
        if (!U8(a) || !U8(b)) return false;
        value = uint16_t(a) | uint16_t(uint16_t(b) << 8);
        return true;
    }
    bool U32(uint32_t& value)
    {
        uint8_t a, b, c, d;
        if (!U8(a) || !U8(b) || !U8(c) || !U8(d)) return false;
        value = uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
        return true;
    }
    bool F32(float& value)
    {
        static_assert(sizeof(float) == sizeof(uint32_t));
        uint32_t bits;
        if (!U32(bits)) return false;
        std::memcpy(&value, &bits, sizeof(value));
        return std::isfinite(value);
    }
    size_t Offset() const { return offset_; }

    bool Header(uint16_t& version, uint32_t& count)
    {
        uint8_t magic[4];
        uint16_t reserved;
        return U8(magic[0]) && U8(magic[1]) && U8(magic[2]) && U8(magic[3])
            && magic[0] == 'B' && magic[1] == '3' && magic[2] == 'D' && magic[3] == 'F'
            && U16(version) && (version == 1 || version == 2 || version == 3)
            && U16(reserved) && reserved == 0
            && U32(count) && count > 0 && count <= maxShapes;
    }

    bool Shape(Record& record, uint16_t version)
    {
        uint8_t reserved;
        if (!U8(record.type) || !U8(record.flags) || !U8(record.category) || !U8(reserved)
            || reserved != 0 || (record.flags & ~uint8_t(3)) != 0
            || (record.type != 0 && record.type != 2 && !(version == 3 && record.type == 3))) return false;
        for (float& value : record.position) if (!F32(value)) return false;
        for (float& value : record.rotation) if (!F32(value)) return false;
        double length = 0;
        for (float value : record.rotation) length += double(value) * value;
        if (length < 0.99 || length > 1.01) return false;
        if (record.type == 3)
        {
            uint8_t clockwise;
            if ((record.flags & 2) != 0 || !U32(record.countX) || !U32(record.countZ)
                || record.countX < 2 || record.countZ < 2
                || record.countX > maxHeightAxis || record.countZ > maxHeightAxis) return false;
            for (float& value : record.scale) if (!F32(value) || value <= 0) return false;
            if (!F32(record.minimum) || !F32(record.maximum) || record.minimum >= record.maximum
                || !U8(clockwise) || clockwise > 1) return false;
            record.clockwise = clockwise != 0;
            record.heights.resize(size_t(record.countX) * record.countZ);
            for (float& value : record.heights)
                if (!F32(value) || value < record.minimum || value > record.maximum) return false;
            record.materials.resize(size_t(record.countX - 1) * (record.countZ - 1));
            for (uint8_t& value : record.materials)
                if (!U8(value) || (value != 0 && value != 0xFF)) return false;
        }
        else
        {
            for (int i = 0, n = record.type == 0 ? 3 : 7; i < n; ++i)
                if (!F32(record.geometry[i])) return false;
            if (record.type == 0)
            {
                if (record.geometry[0] <= 0 || record.geometry[1] <= 0 || record.geometry[2] <= 0) return false;
            }
            else if (record.geometry[6] <= 0) return false;
        }
        if (version >= 2)
        {
            bool nonzero = false;
            for (uint32_t& word : record.exportId)
            {
                if (!U32(word)) return false;
                nonzero |= word != 0;
            }
            if (!nonzero) return false;
        }
        return true;
    }

private:
    const std::vector<uint8_t>& bytes_;
    size_t offset_ = 0;
};

uint64_t HashBytes(const std::vector<uint8_t>& bytes)
{
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t value : bytes)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool OnPlane(b3ShapeId shapeId, const b3PlaneResult* results, int count, void* context)
{
    if (b3Body_GetType(b3Shape_GetBody(shapeId)) == b3_dynamicBody) return true;
    auto& planes = *static_cast<std::vector<b3CollisionPlane>*>(context);
    for (int i = 0; i < count; ++i)
    {
        b3CollisionPlane plane{};
        plane.plane = results[i].plane;
        plane.pushLimit = FLT_MAX;
        plane.push = 0;
        plane.clipVelocity = true;
        planes.push_back(plane);
    }
    return true;
}

bool OnCast(b3ShapeId shapeId, void*)
{
    return b3Body_GetType(b3Shape_GetBody(shapeId)) != b3_dynamicBody;
}

bool Walkable(const std::vector<b3CollisionPlane>& planes)
{
    for (const b3CollisionPlane& plane : planes)
        if (plane.plane.normal.z > 0.7f) return true;
    return false;
}

b3CollisionPlane* PlaneData(std::vector<b3CollisionPlane>& planes)
{
    return planes.empty() ? nullptr : planes.data();
}
} // namespace

World::World()
{
    b3WorldDef def = b3DefaultWorldDef();
    def.gravity = b3Vec3{0, 0, -9.81f};
    worldId_ = b3CreateWorld(&def);
}

World::~World() { Reset(); }

void World::Reset()
{
    if (b3World_IsValid(worldId_)) b3DestroyWorld(worldId_);
    worldId_ = b3_nullWorldId;
    for (b3HeightFieldData* field : heightFields_) b3DestroyHeightField(field);
    heightFields_.clear();
    shapes_.clear();
    levelHash_ = 0;
}

bool World::LoadFile(const std::string& path, std::string& error)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        error = "Cannot open B3DF file: " + path;
        return false;
    }
    const std::streamoff length = file.tellg();
    if (length < 12 || uint64_t(length) > maxFileBytes)
    {
        error = "B3DF file size is outside supported range";
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), length))
    {
        error = "Cannot read complete B3DF file";
        return false;
    }
    return LoadBytes(bytes, error);
}

bool World::LoadBytes(const std::vector<uint8_t>& bytes, std::string& error)
{
    error.clear();
    if (bytes.size() < 12 || bytes.size() > maxFileBytes)
    {
        error = "B3DF file size is outside supported range";
        return false;
    }
    Reader reader(bytes);
    uint16_t version;
    uint32_t count;
    if (!reader.Header(version, count))
    {
        error = "Invalid B3DF header or unsupported version";
        return false;
    }
    std::vector<Record> records;
    records.reserve(count);
    std::unordered_set<std::string> seenIds;
    for (uint32_t i = 0; i < count; ++i)
    {
        Record record;
        if (!reader.Shape(record, version))
        {
            error = "Invalid B3DF shape record " + std::to_string(i);
            return false;
        }
        if (version >= 2)
        {
            std::string id(reinterpret_cast<const char*>(record.exportId.data()), sizeof(record.exportId));
            if (!seenIds.insert(id).second)
            {
                error = "Duplicate B3DF export ID at record " + std::to_string(i);
                return false;
            }
        }
        records.push_back(std::move(record));
    }
    if (reader.Offset() != bytes.size())
    {
        error = "Trailing data after B3DF shape records";
        return false;
    }

    // All bytes are validated before replacing the live world.
    Reset();
    b3WorldDef worldDef = b3DefaultWorldDef();
    worldDef.gravity = b3Vec3{0, 0, -9.81f};
    worldId_ = b3CreateWorld(&worldDef);
    if (!b3World_IsValid(worldId_))
    {
        error = "Box3D could not create world";
        return false;
    }
    shapes_.reserve(records.size());
    for (size_t i = 0; i < records.size(); ++i)
    {
        Record& record = records[i];
        b3BodyDef bodyDef = b3DefaultBodyDef();
        bodyDef.type = (record.flags & 2) ? b3_dynamicBody : b3_staticBody;
        bodyDef.position = b3Pos{record.position[0], record.position[1], record.position[2]};
        bodyDef.rotation.v = b3Vec3{record.rotation[0], record.rotation[1], record.rotation[2]};
        bodyDef.rotation.s = record.rotation[3];
        b3BodyId body = b3CreateBody(worldId_, &bodyDef);
        if (!b3Body_IsValid(body))
        {
            error = "Box3D could not create body " + std::to_string(i);
            Reset();
            return false;
        }
        b3ShapeDef shapeDef = b3DefaultShapeDef();
        shapeDef.density = (record.flags & 2) ? 1.0f : 0.0f;
        shapeDef.filter.categoryBits = record.category;
        shapeDef.filter.maskBits = (record.flags & 1) ? UINT64_MAX : 0;
        b3ShapeId shape = b3_nullShapeId;
        if (record.type == 0)
        {
            b3BoxHull box = b3MakeBoxHull(record.geometry[0], record.geometry[1], record.geometry[2]);
            shape = b3CreateHullShape(body, &shapeDef, &box.base);
        }
        else if (record.type == 2)
        {
            b3Capsule capsule{};
            capsule.center1 = b3Vec3{record.geometry[0], record.geometry[1], record.geometry[2]};
            capsule.center2 = b3Vec3{record.geometry[3], record.geometry[4], record.geometry[5]};
            capsule.radius = record.geometry[6];
            shape = b3CreateCapsuleShape(body, &shapeDef, &capsule);
        }
        else
        {
            b3HeightFieldDef fieldDef{};
            fieldDef.heights = record.heights.data();
            fieldDef.materialIndices = record.materials.data();
            fieldDef.countX = int(record.countX);
            fieldDef.countZ = int(record.countZ);
            fieldDef.scale = b3Vec3{record.scale[0], record.scale[1], record.scale[2]};
            fieldDef.globalMinimumHeight = record.minimum;
            fieldDef.globalMaximumHeight = record.maximum;
            fieldDef.clockwiseWinding = record.clockwise;
            b3HeightFieldData* field = b3CreateHeightField(&fieldDef);
            if (field)
            {
                heightFields_.push_back(field);
                shape = b3CreateHeightFieldShape(body, &shapeDef, field);
            }
        }
        if (!b3Shape_IsValid(shape))
        {
            error = "Box3D could not create shape " + std::to_string(i);
            Reset();
            return false;
        }
        shapes_.push_back(ImportedShape{body, record.exportId, record.type, (record.flags & 2) != 0});
    }
    levelHash_ = HashBytes(bytes);
    return true;
}

void World::Step(float seconds, int substeps)
{
    if (b3World_IsValid(worldId_) && seconds > 0 && std::isfinite(seconds))
        b3World_Step(worldId_, seconds, std::max(1, substeps));
}

Mover::Mover(World& world, const b3Capsule& capsule, b3Pos start, uint64_t categoryBits)
    : Mover(world.Id(), capsule, start, categoryBits) {}

Mover::Mover(b3WorldId worldId, const b3Capsule& capsule, b3Pos start, uint64_t categoryBits)
    : worldId_(worldId), capsule_(capsule), categoryBits_(categoryBits)
{
    state_.position = start;
    if (!b3World_IsValid(worldId) || capsule.radius <= 0) return;
    b3BodyDef bodyDef = b3DefaultBodyDef();
    bodyDef.type = b3_kinematicBody;
    bodyDef.position = start;
    bodyDef.enableSleep = false;
    b3BodyId body = b3CreateBody(worldId, &bodyDef);
    if (!b3Body_IsValid(body)) return;
    b3ShapeDef shapeDef = b3DefaultShapeDef();
    shapeDef.filter.categoryBits = categoryBits;
    shapeDef.filter.maskBits = UINT64_MAX;
    if (!b3Shape_IsValid(b3CreateCapsuleShape(body, &shapeDef, &capsule)))
    {
        b3DestroyBody(body);
        return;
    }
    proxyBodyId_ = body;
}

Mover::~Mover()
{
    if (b3Body_IsValid(proxyBodyId_)) b3DestroyBody(proxyBodyId_);
}

void Mover::ResetState(const MoverState& state)
{
    state_ = state;
    if (b3Body_IsValid(proxyBodyId_)) b3Body_SetTransform(proxyBodyId_, state.position, b3Quat_identity);
}

bool Mover::Simulate(b3Vec3 desiredPlanarVelocity, bool jump, float seconds, float jumpSpeed)
{
    if (!b3World_IsValid(worldId_) || !b3Body_IsValid(proxyBodyId_)
        || !std::isfinite(seconds) || seconds <= 0 || capsule_.radius <= 0) return false;
    b3WorldId world = worldId_;
    b3Pos& position = state_.position;
    b3Vec3& velocity = state_.velocity;
    bool& grounded = state_.grounded;
    velocity.x = desiredPlanarVelocity.x;
    velocity.y = desiredPlanarVelocity.y;
    if (jump && grounded)
    {
        velocity.z = jumpSpeed;
        grounded = false;
    }
    const bool wasGrounded = grounded;
    velocity.z += b3World_GetGravity(world).z * seconds;
    b3QueryFilter filter = b3DefaultQueryFilter();
    filter.categoryBits = categoryBits_;
    filter.maskBits &= ~categoryBits_;
    std::vector<b3CollisionPlane> planes;
    b3World_CollideMover(world, position, &capsule_, filter, OnPlane, &planes);
    for (const b3CollisionPlane& plane : planes)
        if (plane.plane.normal.z > 0.7f && velocity.z < 0) velocity.z = 0;
    b3PlaneSolverResult solved = b3SolvePlanes(velocity * seconds, PlaneData(planes), int(planes.size()));
    float fraction = b3World_CastMover(world, position, &capsule_, solved.delta, filter, OnCast, nullptr);
    position = position + solved.delta * fraction;

    planes.clear();
    b3World_CollideMover(world, position, &capsule_, filter, OnPlane, &planes);
    grounded = false;
    if (velocity.z <= 0 && Walkable(planes))
    {
        grounded = true;
        velocity.z = 0;
    }
    b3PlaneSolverResult correction = b3SolvePlanes(b3Vec3{0, 0, 0}, PlaneData(planes), int(planes.size()));
    position = position + correction.delta;
    velocity = b3ClipVector(velocity, PlaneData(planes), int(planes.size()));

    if (wasGrounded && !grounded)
    {
        constexpr float snapDistance = 0.25f;
        const b3Vec3 down{0, 0, -snapDistance};
        float snapFraction = b3World_CastMover(world, position, &capsule_, down, filter, OnCast, nullptr);
        if (snapFraction < 1)
        {
            b3Pos candidate = position;
            candidate.z -= std::min(snapDistance, snapDistance * snapFraction + 0.01f);
            std::vector<b3CollisionPlane> groundPlanes;
            b3World_CollideMover(world, candidate, &capsule_, filter, OnPlane, &groundPlanes);
            if (Walkable(groundPlanes))
            {
                b3PlaneSolverResult snapCorrection = b3SolvePlanes(
                    b3Vec3{0, 0, 0}, PlaneData(groundPlanes), int(groundPlanes.size()));
                position = candidate + snapCorrection.delta;
                velocity.z = 0;
                velocity = b3ClipVector(velocity, PlaneData(groundPlanes), int(groundPlanes.size()));
                grounded = true;
            }
        }
    }
    b3Pos current = b3Body_GetPosition(proxyBodyId_);
    b3Vec3 toTarget{position.x - current.x, position.y - current.y, position.z - current.z};
    b3Body_SetLinearVelocity(proxyBodyId_, toTarget * (1.0f / seconds));
    return true;
}
} // namespace clicknet
