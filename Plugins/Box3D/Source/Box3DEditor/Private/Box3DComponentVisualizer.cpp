#include "Box3DComponentVisualizer.h"

#include "BaseBox3DComponent.h"
#include "Box3DBoxComponent.h"
#include "Box3DCapsuleComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "PrimitiveDrawingUtils.h"

void FBox3DComponentVisualizer::DrawVisualization(const UActorComponent* Component,
    const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
    const UBaseBox3DComponent* Shape = Cast<UBaseBox3DComponent>(Component);
    if (!Shape || !PDI)
    {
        return;
    }

    const UWorld* World = Shape->GetWorld();
    // The editor-world copy stays at its authored transform during PIE.
    // Do not draw that copy over the moving simulation.
    if (World && World->WorldType == EWorldType::Editor
        && GEditor && GEditor->IsPlayingSessionInEditor())
    {
        return;
    }
    // Selected components are visualized in both viewports. The level viewport
    // also has the unselected editor-tick preview controlled by this setting.
    if (World && World->WorldType == EWorldType::Editor && !Shape->bDrawDebugLines)
    {
        return;
    }

    constexpr float LineThickness = 2.0f;
    const FLinearColor Color = FLinearColor::Green;

    if (const UBox3DBoxComponent* Box = Cast<UBox3DBoxComponent>(Shape))
    {
        // Box3D's box arguments are half extents, as are these editor values.
        const FVector HalfExtents = Box->bCube
            ? FVector(Box->size, Box->size, Box->size)
            : FVector(Box->x, Box->y, Box->z);
        DrawWireBox(PDI, Box->GetComponentTransform().ToMatrixWithScale(),
            FBox(-HalfExtents, HalfExtents), Color, SDPG_World, LineThickness);
        return;
    }

    if (const UBox3DCapsuleComponent* Capsule = Cast<UBox3DCapsuleComponent>(Shape))
    {
        const FTransform Transform = Capsule->GetComponentTransform();
        const FVector Start = Transform.TransformPosition(FVector(Capsule->centerOne));
        const FVector End = Transform.TransformPosition(FVector(Capsule->centerTwo));
        const FVector Axis = End - Start;
        const FVector Center = (Start + End) * 0.5;
        const double Radius = Capsule->radius * Transform.GetScale3D().GetAbsMax();

        if (Axis.IsNearlyZero())
        {
            DrawWireSphere(PDI, Center, Color, Radius, 16, SDPG_World, LineThickness);
            return;
        }

        const FQuat Rotation = FQuat::FindBetweenNormals(FVector::UpVector, Axis.GetSafeNormal());
        const double HalfHeight = Axis.Size() * 0.5 + Radius;
        DrawWireCapsule(PDI, Center, Rotation.GetAxisX(), Rotation.GetAxisY(),
            Rotation.GetAxisZ(), Color, Radius, HalfHeight, 16, SDPG_World, LineThickness);
    }
}
