#include "ClickNetLocalBox3DWorld.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClickNetBox3DImportTest,
    "ClickNet.Box3D.ImportStaticAndDynamic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClickNetBox3DImportTest::RunTest(const FString& Parameters)
{
    TArray<uint8> Bytes;
    auto U8 = [&Bytes](uint8 Value) { Bytes.Add(Value); };
    auto U16 = [&U8](uint16 Value) { U8(uint8(Value)); U8(uint8(Value >> 8)); };
    auto U32 = [&U8](uint32 Value) {
        U8(uint8(Value)); U8(uint8(Value >> 8)); U8(uint8(Value >> 16)); U8(uint8(Value >> 24));
    };
    auto F32 = [&U32](float Value) {
        uint32 Bits;
        FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
        U32(Bits);
    };
    auto Common = [&U8, &F32](uint8 Type, uint8 Flags, float Z) {
        U8(Type); U8(Flags); U8(1); U8(0);
        F32(0); F32(0); F32(Z);
        F32(0); F32(0); F32(0); F32(1);
    };
    U8('B'); U8('3'); U8('D'); U8('F');
    U16(2); U16(0); U32(2);
    Common(0, 1, -100.0f);
    F32(1); F32(1); F32(1);
    U32(1); U32(2); U32(3); U32(4);
    Common(2, 3, 5.0f);
    F32(0); F32(0); F32(-0.5f);
    F32(0); F32(0); F32(0.5f);
    F32(0.25f);
    U32(5); U32(6); U32(7); U32(8);

    const FString FilePath = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("Box3DImportTest_"), TEXT(".box3d"));
    if (!TestTrue(TEXT("Write test file"), FFileHelper::SaveArrayToFile(Bytes, *FilePath))) return false;

    b3WorldId WorldId = b3_nullWorldId;
    int32 BodyCount = 0;
    FString Error;
    TArray<FClientBox3DImportedShape> Shapes;
    const bool bImported = FClientClickNetLocalBox3DWorld::ImportFile(FilePath, WorldId, BodyCount, Error, &Shapes);
    TestTrue(TEXT("Import valid file: ") + Error, bImported);
    if (bImported)
    {
        const b3Counters Counters = b3World_GetCounters(WorldId);
        TestEqual(TEXT("Body count"), Counters.bodyCount, 2);
        TestEqual(TEXT("Shape count"), Counters.shapeCount, 2);
        TestEqual(TEXT("Imported count"), BodyCount, 2);
        TestEqual(TEXT("Debug shape count"), Shapes.Num(), 2);
        if (Shapes.Num() == 2)
        {
            TestFalse(TEXT("Box is static"), Shapes[0].bDynamic);
            TestTrue(TEXT("Capsule is dynamic"), Shapes[1].bDynamic);
            TestTrue(TEXT("Capsule body handle is valid"), b3Body_IsValid(Shapes[1].BodyId));
            TestTrue(TEXT("Capsule export ID"), Shapes[1].ExportId == FGuid(5, 6, 7, 8));
        }
        b3World_Step(WorldId, 1.0f / 60.0f, 4);
        const b3BodyEvents Events = b3World_GetBodyEvents(WorldId);
        TestEqual(TEXT("Only dynamic body moves"), Events.moveCount, 1);
        if (Events.moveCount == 1)
        {
            TestEqual(TEXT("Physics flag creates a dynamic body"), b3Body_GetType(Events.moveEvents[0].bodyId), b3_dynamicBody);
            TestTrue(TEXT("Dynamic body falls under gravity"), b3Body_GetPosition(Events.moveEvents[0].bodyId).z < 5.0);
        }
        FClientClickNetLocalBox3DWorld::DestroyLocalWorld(WorldId);
    }

    Bytes.Pop(); // A truncated component ID must fail before any body is created.
    FFileHelper::SaveArrayToFile(Bytes, *FilePath);
    WorldId = b3_nullWorldId;
    BodyCount = 0;
    Error.Empty();
    TestFalse(TEXT("Reject truncated file"),
        FClientClickNetLocalBox3DWorld::ImportFile(FilePath, WorldId, BodyCount, Error));
    TestFalse(TEXT("No partial world is returned"), b3World_IsValid(WorldId));
    IFileManager::Get().Delete(*FilePath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClickNetBox3DHeightFieldTest,
    "ClickNet.Box3D.ImportHeightField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClickNetBox3DHeightFieldTest::RunTest(const FString& Parameters)
{
    TArray<uint8> Bytes;
    auto U8 = [&Bytes](uint8 V) { Bytes.Add(V); };
    auto U32 = [&U8](uint32 V) {
        U8(uint8(V)); U8(uint8(V >> 8)); U8(uint8(V >> 16)); U8(uint8(V >> 24));
    };
    auto F32 = [&U32](float V) {
        uint32 Bits;
        FMemory::Memcpy(&Bits, &V, sizeof(Bits));
        U32(Bits);
    };
    U8('B'); U8('3'); U8('D'); U8('F'); U8(3); U8(0); U8(0); U8(0); U32(1);
    U8(3); U8(1); U8(1); U8(0); // Static heightfield with collision.
    F32(0); F32(0); F32(0); // World position.
    const FQuat Q(FVector::ForwardVector, UE_HALF_PI);
    F32(float(Q.X)); F32(float(Q.Y)); F32(float(Q.Z)); F32(float(Q.W));
    U32(2); U32(2);
    F32(2); F32(1); F32(2); // X, height, Z scale.
    F32(-256); F32(256); U8(0);
    for (int32 I = 0; I < 4; ++I) F32(0);
    U8(0); // One solid cell.
    U32(9); U32(10); U32(11); U32(12);

    const FString FilePath = FPaths::CreateTempFilename(*FPaths::ProjectSavedDir(), TEXT("Box3DHeightFieldTest_"), TEXT(".box3d"));
    if (!TestTrue(TEXT("Write heightfield file"), FFileHelper::SaveArrayToFile(Bytes, *FilePath))) return false;
    b3WorldId WorldId = b3_nullWorldId;
    int32 BodyCount = 0;
    FString Error;
    const bool bImported = FClientClickNetLocalBox3DWorld::ImportFile(FilePath, WorldId, BodyCount, Error);
    TestTrue(TEXT("Import heightfield: ") + Error, bImported);
    if (bImported)
    {
        TestEqual(TEXT("Terrain body count"), BodyCount, 1);
        b3BodyDef BodyDef = b3DefaultBodyDef();
        BodyDef.type = b3_dynamicBody;
        BodyDef.position = b3Pos{1.0f, -1.0f, 1.0f};
        const b3BodyId Body = b3CreateBody(WorldId, &BodyDef);
        b3ShapeDef ShapeDef = b3DefaultShapeDef();
        ShapeDef.density = 1;
        const b3BoxHull Box = b3MakeBoxHull(0.2f, 0.2f, 0.2f);
        b3CreateHullShape(Body, &ShapeDef, &Box.base);
        for (int32 I = 0; I < 120; ++I) b3World_Step(WorldId, 1.0f / 60.0f, 4);
        TestTrue(TEXT("Body rests on heightfield"), b3Body_GetPosition(Body).z > 0.1f);
        FClientClickNetLocalBox3DWorld::DestroyLocalWorld(WorldId);
    }
    IFileManager::Get().Delete(*FilePath);
    return true;
}
#endif
