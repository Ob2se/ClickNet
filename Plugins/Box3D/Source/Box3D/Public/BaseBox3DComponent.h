#pragma once


#include "CoreMinimal.h"
#include "Box3DCapsule.h"
#include "box3d/math_functions.h"
#include "box3d/box3d.h"
#include "Components/SceneComponent.h"
#include "Engine/EngineTypes.h"
#include "Math/Vector.h"
#include "Box3DConversions.h"
#include "Box3DCollisionProfiles.h"
#include "BaseBox3DComponent.generated.h"

class UStaticMeshComponent;

//the currently supported shapes from box3d, missing cylinders and planes
UENUM()
enum class EBox3DShapeType : uint8
{
	Box,
	Sphere,
	Capsule,
	ConvexHullSimplified,
	ConvexHullComplex
};


//this struct is used to store the shape data for the component, and is used to generate the shape in the box3d physics engine and create its transform in the sim
USTRUCT()
struct FShapeData
{
	GENERATED_BODY()

	EBox3DShapeType ShapeType;


	bool bHasCollision = true;
	int CollisionBit = 0;
	bool bHasPhysics = true;

	float radius = 1.0f;

	b3Vec3 centerOne = b3Vec3(1.0f, 1.0f, 1.0f);
	b3Vec3 centerTwo = b3Vec3(1.0f, 1.0f, 1.0f);

	float x = 1.0f;
	float y = 1.0f;
	float z = 1.0f;

	float size = 1.0f;

	b3Vec3 center = b3Vec3(1.0f, 1.0f, 1.0f);


	b3Vec3 WorldPosition = b3Vec3(1.0f, 1.0f, 1.0f);
	b3Quat WorldRotation = b3Quat(b3Vec3(0.0f, 0.0f, 0.0f), 1.0f);

};


UCLASS(ClassGroup = Box3D)
class BOX3D_API UBaseBox3DComponent : public USceneComponent
{
	GENERATED_BODY()


public:

	UBaseBox3DComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Box3D")
	bool bDrawDebugLines = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Box3D")
	bool bCollisionEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Box3D")
	EBox3DCollisionProfile CollisionProfile = EBox3DCollisionProfile::WorldDynamic;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Box3D")
	bool bEnablePhysics = true;

	// Assigned by the editor exporter and saved with the level. PIE copies the
	// same ID so imported bodies can be matched to their visual components.
	UPROPERTY(VisibleInstanceOnly, Category = "Box3D|Visual")
	FGuid ExportId;

	// Optional when the owner has exactly one Static Mesh component.
	UPROPERTY(EditAnywhere, Category = "Box3D|Visual", meta = (UseComponentPicker, AllowedClasses = "/Script/Engine.StaticMeshComponent"))
	FComponentReference VisualMesh;
	


public:

	

	
	virtual FShapeData GenerateShape() const;
	UStaticMeshComponent* ResolveVisualMesh() const;

	
};
