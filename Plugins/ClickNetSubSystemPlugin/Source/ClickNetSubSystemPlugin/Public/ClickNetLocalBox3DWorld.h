#pragma once

#include "CoreMinimal.h"
#include "box3d/box3d.h"

struct FClientBox3DImportedShape
{
    b3BodyId BodyId = b3_nullBodyId;
    uint8 Type = 0;
    bool bDynamic = false;
    FGuid ExportId;
    b3Vec3 BoxHalfExtents = b3Vec3(0, 0, 0);
    b3Vec3 CapsuleCenter1 = b3Vec3(0, 0, 0);
    b3Vec3 CapsuleCenter2 = b3Vec3(0, 0, 0);
    float CapsuleRadius = 0;
};

class CLICKNETSUBSYSTEMPLUGIN_API FClientClickNetLocalBox3DWorld
{
public:
    static b3WorldId CreateLocalWorld();
    // Releases heightfield data after destroying the shapes that reference it.
    static void DestroyLocalWorld(b3WorldId WorldId);
    // Creates a complete replacement world. On failure OutWorldId remains null.
    static bool ImportFile(const FString& FilePath, b3WorldId& OutWorldId,
        int32& OutBodyCount, FString& OutError,
        TArray<FClientBox3DImportedShape>* OutShapes = nullptr);

    
};
