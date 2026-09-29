#pragma once


#include "CoreMinimal.h"
#include "Box3DCapsule.h"
#include "box3d/math_functions.h"
#include "Components/SceneComponent.h"
#include "Math/Vector.h"
#include "Box3DConversions.h"
#include "BaseBox3DComponent.h"
#include "Box3DCapsuleComponent.generated.h"

UCLASS(ClassGroup=(Box3D), meta=(BlueprintSpawnableComponent))
class BOX3D_API UBox3DCapsuleComponent : public UBaseBox3DComponent
{
	GENERATED_BODY()





public:

	UBox3DCapsuleComponent();

	b3Capsule GenerateCapsuleForMover();


	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Box3D")
	FVector3f centerOne = FVector3f(1.0f, 1.0f, 1.0f);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Box3D")
	FVector3f centerTwo = FVector3f(1.0f, 1.0f, 1.0f);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Box3D")
	float radius = 1.0f;




private:

	EBox3DShapeType ShapeType = EBox3DShapeType::Capsule;


	FShapeData GenerateShape() const override;


};
