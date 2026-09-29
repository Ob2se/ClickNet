#include "Box3DFileCreator.h"

#include "BaseBox3DComponent.h"
#include "Box3DBoxComponent.h"
#include "Box3DCapsuleComponent.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "LandscapeComponent.h"
#include "LandscapeDataAccess.h"
#include "LandscapeEdit.h"
#include "LandscapeProxy.h"
#include "IDesktopPlatform.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"

namespace
{
// Version 2 is little-endian and uses meters. The 12-byte header is B3DF,
// a u16 version, a reserved u16, and a u32 record count. Each record has
// type/flags/category/reserved bytes, position (3 f32), rotation (4 f32),
// then box half extents (3 f32) or capsule endpoints and radius (7 f32),
// followed by a 16-byte component GUID.
constexpr uint32 MaxShapeCount = 1000000;
constexpr int32 MaxHeightFieldAxis = 1025;

void WriteU8(FArchive& Ar, uint8 Value)
{
    Ar.Serialize(&Value, 1);
}

void WriteU16(FArchive& Ar, uint16 Value)
{
    uint8 Bytes[2] = {uint8(Value), uint8(Value >> 8)};
    Ar.Serialize(Bytes, sizeof(Bytes));
}

void WriteU32(FArchive& Ar, uint32 Value)
{
    uint8 Bytes[4] = {
        uint8(Value), uint8(Value >> 8), uint8(Value >> 16), uint8(Value >> 24)};
    Ar.Serialize(Bytes, sizeof(Bytes));
}

void WriteF32(FArchive& Ar, float Value)
{
    static_assert(sizeof(float) == sizeof(uint32), "Box3D export requires 32-bit floats");
    uint32 Bits;
    FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
    WriteU32(Ar, Bits);
}

void WriteGuid(FArchive& Ar, const FGuid& Guid)
{
    WriteU32(Ar, Guid.A);
    WriteU32(Ar, Guid.B);
    WriteU32(Ar, Guid.C);
    WriteU32(Ar, Guid.D);
}

bool IsFinite(const b3Vec3& Value)
{
    return FMath::IsFinite(Value.x) && FMath::IsFinite(Value.y) && FMath::IsFinite(Value.z);
}

bool IsValidCommon(const FShapeData& Shape)
{
    const float RotationLengthSquared = Shape.WorldRotation.v.x * Shape.WorldRotation.v.x
        + Shape.WorldRotation.v.y * Shape.WorldRotation.v.y
        + Shape.WorldRotation.v.z * Shape.WorldRotation.v.z
        + Shape.WorldRotation.s * Shape.WorldRotation.s;
    return IsFinite(Shape.WorldPosition)
        && IsFinite(Shape.WorldRotation.v)
        && FMath::IsFinite(Shape.WorldRotation.s)
        && FMath::IsFinite(RotationLengthSquared)
        && RotationLengthSquared > 0.000001f
        && Shape.CollisionBit >= 0 && Shape.CollisionBit <= 255;
}

void WriteCommon(FArchive& Ar, const FShapeData& Shape)
{
    const uint8 Flags = (Shape.bHasCollision ? 1 : 0) | (Shape.bHasPhysics ? 2 : 0);
    WriteU8(Ar, static_cast<uint8>(Shape.ShapeType));
    WriteU8(Ar, Flags);
    WriteU8(Ar, static_cast<uint8>(Shape.CollisionBit));
    WriteU8(Ar, 0);

    WriteF32(Ar, Shape.WorldPosition.x);
    WriteF32(Ar, Shape.WorldPosition.y);
    WriteF32(Ar, Shape.WorldPosition.z);
    WriteF32(Ar, Shape.WorldRotation.v.x);
    WriteF32(Ar, Shape.WorldRotation.v.y);
    WriteF32(Ar, Shape.WorldRotation.v.z);
    WriteF32(Ar, Shape.WorldRotation.s);
}

bool WriteShape(FArchive& Ar, const UBaseBox3DComponent* Component, FString& OutError)
{
    const FShapeData Shape = Component->GenerateShape();
    if (!IsValidCommon(Shape))
    {
        OutError = FString::Printf(TEXT("Invalid transform or collision category: %s"), *Component->GetPathName());
        return false;
    }

    const FVector Scale = Component->GetComponentScale();
    if (Scale.ContainsNaN())
    {
        OutError = FString::Printf(TEXT("Invalid scale: %s"), *Component->GetPathName());
        return false;
    }

    if (const UBox3DBoxComponent* Box = Cast<UBox3DBoxComponent>(Component))
    {
        if (Shape.ShapeType != EBox3DShapeType::Box)
        {
            OutError = FString::Printf(TEXT("Box shape type mismatch: %s"), *Box->GetPathName());
            return false;
        }

        // Editor dimensions are centimeters; Box3D and the file use meters.
        const float X = Shape.x * FMath::Abs(float(Scale.X)) * 0.01f;
        const float Y = Shape.y * FMath::Abs(float(Scale.Y)) * 0.01f;
        const float Z = Shape.z * FMath::Abs(float(Scale.Z)) * 0.01f;
        if (!FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z)
            || X <= 0 || Y <= 0 || Z <= 0)
        {
            OutError = FString::Printf(TEXT("Box half extents must be positive and finite: %s"), *Box->GetPathName());
            return false;
        }

        WriteCommon(Ar, Shape);
        WriteF32(Ar, X); WriteF32(Ar, Y); WriteF32(Ar, Z);
        WriteGuid(Ar, Component->ExportId);
        return true;
    }

    if (const UBox3DCapsuleComponent* Capsule = Cast<UBox3DCapsuleComponent>(Component))
    {
        if (Shape.ShapeType != EBox3DShapeType::Capsule)
        {
            OutError = FString::Printf(TEXT("Capsule shape type mismatch: %s"), *Capsule->GetPathName());
            return false;
        }

        const b3Vec3 A(Shape.centerOne.x * float(Scale.X),
            Shape.centerOne.y * float(Scale.Y), Shape.centerOne.z * float(Scale.Z));
        const b3Vec3 B(Shape.centerTwo.x * float(Scale.X),
            Shape.centerTwo.y * float(Scale.Y), Shape.centerTwo.z * float(Scale.Z));
        const float Radius = Shape.radius * float(Scale.GetAbsMax()) * 0.01f;
        if (!IsFinite(A) || !IsFinite(B) || !FMath::IsFinite(Radius) || Radius <= 0)
        {
            OutError = FString::Printf(TEXT("Capsule dimensions must be positive and finite: %s"), *Capsule->GetPathName());
            return false;
        }

        WriteCommon(Ar, Shape);
        WriteF32(Ar, A.x); WriteF32(Ar, A.y); WriteF32(Ar, A.z);
        WriteF32(Ar, B.x); WriteF32(Ar, B.y); WriteF32(Ar, B.z);
        WriteF32(Ar, Radius);
        WriteGuid(Ar, Component->ExportId);
        return true;
    }

    OutError = FString::Printf(TEXT("Unsupported Box3D component: %s"), *Component->GetPathName());
    return false;
}

bool WriteLandscapeComponent(FArchive& Ar, ULandscapeComponent* Component, FString& OutError)
{
    ULandscapeInfo* Info = Component->GetLandscapeInfo();
    if (!Info)
    {
        OutError = FString::Printf(TEXT("Landscape has no info: %s"), *Component->GetPathName());
        return false;
    }
    const FIntRect Extent = Component->GetComponentExtent();
    const int32 MinX = Extent.Min.X;
    const int32 MinY = Extent.Min.Y;
    const int32 MaxX = Extent.Max.X;
    const int32 MaxY = Extent.Max.Y;
    const int32 CountX = MaxX - MinX + 1;
    const int32 CountZ = MaxY - MinY + 1;
    const FVector Scale = Component->GetComponentScale();
    if (CountX < 2 || CountZ < 2 || CountX > MaxHeightFieldAxis || CountZ > MaxHeightFieldAxis
        || Scale.ContainsNaN() || Scale.X <= 0 || Scale.Y <= 0 || Scale.Z <= 0)
    {
        OutError = FString::Printf(TEXT("Unsupported Landscape size or scale (%d x %d, scale %.3f, %.3f, %.3f): %s"),
            CountX, CountZ, Scale.X, Scale.Y, Scale.Z, *Component->GetPathName());
        return false;
    }

    TArray<uint16> HeightSamples;
    HeightSamples.SetNumUninitialized(CountX * CountZ);
    FLandscapeEditDataInterface Edit(Info);
    Edit.GetHeightDataFast(MinX, MinY, MaxX, MaxY, HeightSamples.GetData(), CountX);

    TArray<uint8> Visibility;
    if (ALandscapeProxy::VisibilityLayer)
    {
        Visibility.SetNumZeroed(CountX * CountZ);
        Edit.GetWeightDataFast(ALandscapeProxy::VisibilityLayer, MinX, MinY, MaxX, MaxY,
            Visibility.GetData(), CountX);
    }

    // Box3D heightfields rise along local Y and advance along local X/Z.
    // A +90 degree X rotation maps local +Y to Unreal +Z and +Z to Unreal -Y.
    const FVector Origin = Component->GetComponentTransform().TransformPosition(FVector(0, CountZ - 1, 0));
    const FQuat Rotation = Component->GetComponentQuat() * FQuat(FVector::ForwardVector, UE_HALF_PI);
    if (Origin.ContainsNaN() || Rotation.ContainsNaN())
    {
        OutError = FString::Printf(TEXT("Invalid Landscape transform: %s"), *Component->GetPathName());
        return false;
    }
    WriteU8(Ar, 3); // Heightfield, version 3.
    WriteU8(Ar, 1); // Collision enabled, static.
    WriteU8(Ar, 1); // Default collision category.
    WriteU8(Ar, 0);
    WriteF32(Ar, float(Origin.X) * 0.01f);
    WriteF32(Ar, float(Origin.Y) * 0.01f);
    WriteF32(Ar, float(Origin.Z) * 0.01f);
    WriteF32(Ar, float(Rotation.X)); WriteF32(Ar, float(Rotation.Y));
    WriteF32(Ar, float(Rotation.Z)); WriteF32(Ar, float(Rotation.W));
    WriteU32(Ar, uint32(CountX)); WriteU32(Ar, uint32(CountZ));
    WriteF32(Ar, float(Scale.X) * 0.01f);
    WriteF32(Ar, float(Scale.Z) * 0.01f);
    WriteF32(Ar, float(Scale.Y) * 0.01f);
    // A shared range keeps adjacent components' quantized border vertices identical.
    WriteF32(Ar, LandscapeDataAccess::GetLocalHeight(0));
    WriteF32(Ar, LandscapeDataAccess::GetLocalHeight(65535));
    WriteU8(Ar, 0); // Counter-clockwise winding in Box3D local X/Z.
    for (int32 Z = 0; Z < CountZ; ++Z)
    {
        for (int32 X = 0; X < CountX; ++X)
        {
            const uint16 Raw = HeightSamples[(CountZ - 1 - Z) * CountX + X];
            WriteF32(Ar, LandscapeDataAccess::GetLocalHeight(Raw));
        }
    }
    for (int32 Z = 0; Z < CountZ - 1; ++Z)
    {
        for (int32 X = 0; X < CountX - 1; ++X)
        {
            const bool bHole = !Visibility.IsEmpty()
                && Visibility[(CountZ - 2 - Z) * CountX + X] >= 128;
            WriteU8(Ar, bHole ? 0xFF : 0);
        }
    }
    // Landscape tiles do not bind to moving visual meshes.
    WriteGuid(Ar, FGuid::NewGuid());
    return !Ar.IsError();
}

bool ExportWorld(UWorld* World, const FString& Filename, uint32& OutCount, FString& OutError)
{
    IFileManager& Files = IFileManager::Get();
    const FString TempFilename = FPaths::CreateTempFilename(
        *FPaths::GetPath(Filename), TEXT("Box3D_"), TEXT(".tmp"));
    TUniquePtr<FArchive> Writer(Files.CreateFileWriter(*TempFilename));
    if (!Writer)
    {
        OutError = TEXT("Could not create the export file. Check the folder and permissions.");
        return false;
    }

    char Magic[4] = {'B', '3', 'D', 'F'};
    Writer->Serialize(Magic, sizeof(Magic));
    WriteU16(*Writer, 3);
    WriteU16(*Writer, 0);
    WriteU32(*Writer, 0); // Filled in after streaming the records.

    bool bValid = true;
    OutCount = 0;
    TSet<FGuid> UsedIds;
    for (TActorIterator<AActor> It(World); It && bValid; ++It)
    {
        if (ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(*It))
        {
            for (ULandscapeComponent* Component : Proxy->LandscapeComponents)
            {
                if (!IsValid(Component) || Component->IsTemplate()) continue;
                if (OutCount == MaxShapeCount)
                {
                    OutError = TEXT("The level exceeds the Box3D export shape limit.");
                    bValid = false;
                    break;
                }
                if (!WriteLandscapeComponent(*Writer, Component, OutError))
                {
                    if (OutError.IsEmpty()) OutError = TEXT("Could not write the Landscape heightfield.");
                    bValid = false;
                    break;
                }
                ++OutCount;
            }
            if (!bValid) break;
        }
        TInlineComponentArray<UBaseBox3DComponent*> Components(*It);
        for (UBaseBox3DComponent* Component : Components)
        {
            if (!IsValid(Component) || Component->IsTemplate())
            {
                continue;
            }
            if (OutCount == MaxShapeCount)
            {
                OutError = TEXT("The level exceeds the Box3D export limit of 1,000,000 shapes.");
                bValid = false;
                break;
            }
            if (!Component->ExportId.IsValid() || UsedIds.Contains(Component->ExportId))
            {
                Component->Modify();
                do
                {
                    Component->ExportId = FGuid::NewGuid();
                }
                while (UsedIds.Contains(Component->ExportId));
                Component->MarkPackageDirty();
            }
            UsedIds.Add(Component->ExportId);
            if (!WriteShape(*Writer, Component, OutError) || Writer->IsError())
            {
                if (OutError.IsEmpty())
                {
                    OutError = TEXT("A write error occurred while exporting shapes.");
                }
                bValid = false;
                break;
            }
            ++OutCount;
        }
    }

    if (bValid)
    {
        if (OutCount == 0)
        {
            OutError = TEXT("No loaded Box3D components or Landscape tiles were found in this level.");
            bValid = false;
        }
    }
    if (bValid)
    {
        Writer->Seek(8);
        WriteU32(*Writer, OutCount);
    }
    const bool bWritten = bValid && !Writer->IsError() && Writer->Close();
    Writer.Reset();
    if (!bWritten)
    {
        Files.Delete(*TempFilename, false, true, true);
        if (OutError.IsEmpty())
        {
            OutError = TEXT("Could not finish writing the export file.");
        }
        return false;
    }

    if (!Files.Move(*Filename, *TempFilename, true, false, false, true))
    {
        Files.Delete(*TempFilename, false, true, true);
        OutError = TEXT("Could not move the completed file to the selected destination.");
        return false;
    }
    return true;
}
} // namespace

void FBox3DFileCreator::ExportBox3DFile()
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::FromString(TEXT("No editor level is open.")),
            FText::FromString(TEXT("Box3D Export")));
        return;
    }

    if (FMessageDialog::Open(EAppMsgType::YesNo,
            FText::FromString(TEXT("Export the loaded Box3D components and Landscape collision in this level?")),
            FText::FromString(TEXT("Box3D Export"))) != EAppReturnType::Yes)
    {
        return;
    }

    IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
    TArray<FString> Filenames;
    if (!Desktop || !Desktop->SaveFileDialog(nullptr, TEXT("Export Box3D Level"),
            FPaths::ProjectSavedDir(), World->GetName() + TEXT(".box3d"),
            TEXT("Box3D files (*.box3d)|*.box3d"), EFileDialogFlags::None, Filenames)
        || Filenames.Num() != 1)
    {
        return;
    }

    FString Filename = Filenames[0];
    if (!Filename.EndsWith(TEXT(".box3d"), ESearchCase::IgnoreCase))
    {
        Filename += TEXT(".box3d");
    }

    uint32 Count = 0;
    FString Error;
    if (!ExportWorld(World, Filename, Count, Error))
    {
        UE_LOG(LogTemp, Error, TEXT("Box3D export failed: %s"), *Error);
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Error),
            FText::FromString(TEXT("Box3D Export Failed")));
        return;
    }

    UE_LOG(LogTemp, Display, TEXT("Box3D exported %u shapes to %s"), Count, *Filename);
    FMessageDialog::Open(EAppMsgType::Ok,
        FText::FromString(FString::Printf(TEXT("Exported %u Box3D shapes. Save the level to keep the component IDs used for mesh syncing."), Count)),
        FText::FromString(TEXT("Box3D Export")));
}
