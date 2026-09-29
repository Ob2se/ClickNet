#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Box3DMovementComponent.h"
#include "ClickNetRemoteMovementReceiver.h"
#include "Box3DPawn.generated.h"




struct FInputActionValue;

UCLASS(ClassGroup = (Box3D))
class CLICKNET_API ABox3DPawn : public APawn, public IClickNetRemoteMovementReceiver
{
	GENERATED_BODY()



public:

	ABox3DPawn();
	virtual void ApplyRemoteMovementState(const FVector& Velocity, bool bGrounded) override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Box3D")
	TObjectPtr<UBox3DCapsuleComponent> CapsuleComponent;


	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Box3D")
	TObjectPtr<UBox3DMovementComponent> MovementComponent;

	UFUNCTION(BlueprintCallable, Category = "Box3DMovement")
	void MoveForwardBackward(FVector2D value);

	UFUNCTION(BlueprintCallable, Category = "Box3DMovement")
	void MoveLeftRight(FVector2D value);

	UFUNCTION(BlueprintCallable, Category = "Box3DMovement")
	void LookUpDown(FVector2D value);

	UFUNCTION(BlueprintCallable, Category = "Box3DMovement")
	void LookLeftRight(FVector2D value);

	UFUNCTION(BlueprintCallable, Category = "Box3DMovement")
	void Jump(FVector2D value);

protected:
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
};
