#pragma once
#include "BaseBox3DComponent.h"
#include "Box3DBoxComponent.generated.h"

UCLASS(ClassGroup = (Box3D), meta = (BlueprintSpawnableComponent))
class BOX3D_API UBox3DBoxComponent : public UBaseBox3DComponent
{
	GENERATED_BODY()



public:
	UBox3DBoxComponent();


	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Box3D")
	bool bCube = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Box3D", meta = (EditCondition = "bCube", EditConditionHides))
	float size = 1.0f;


	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Box3D", meta = (EditCondition = "!bCube", EditConditionHides))
	float x = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Box3D", meta = (EditCondition = "!bCube", EditConditionHides))
	float y = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Box3D", meta = (EditCondition = "!bCube", EditConditionHides))
	float z = 1.0f;



private:
	EBox3DShapeType ShapeType = EBox3DShapeType::Box;

	FShapeData GenerateShape() const override;

};
