#include "BaseBox3DComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"

UBaseBox3DComponent::UBaseBox3DComponent()
{

}

FShapeData UBaseBox3DComponent::GenerateShape() const
{
	return FShapeData();
}

UStaticMeshComponent* UBaseBox3DComponent::ResolveVisualMesh() const
{
    AActor* Owner = GetOwner();
    if (!Owner) return nullptr;

    const bool bExplicit = !VisualMesh.ComponentProperty.IsNone()
        || !VisualMesh.PathToComponent.IsEmpty()
        || VisualMesh.OverrideComponent.IsValid()
        || VisualMesh.OtherActor.IsValid();
    if (bExplicit)
    {
        return Cast<UStaticMeshComponent>(VisualMesh.GetComponent(Owner));
    }

    for (USceneComponent* Parent = GetAttachParent(); Parent; Parent = Parent->GetAttachParent())
    {
        if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Parent)) return Mesh;
    }

    TInlineComponentArray<UStaticMeshComponent*> Meshes(Owner);
    return Meshes.Num() == 1 ? Meshes[0] : nullptr;
}


