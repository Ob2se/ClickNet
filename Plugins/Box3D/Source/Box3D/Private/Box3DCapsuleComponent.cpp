#include "Box3DCapsuleComponent.h"
#include "Box3DConversions.h"
#include "CoreMinimal.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"


UBox3DCapsuleComponent::UBox3DCapsuleComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	
}

b3Capsule UBox3DCapsuleComponent::GenerateCapsuleForMover()
{

	b3Capsule capsule;
	capsule.center1 = FBox3DConversions::FVector3fToB3Vec3(centerOne);
	capsule.center2 = FBox3DConversions::FVector3fToB3Vec3(centerTwo);
	capsule.radius = radius * 0.01f;

	return capsule;
}



FShapeData UBox3DCapsuleComponent::GenerateShape() const
{
	FVector CompLocation = GetComponentLocation();
	FQuat CompRotation = GetComponentQuat();

	FShapeData ShapeData{};
	ShapeData.bHasCollision = bCollisionEnabled;
	ShapeData.bHasPhysics = bEnablePhysics;
	ShapeData.CollisionBit = static_cast<int>(CollisionProfile);
	ShapeData.WorldPosition = FBox3DConversions::FVectorToB3Vec3(CompLocation);
	ShapeData.WorldRotation = FBox3DConversions::FQuatToB3Quat(CompRotation);
	ShapeData.centerOne = FBox3DConversions::FVector3fToB3Vec3(centerOne);
	ShapeData.centerTwo = FBox3DConversions::FVector3fToB3Vec3(centerTwo);
	ShapeData.radius = radius;
	ShapeData.ShapeType = ShapeType;

	
	return ShapeData;
}



