#include "Box3DBoxComponent.h"
#include "CoreMinimal.h"
#include "Box3dConversions.h"
#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"

UBox3DBoxComponent::UBox3DBoxComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	
}



FShapeData UBox3DBoxComponent::GenerateShape() const
{

	FVector CompLocation = GetComponentLocation();
	FQuat CompRotation = GetComponentQuat();

	FShapeData ShapeData{};

	switch (bCube)
	{
	case false:
		ShapeData.bHasCollision = bCollisionEnabled;
		ShapeData.bHasPhysics = bEnablePhysics;
		ShapeData.CollisionBit = static_cast<int>(CollisionProfile);
		ShapeData.x = x;
		ShapeData.y = y;
		ShapeData.z = z;
		ShapeData.WorldPosition = FBox3DConversions::FVectorToB3Vec3(CompLocation);
		ShapeData.WorldRotation = FBox3DConversions::FQuatToB3Quat(CompRotation);
		ShapeData.ShapeType = ShapeType;
		break;
	case true:
		ShapeData.bHasCollision = bCollisionEnabled;
		ShapeData.bHasPhysics = bEnablePhysics;
		ShapeData.CollisionBit = static_cast<int>(CollisionProfile);
		ShapeData.size = size;
		ShapeData.x = size;
		ShapeData.y = size;
		ShapeData.z = size;
		ShapeData.WorldPosition = FBox3DConversions::FVectorToB3Vec3(CompLocation);
		ShapeData.WorldRotation = FBox3DConversions::FQuatToB3Quat(CompRotation);
		ShapeData.ShapeType = ShapeType;
		break;
	}

	


	return ShapeData;
}
