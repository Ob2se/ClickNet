#include "ClickNetLocalBox3DWorld.h"

#include "box3d/collision.h"
#include "HAL/FileManager.h"
#include "Serialization/Archive.h"

namespace
{
constexpr uint32 MaxShapeCount = 1000000;
constexpr int64 MaxFileBytes = 512ll * 1024 * 1024;
constexpr uint32 MaxHeightFieldAxis = 1025;
TMap<uint32, TArray<b3HeightFieldData*>> HeightFieldsByWorld;

struct FRecord
{
    uint8 Type = 0, Flags = 0, Category = 0;
    float Position[3] = {}, Rotation[4] = {}, Geometry[7] = {};
    uint32 CountX = 0, CountZ = 0;
    float Scale[3] = {}, Minimum = 0, Maximum = 0;
    bool bClockwise = false;
    TArray<float> Heights;
    TArray<uint8> Materials;
    FGuid ExportId;
};

class FReader
{
public:
    explicit FReader(const TArray<uint8>& InBytes) : Bytes(InBytes) {}
    bool U8(uint8& Value)
    {
        if (Offset >= Bytes.Num()) return false;
        Value = Bytes[Offset++];
        return true;
    }
    bool U16(uint16& Value)
    {
        uint8 A, B;
        if (!U8(A) || !U8(B)) return false;
        Value = uint16(A) | (uint16(B) << 8);
        return true;
    }
    bool U32(uint32& Value)
    {
        uint8 A, B, C, D;
        if (!U8(A) || !U8(B) || !U8(C) || !U8(D)) return false;
        Value = uint32(A) | (uint32(B) << 8) | (uint32(C) << 16) | (uint32(D) << 24);
        return true;
    }
    bool F32(float& Value)
    {
        static_assert(sizeof(float) == sizeof(uint32), "Box3D files require 32-bit floats");
        uint32 Bits;
        if (!U32(Bits)) return false;
        FMemory::Memcpy(&Value, &Bits, sizeof(Value));
        return FMath::IsFinite(Value);
    }
    bool Record(FRecord& R, uint16 Version)
    {
        uint8 Reserved;
        if (!U8(R.Type) || !U8(R.Flags) || !U8(R.Category) || !U8(Reserved)
            || Reserved != 0 || (R.Flags & ~uint8(3)) != 0
            || (R.Type != 0 && R.Type != 2 && !(Version == 3 && R.Type == 3))) return false;
        for (float& V : R.Position) if (!F32(V)) return false;
        for (float& V : R.Rotation) if (!F32(V)) return false;
        const double Length = double(R.Rotation[0]) * R.Rotation[0]
            + double(R.Rotation[1]) * R.Rotation[1]
            + double(R.Rotation[2]) * R.Rotation[2]
            + double(R.Rotation[3]) * R.Rotation[3];
        if (Length < 0.99 || Length > 1.01) return false;
        if (R.Type == 3)
        {
            uint8 Clockwise;
            if ((R.Flags & 2) != 0 || !U32(R.CountX) || !U32(R.CountZ)
                || R.CountX < 2 || R.CountZ < 2
                || R.CountX > MaxHeightFieldAxis || R.CountZ > MaxHeightFieldAxis) return false;
            for (float& V : R.Scale) if (!F32(V) || V <= 0) return false;
            if (!F32(R.Minimum) || !F32(R.Maximum) || R.Minimum >= R.Maximum
                || !U8(Clockwise) || Clockwise > 1) return false;
            R.bClockwise = Clockwise != 0;
            R.Heights.SetNumUninitialized(int32(R.CountX * R.CountZ));
            for (float& V : R.Heights)
                if (!F32(V) || V < R.Minimum || V > R.Maximum) return false;
            R.Materials.SetNumUninitialized(int32((R.CountX - 1) * (R.CountZ - 1)));
            for (uint8& V : R.Materials) if (!U8(V) || (V != 0 && V != 0xFF)) return false;
        }
        else
        {
            for (int32 I = 0, N = R.Type == 0 ? 3 : 7; I < N; ++I) if (!F32(R.Geometry[I])) return false;
            if (R.Type == 0)
            {
                if (R.Geometry[0] <= 0 || R.Geometry[1] <= 0 || R.Geometry[2] <= 0) return false;
            }
            else if (R.Geometry[6] <= 0) return false;
        }
        if (Version >= 2)
        {
            if (!U32(R.ExportId.A) || !U32(R.ExportId.B)
                || !U32(R.ExportId.C) || !U32(R.ExportId.D)
                || !R.ExportId.IsValid()) return false;
        }
        return true;
    }
    int64 Tell() const { return Offset; }
private:
    const TArray<uint8>& Bytes;
    int64 Offset = 0;
};

bool ReadHeader(FReader& Reader, uint16& Version, uint32& Count)
{
    uint8 Magic[4];
    uint16 Reserved;
    return Reader.U8(Magic[0]) && Reader.U8(Magic[1])
        && Reader.U8(Magic[2]) && Reader.U8(Magic[3])
        && Magic[0] == 'B' && Magic[1] == '3' && Magic[2] == 'D' && Magic[3] == 'F'
        && Reader.U16(Version) && (Version == 1 || Version == 2 || Version == 3)
        && Reader.U16(Reserved) && Reserved == 0
        && Reader.U32(Count) && Count > 0 && Count <= MaxShapeCount;
}
}

b3WorldId FClientClickNetLocalBox3DWorld::CreateLocalWorld()
{
    b3WorldDef Def = b3DefaultWorldDef();
    Def.gravity = b3Vec3(0.0f, 0.0f, -9.81f);
    return b3CreateWorld(&Def);
}

void FClientClickNetLocalBox3DWorld::DestroyLocalWorld(b3WorldId WorldId)
{
    if (!b3World_IsValid(WorldId)) return;
    const uint32 Key = b3StoreWorldId(WorldId);
    b3DestroyWorld(WorldId);
    if (TArray<b3HeightFieldData*>* Fields = HeightFieldsByWorld.Find(Key))
    {
        for (b3HeightFieldData* Field : *Fields) b3DestroyHeightField(Field);
        HeightFieldsByWorld.Remove(Key);
    }
}

bool FClientClickNetLocalBox3DWorld::ImportFile(const FString& FilePath,
    b3WorldId& OutWorldId, int32& OutBodyCount, FString& OutError,
    TArray<FClientBox3DImportedShape>* OutShapes)
{
    OutWorldId = b3_nullWorldId;
    OutBodyCount = 0;
    if (OutShapes) OutShapes->Reset();
    TUniquePtr<FArchive> File(IFileManager::Get().CreateFileReader(*FilePath));
    const int64 FileSize = File ? File->TotalSize() : -1;
    if (FileSize < 12 || FileSize > MaxFileBytes)
    {
        OutError = TEXT("Box3D file is missing or outside the supported size range.");
        return false;
    }
    TArray<uint8> Bytes;
    Bytes.SetNumUninitialized(FileSize);
    File->Serialize(Bytes.GetData(), FileSize);
    if (File->IsError() || !File->Close())
    {
        OutError = TEXT("Could not read the Box3D file completely.");
        return false;
    }
    File.Reset();
    FReader Validator(Bytes);
    uint16 Version;
    uint32 Count;
    if (!ReadHeader(Validator, Version, Count))
    {
        OutError = TEXT("Invalid Box3D file header or unsupported version.");
        return false;
    }
    TSet<FGuid> SeenIds;
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        FRecord Record;
        if (!Validator.Record(Record, Version)
            || (Version >= 2 && SeenIds.Contains(Record.ExportId)))
        {
            OutError = FString::Printf(TEXT("Invalid or truncated Box3D shape record %u."), Index);
            return false;
        }
        if (Version >= 2) SeenIds.Add(Record.ExportId);
    }
    if (Validator.Tell() != Bytes.Num())
    {
        OutError = TEXT("Unexpected data after the Box3D shape records.");
        return false;
    }
    b3WorldId NewWorld = CreateLocalWorld();
    if (!b3World_IsValid(NewWorld))
    {
        OutError = TEXT("Box3D could not create a client world.");
        return false;
    }
    FReader Reader(Bytes);
    uint16 ParsedVersion;
    uint32 ParsedCount;
    ReadHeader(Reader, ParsedVersion, ParsedCount); // Validated above.
    TArray<FClientBox3DImportedShape> Shapes;
    TArray<b3HeightFieldData*> HeightFields;
    auto FailWorld = [&]()
    {
        b3DestroyWorld(NewWorld);
        for (b3HeightFieldData* Field : HeightFields) b3DestroyHeightField(Field);
    };
    if (OutShapes) Shapes.Reserve(Count);
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        FRecord Record;
        Reader.Record(Record, Version);
        b3BodyDef BodyDef = b3DefaultBodyDef();
        BodyDef.type = (Record.Flags & 2) ? b3_dynamicBody : b3_staticBody;
        BodyDef.position = b3Pos{Record.Position[0], Record.Position[1], Record.Position[2]};
        BodyDef.rotation.v = b3Vec3(Record.Rotation[0], Record.Rotation[1], Record.Rotation[2]);
        BodyDef.rotation.s = Record.Rotation[3];
        const b3BodyId BodyId = b3CreateBody(NewWorld, &BodyDef);
        if (!b3Body_IsValid(BodyId))
        {
            OutError = FString::Printf(TEXT("Box3D could not create body %u."), Index);
            FailWorld();
            return false;
        }
        b3ShapeDef ShapeDef = b3DefaultShapeDef();
        ShapeDef.density = (Record.Flags & 2) ? 1.0f : 0.0f;
        ShapeDef.filter.categoryBits = Record.Category;
        ShapeDef.filter.maskBits = (Record.Flags & 1) ? UINT64_MAX : 0;
        b3ShapeId ShapeId;
        if (Record.Type == 0)
        {
            const b3BoxHull Box = b3MakeBoxHull(Record.Geometry[0], Record.Geometry[1], Record.Geometry[2]);
            ShapeId = b3CreateHullShape(BodyId, &ShapeDef, &Box.base);
        }
        else if (Record.Type == 2)
        {
            b3Capsule Capsule{};
            Capsule.center1 = b3Vec3(Record.Geometry[0], Record.Geometry[1], Record.Geometry[2]);
            Capsule.center2 = b3Vec3(Record.Geometry[3], Record.Geometry[4], Record.Geometry[5]);
            Capsule.radius = Record.Geometry[6];
            ShapeId = b3CreateCapsuleShape(BodyId, &ShapeDef, &Capsule);
        }
        else
        {
            b3HeightFieldDef FieldDef{};
            FieldDef.heights = Record.Heights.GetData();
            FieldDef.materialIndices = Record.Materials.GetData();
            FieldDef.countX = int(Record.CountX);
            FieldDef.countZ = int(Record.CountZ);
            FieldDef.scale = b3Vec3(Record.Scale[0], Record.Scale[1], Record.Scale[2]);
            FieldDef.globalMinimumHeight = Record.Minimum;
            FieldDef.globalMaximumHeight = Record.Maximum;
            FieldDef.clockwiseWinding = Record.bClockwise;
            b3HeightFieldData* Field = b3CreateHeightField(&FieldDef);
            if (!Field)
            {
                OutError = FString::Printf(TEXT("Box3D could not create heightfield %u."), Index);
                FailWorld();
                return false;
            }
            HeightFields.Add(Field);
            ShapeId = b3CreateHeightFieldShape(BodyId, &ShapeDef, Field);
        }
        if (!b3Shape_IsValid(ShapeId))
        {
            OutError = FString::Printf(TEXT("Box3D could not create shape %u."), Index);
            FailWorld();
            return false;
        }
        if (OutShapes && Record.Type != 3)
        {
            FClientBox3DImportedShape& Shape = Shapes.AddDefaulted_GetRef();
            Shape.BodyId = BodyId;
            Shape.Type = Record.Type;
            Shape.bDynamic = (Record.Flags & 2) != 0;
            Shape.ExportId = Record.ExportId;
            if (Record.Type == 0)
            {
                Shape.BoxHalfExtents = b3Vec3(Record.Geometry[0], Record.Geometry[1], Record.Geometry[2]);
            }
            else
            {
                Shape.CapsuleCenter1 = b3Vec3(Record.Geometry[0], Record.Geometry[1], Record.Geometry[2]);
                Shape.CapsuleCenter2 = b3Vec3(Record.Geometry[3], Record.Geometry[4], Record.Geometry[5]);
                Shape.CapsuleRadius = Record.Geometry[6];
            }
        }
    }
    OutWorldId = NewWorld;
    if (!HeightFields.IsEmpty()) HeightFieldsByWorld.Add(b3StoreWorldId(NewWorld), MoveTemp(HeightFields));
    OutBodyCount = int32(Count);
    if (OutShapes) *OutShapes = MoveTemp(Shapes);
    return true;
}
